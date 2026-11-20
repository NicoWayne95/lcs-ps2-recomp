#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"
#include "emulator/imports/iop_cdvd.h"
#include "emulator/imports/iop_imports.h"
#include "emulator/imports/iop_loadcore.h"
#include "emulator/imports/iop_timrman.h"
#include "emulator/imports/iop_sysclib.h"
#include "emulator/services/iop_rpc.h"
#include "ps2x/iop/iop_host.h"

#include <cstdint>
#include <chrono>
#include <cstring>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace ps2x::iop;
    using namespace ps2x::iop::detail;

    constexpr uint32_t kExportMagic = 0x41C00000u;
    constexpr int32_t kLibraryNotFound = -213;
    constexpr int32_t kIllegalLibrary = -214;

    class NullHost : public IopHost
    {
    public:
        bool readGuest(uint32_t, void *, size_t) const override { return false; }
        bool writeGuest(uint32_t, const void *, size_t) override { return false; }
        bool zeroGuest(uint32_t, size_t) override { return false; }
        bool normalizeGuestAddress(uint32_t, uint32_t &) const override { return false; }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return 0u; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel, std::string_view) override {}
    };

    class CdRootHost final : public NullHost
    {
    public:
        explicit CdRootHost(std::filesystem::path rootPath)
            : root(std::move(rootPath))
        {
        }

        std::string hostPath(HostPathKind kind) const override
        {
            return kind == HostPathKind::CdRoot ? root.string() : std::string{};
        }

    private:
        std::filesystem::path root;
    };

    class RecordingExecutor final : public IopGuestExecutor
    {
    public:
        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t,
                                      uint32_t,
                                      uint32_t,
                                      uint32_t gp) override
        {
            ++calls;
            lastAddress = address;
            lastArgument = a0;
            lastGp = gp;
            return callbackResult;
        }

        uint32_t callbackResult = 0u;
        uint32_t calls = 0u;
        uint32_t lastAddress = 0u;
        uint32_t lastArgument = 0u;
        uint32_t lastGp = 0u;
    };

    bool expect(bool condition, std::string_view message)
    {
        if (condition)
            return true;
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }

    bool testLoadcoreRebootLibraryMode()
    {
        IopMemory memory;
        IopImportRegistry imports(memory);
        IopLoadcore loadcore(memory, imports);

        IopCpuState cpu{};
        cpu.gpr[4] = 0u;
        cpu.gpr[5] = 2u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) == kIllegalLibrary,
                    "loadcore:27 did not reject a null export table"))
            return false;

        constexpr uint32_t table = 0x1000u;
        memory.write32(table, kExportMagic);
        memory.write16(table + 8u, 0x0101u);
        memory.write16(table + 10u, 0x1234u);
        const char name[8] = {'t', 'e', 's', 't', 'l', 'i', 'b', '\0'};
        (void)memory.writeRam(table + 12u, name, sizeof(name));
        memory.write32(table + 20u, 0u);

        cpu = {};
        cpu.gpr[4] = table;
        cpu.gpr[5] = 2u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 rejected a valid export table") ||
            !expect(cpu.gpr[2] == 0u, "loadcore:27 returned an error for a valid export table") ||
            !expect(memory.read16(table + 10u) == 0x1232u,
                    "loadcore:27 did not replace only export mode bits 1 and 2"))
            return false;

        constexpr uint32_t invalidTable = 0x1100u;
        memory.write32(invalidTable, 0xDEADBEEFu);
        cpu = {};
        cpu.gpr[4] = invalidTable;
        cpu.gpr[5] = 6u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 did not consume an invalid-table call") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) == kLibraryNotFound,
                    "loadcore:27 returned the wrong invalid-table error"))
            return false;

        if (!expect(imports.registerExportTable(table), "test export table did not register"))
            return false;
        memory.write32(table, 0u);
        cpu = {};
        cpu.gpr[4] = table;
        cpu.gpr[5] = 6u;
        return expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 rejected a registered table") &&
               expect(cpu.gpr[2] == 0u, "loadcore:27 returned an error for a registered table") &&
               expect(memory.read16(table + 10u) == 0x1236u,
                      "loadcore:27 did not update a registered table's mode");
    }

    bool pollEvent(IopKernel &kernel, int eventId, uint32_t bits, uint32_t resultAddress, int32_t expected)
    {
        IopCpuState cpu{};
        cpu.gpr[4] = static_cast<uint32_t>(eventId);
        cpu.gpr[5] = bits;
        cpu.gpr[6] = 0u; // WEF_AND
        cpu.gpr[7] = resultAddress;
        return expect(kernel.dispatchEventImport(11u, cpu), "PollEventFlag was not handled") &&
               expect(static_cast<int32_t>(cpu.gpr[2]) == expected, "PollEventFlag returned an unexpected result");
    }

    bool testCdvdSpecialControl()
    {
        NullHost host;
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();

        constexpr uint32_t param = 0x2000u;
        constexpr uint32_t eventResult = 0x2010u;
        IopCpuState cpu{};
        cpu.gpr[4] = static_cast<uint32_t>(-11); // sceCdSC: return cdvdman interrupt event flag
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "cdvdman:50 was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) > 0, "sceCdSC(-11) did not return a valid event flag"))
            return false;
        const int eventId = static_cast<int>(cpu.gpr[2]);

        if (!pollEvent(kernel, eventId, 0x29u, eventResult, 0) ||
            !expect(memory.read32(eventResult) == 0x29u, "cdvdman event flag did not start with bits 0x29"))
            return false;

        IopCpuState clear{};
        clear.gpr[4] = static_cast<uint32_t>(eventId);
        clear.gpr[5] = ~0x29u;
        if (!expect(kernel.dispatchEventImport(8u, clear), "ClearEventFlag was not handled") ||
            !pollEvent(kernel, eventId, 0x29u, eventResult, -418))
            return false;

        cpu = {};
        cpu.gpr[4] = 0x12345u;
        if (!expect(cdvd.dispatchImport(7u, cpu), "sceCdSeek was not handled") ||
            !pollEvent(kernel, eventId, 0x29u, eventResult, 0))
            return false;

        memory.write8(param, 0x30u);
        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-2);
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(-2) was not handled") ||
            !expect(cpu.gpr[2] == 0x30u, "sceCdSC(-2) did not store the low-byte error"))
            return false;

        memory.write32(param, 0u);
        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-1);
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(-1) was not handled") ||
            !expect(cpu.gpr[2] == 0u, "sceCdSC(-1) returned the wrong initial stream state") ||
            !expect(memory.read32(param) == 0x30u, "sceCdSC(-1) did not publish the last error"))
            return false;

        cpu = {};
        cpu.gpr[4] = 2u;
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(2) was not handled") ||
            !expect(cpu.gpr[2] == 2u, "sceCdSC(2) did not update the stream state"))
            return false;

        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-1);
        cpu.gpr[5] = param;
        return expect(cdvd.dispatchImport(50u, cpu), "second sceCdSC(-1) was not handled") &&
               expect(cpu.gpr[2] == 2u, "sceCdSC(-1) did not preserve the stream state");
    }

    bool testCdvdSearchFile()
    {
        const auto suffix = std::to_string(
            static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / ("ps2x-iop-cdvd-search-" + suffix);
        const std::filesystem::path movieDirectory = root / "MOVIE";
        const std::filesystem::path moviePath = movieDirectory / "OPENING.PSS";
        std::error_code error;
        std::filesystem::create_directories(movieDirectory, error);
        if (!expect(!error, "could not create the temporary CD root"))
            return false;
        {
            std::ofstream movie(moviePath, std::ios::binary);
            movie.write("PSS!", 4);
        }

        CdRootHost host(root);
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();

        constexpr uint32_t resultAddress = 0x2400u;
        constexpr uint32_t pathAddress = 0x2480u;
        const char path[] = "cdrom0:\\movie\\opening.pss;1";
        (void)memory.writeRam(pathAddress, path, sizeof(path));

        IopCpuState cpu{};
        cpu.gpr[4] = resultAddress;
        cpu.gpr[5] = pathAddress;
        const bool handled = cdvd.dispatchImport(10u, cpu);
        bool passed =
            expect(handled, "cdvdman:10 was not handled") &&
            expect(cpu.gpr[2] == 1u, "sceCdSearchFile did not find a case-insensitive ISO path") &&
            expect(memory.read32(resultAddress) >= 20u, "sceCdSearchFile returned an invalid LSN") &&
            expect(memory.read32(resultAddress + 4u) == 4u, "sceCdSearchFile returned the wrong size") &&
            expect(memory.readString(resultAddress + 8u, 16u) == "OPENING.PSS",
                   "sceCdSearchFile returned the wrong file name");

        const char rootPath[] = "\\movie\\opening.pss;1";
        (void)memory.writeRam(pathAddress,rootPath,sizeof(rootPath));
        cpu.gpr[4]=resultAddress; cpu.gpr[5]=pathAddress; cpu.gpr[6]=0u;
        passed = expect(cdvd.dispatchImport(84u,cpu) && cpu.gpr[2]==1u,"Layer-0 ISO-root search failed") && passed;
        cpu.gpr[6]=1u;
        passed = expect(cdvd.dispatchImport(84u,cpu) && cpu.gpr[2]==0u,"Unmodelled layer must not alias layer 0") && passed;
        std::filesystem::remove_all(root, error);
        return passed;
    }

    bool testCdvdIsoSearchAndRead()
    {
        class ImageHost final : public NullHost {
        public:
            std::vector<uint8_t> image=std::vector<uint8_t>(24u*2048u);
            bool fail=false;
            std::string hostPath(HostPathKind kind) const override { return kind==HostPathKind::CdImage ? "memory.iso" : ""; }
            uint64_t openHostFile(std::string_view) override { return 1u; }
            bool readHostFile(uint64_t, uint64_t offset, void* dest, size_t size, size_t& read) override {
                read=0;
                if (fail || offset>image.size() || size>image.size()-offset) return false;
                std::memcpy(dest,image.data()+offset,size);read=size;return true;
            }
        } host;
        auto le32=[&](size_t off,uint32_t n) { for(unsigned i=0;i<4;++i) host.image[off+i]=uint8_t(n>>(8*i)); };
        auto record=[&](size_t off,uint32_t lsn,uint32_t size,std::string_view name,bool dir) {
            host.image[off]=uint8_t(33+name.size()+((name.size()&1)==0));
            le32(off+2,lsn);le32(off+10,size);host.image[off+25]=dir ? 2u : 0u;
            host.image[off+32]=uint8_t(name.size());std::memcpy(host.image.data()+off+33,name.data(),name.size());
        };
        const size_t pvd=16u*2048u;
        host.image[pvd]=1;std::memcpy(host.image.data()+pvd+1,"CD001",5);host.image[pvd+6]=1;
        record(pvd+156,20,2048,std::string_view("\0",1),true);
        record(20u*2048u,21,2048,"MOVIE",true);
        record(21u*2048u,22,4,"OPENING.PSS;1",false);
        std::memcpy(host.image.data()+22u*2048u,"DATA",4);
        IopMemory memory; IopKernel kernel(memory); IopCdvd cdvd(host,memory,kernel);
        const char path[]="\\movie\\opening.pss;1";
        (void)memory.writeRam(0x2400,path,sizeof(path));
        IopCpuState cpu{};cpu.gpr[4]=0x2500;cpu.gpr[5]=0x2400;cpu.gpr[6]=0;
        if (!expect(cdvd.dispatchImport(84,cpu) && cpu.gpr[2]==1 && memory.read32(0x2500)==22 && memory.read32(0x2504)==4,
                    "ISO search must preserve physical file extent and size")) return false;
        cpu={};cpu.gpr[4]=0x12340;cpu.gpr[28]=0x56780;
        if(!expect(cdvd.dispatchImport(37,cpu),"CDVD callback registration failed"))return false;
        const uint32_t buffer=memory.allocate(2048,64);
        cpu={};cpu.gpr[4]=22;cpu.gpr[5]=1;cpu.gpr[6]=buffer;
        if (!expect(cdvd.dispatchImport(6,cpu) && cpu.gpr[2]==1 && memory.read32(buffer)==0x41544144,
                    "ISO sector read must return actual DATA bytes")) return false;
        // Accepted reads remain in flight until their transfer budget expires.
        cpu={};cpu.gpr[4]=1;
        if(!expect(cdvd.dispatchImport(11,cpu) && cpu.gpr[2]==1,"Sync poll completed an in-flight read"))return false;
        cpu={};cpu.gpr[4]=22;cpu.gpr[5]=1;cpu.gpr[6]=buffer;
        if(!expect(cdvd.dispatchImport(6,cpu) && cpu.gpr[2]==0,"Second CDVD read overwrote an in-flight operation"))return false;
        if(!expect(!cdvd.takeCompletionCallback().has_value(),"Read callback was available before completion"))return false;
        const uint64_t deadline=cdvd.nextCompletionCycle(UINT64_MAX);
        if(!expect(deadline!=UINT64_MAX && deadline>1,"Accepted read has no completion deadline"))return false;
        cdvd.advanceTo(deadline-1);
        cpu={};cpu.gpr[4]=1;
        if(!expect(cdvd.dispatchImport(11,cpu) && cpu.gpr[2]==1,"Read finished before its deadline"))return false;
        cdvd.advanceTo(deadline);
        cpu={};cpu.gpr[4]=1;
        if(!expect(cdvd.dispatchImport(11,cpu) && cpu.gpr[2]==0,"Read remained busy after completion"))return false;
        const auto completed=cdvd.takeCompletionCallback();
        if(!expect(completed && completed->address==0x12340 && completed->gp==0x56780 && completed->reason==1,
                   "Read completion lost callback identity or reason"))return false;
        if(!expect(!cdvd.takeCompletionCallback(),"Read callback delivered twice"))return false;
        cpu={};cpu.gpr[4]=23;cpu.gpr[5]=1;cpu.gpr[6]=buffer;
        if(!expect(cdvd.dispatchImport(6,cpu) && cpu.gpr[2]==1,"Sequential read was not accepted"))return false;
        const uint64_t sequentialDeadline=cdvd.nextCompletionCycle(UINT64_MAX);
        if(!expect(sequentialDeadline>deadline && sequentialDeadline-deadline<deadline,
                   "Sequential read incorrectly incurred the initial positioning budget"))return false;
        cdvd.advanceTo(sequentialDeadline);(void)cdvd.takeCompletionCallback();
        const auto openDir=[&](const char* path) {
            memory.writeRam(0x2400,path,std::strlen(path)+1);
            cpu={};cpu.gpr[4]=0x2400;
            return cdvd.dispatchIomanImport(13,cpu)?cpu.gpr[2]:0xffffffffu;
        };
        const uint32_t root=openDir("cdrom0:\\");
        if(!expect(root>=0x40000000u && root<0x40010000u,"ISO root Dopen failed"))return false;
        cpu.gpr[4]=root;cpu.gpr[5]=0x3000;
        memory.write8(0x3000+324,0x5a);
        if(!expect(cdvd.dispatchIomanImport(15,cpu) && cpu.gpr[2]==1 && memory.read32(0x3000)==0x1000 &&
            memory.read32(0x3008)==2048 && memory.readString(0x3040,256)=="MOVIE" && memory.read8(0x3000+324)==0x5a,
            "Retail IOMAN Dread must preserve type/size/name offsets and output bounds"))return false;
        if(!expect(cdvd.dispatchIomanImport(15,cpu) && cpu.gpr[2]==0,"ISO directory end must return zero"))return false;
        const uint32_t movie=openDir("cdrom0:\\movie");
        cpu.gpr[4]=movie;cpu.gpr[5]=IopMemory::RamSize-8;
        if(!expect(cdvd.dispatchIomanImport(15,cpu) && cpu.gpr[2]==0xffffffffu,"Invalid Dread output must fail"))return false;
        cpu.gpr[5]=0x3000;
        if(!expect(cdvd.dispatchIomanImport(15,cpu) && cpu.gpr[2]==1 && memory.read32(0x3000)==0x2000 &&
            memory.read32(0x3008)==4 && memory.readString(0x3040,256)=="OPENING.PSS;1",
            "Nested ISO Dread must return actual file metadata without consuming failed reads"))return false;
        if(!expect(cdvd.dispatchIomanImport(14,cpu) && cpu.gpr[2]==0,"Dclose failed"))return false;
        if(!expect(cdvd.dispatchIomanImport(15,cpu) && cpu.gpr[2]==0xffffffffu,"Closed directory must stay invalid"))return false;
        if(!expect(openDir("cdrom0:\\movie\\opening.pss;1")==0xffffffffu,"Dopen must reject files"))return false;
        cpu.gpr[4]=0x2400;memory.writeRam(0x2400,"mc0:/",6);
        if(!expect(!cdvd.dispatchIomanImport(13,cpu),"CD directory dispatch must leave other devices to IOMAN"))return false;
        cpu={};cpu.gpr[4]=22;cpu.gpr[5]=1;cpu.gpr[6]=buffer;
        host.fail=true;
        if (!expect(cdvd.dispatchImport(6,cpu) && cpu.gpr[2]==0,"Failed ISO read must not become virtual success")) return false;
        if(!expect(cdvd.nextCompletionCycle(UINT64_MAX)==UINT64_MAX,"Rejected read left a pending completion"))return false;
        host.fail=false;
        if(!expect(cdvd.dispatchImport(6,cpu) && cpu.gpr[2]==1,"Read after failure was not accepted"))return false;
        cdvd.reset();cdvd.advanceTo(UINT64_MAX-1);
        cpu={};cpu.gpr[4]=1;
        if(!expect(cdvd.dispatchImport(11,cpu) && cpu.gpr[2]==0 && !cdvd.takeCompletionCallback() &&
                   cdvd.nextCompletionCycle(UINT64_MAX)==UINT64_MAX,"Reset retained an in-flight read or callback"))return false;
        host.image[21u*2048u]=2;
        cpu.gpr[4]=0x2500;cpu.gpr[5]=0x2400;cpu.gpr[6]=0;
        return expect(cdvd.dispatchImport(84,cpu) && cpu.gpr[2]==0,"Malformed ISO directory record must fail safely");
    }

    bool testCdvdTrayAndReadBuffer()
    {
        NullHost host;
        IopMemory memory;
        IopKernel kernel(memory);
        IopCdvd cdvd(host, memory, kernel);
        IopCpuState cpu{};
        cpu.gpr[4]=2u; cpu.gpr[5]=0x2400u;
        if (!expect(cdvd.dispatchImport(14u,cpu) && cpu.gpr[2]==1u && memory.read32(0x2400u)==1u,
                    "Tray check must return and consume initial media change")) return false;
        if (!expect(cdvd.dispatchImport(14u,cpu) && memory.read32(0x2400u)==0u,"Tray check did not consume change")) return false;
        cpu.gpr[4]=0u;
        if (!expect(cdvd.dispatchImport(14u,cpu) && cpu.gpr[2]==1u,"Tray open failed")) return false;
        if (!expect(cdvd.dispatchImport(12u,cpu) && cpu.gpr[2]==0u,"Open tray must report no disc")) return false;
        if (!expect(cdvd.dispatchImport(28u,cpu) && cpu.gpr[2]==1u,"Open tray must report shell-open status")) return false;
        cpu.gpr[4]=1u;
        if (!expect(cdvd.dispatchImport(14u,cpu) && cpu.gpr[2]==1u,"Tray close failed")) return false;
        if (!expect(cdvd.dispatchImport(12u,cpu) && cpu.gpr[2]==0x14u,"Closed tray must expose PS2 DVD")) return false;
        cpu.gpr[4]=2u; cpu.gpr[5]=0u;
        if (!expect(cdvd.dispatchImport(14u,cpu) && cpu.gpr[2]==0u,"Null tray-check output must fail")) return false;
        cpu.gpr[5]=0x2400u;
        if (!expect(cdvd.dispatchImport(14u,cpu) && memory.read32(0x2400u)==1u,"Failed output must preserve change")) return false;
        cpu.gpr[4]=99u;
        if (!expect(cdvd.dispatchImport(14u,cpu) && cpu.gpr[2]==0u,"Unknown tray mode must fail")) return false;
        if (!expect(cdvd.dispatchImport(47u,cpu),"FSV read buffer import missing")) return false;
        const uint32_t buffer=cpu.gpr[2];
        if (!expect(buffer!=0u && (buffer&63u)==0u && memory.ownsRamRange(buffer,42128u),"FSV buffer must be aligned and fully owned")) return false;
        return expect(cdvd.dispatchImport(47u,cpu) && cpu.gpr[2]==buffer,"FSV buffer address must remain stable");
    }

    bool testSysclibFormatting()
    {
        IopMemory memory;IopSysclib sysclib(memory);IopCpuState cpu{};
        auto text=[&](uint32_t address,const char* value){return memory.writeRam(address,value,std::strlen(value)+1);};
        text(0x2400,"\\AUDIO\\SET%s_PS.SDT;1");text(0x2500,"0\\SFX0");
        cpu.gpr[4]=0x2600;cpu.gpr[5]=0x2400;cpu.gpr[6]=0x2500;
        if(!expect(sysclib.dispatchImport(19,cpu) && memory.readString(0x2600,128)=="\\AUDIO\\SET0\\SFX0_PS.SDT;1" &&
                   cpu.gpr[2]==std::strlen("\\AUDIO\\SET0\\SFX0_PS.SDT;1"),"sprintf did not build the original audio catalog path"))return false;
        text(0x2400,"%s:%d:%04X:%%");text(0x2500,"track");
        cpu={};cpu.gpr[4]=0x2600;cpu.gpr[5]=0x2400;cpu.gpr[6]=0x2500;cpu.gpr[7]=uint32_t(-7);cpu.gpr[29]=0x3000;
        memory.write32(0x3010,0x2a);
        if(!expect(sysclib.dispatchImport(19,cpu) && memory.readString(0x2600,128)=="track:-7:002A:%","sprintf o32 register/stack arguments are incorrect"))return false;
        text(0x2400,"%.*s/%u");memory.write32(0x2800,3);memory.write32(0x2804,0x2500);memory.write32(0x2808,42);
        cpu={};cpu.gpr[4]=0x2600;cpu.gpr[5]=0x2400;cpu.gpr[6]=0x2800;
        if(!expect(sysclib.dispatchImport(42,cpu) && memory.readString(0x2600,128)=="tra/42","vsprintf va_list/precision handling is incorrect"))return false;
        text(0x2400,"%u %lld");memory.write32(0x3010,0xffffffff);memory.write32(0x3014,0xffffffff);
        cpu={};cpu.gpr[4]=0x2600;cpu.gpr[5]=0x2400;cpu.gpr[6]=5;cpu.gpr[7]=0xfeed;cpu.gpr[29]=0x3000;
        if(!expect(sysclib.dispatchImport(19,cpu) && memory.readString(0x2600,128)=="5 -1","sprintf did not align a 64-bit argument after a single word"))return false;
        text(0x2400,"%5000s");cpu.gpr[6]=0x2500;
        if(!expect(sysclib.dispatchImport(19,cpu) && cpu.gpr[2]==0xffffffff && memory.read8(0x2600)==0,"Unbounded sprintf width was not rejected"))return false;
        text(0x2400,"%f");
        return expect(sysclib.dispatchImport(19,cpu) && cpu.gpr[2]==0xffffffff,"Unsupported format conversion produced fake output");
    }

    bool testTimrmanPeriodicCallback()
    {
        IopTimrman timrman;
        timrman.reset();
        IopCpuState cpu{};

        cpu.gpr[4] = 1u; // SYSCLK
        cpu.gpr[5] = 32u;
        cpu.gpr[6] = 1u;
        if (!expect(timrman.dispatchImport(4u, cpu, 100u), "AllocHardTimer was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) > 0, "AllocHardTimer did not allocate a 32-bit timer"))
            return false;
        const uint32_t timerId = cpu.gpr[2];

        cpu = {};
        cpu.gpr[4] = timerId;
        cpu.gpr[5] = 100u;
        cpu.gpr[6] = 0x12340u;
        cpu.gpr[7] = 0x45670u;
        cpu.gpr[28] = 0x89AB0u;
        if (!expect(timrman.dispatchImport(20u, cpu, 100u), "SetTimerHandler was not handled") ||
            !expect(cpu.gpr[2] == 0u, "SetTimerHandler failed"))
            return false;

        cpu = {};
        cpu.gpr[4] = timerId;
        cpu.gpr[5] = 1u;
        cpu.gpr[6] = 0u;
        cpu.gpr[7] = 1u;
        if (!expect(timrman.dispatchImport(22u, cpu, 100u), "SetupHardTimer was not handled") ||
            !expect(cpu.gpr[2] == 0u, "SetupHardTimer failed"))
            return false;

        cpu = {};
        cpu.gpr[4] = timerId;
        if (!expect(timrman.dispatchImport(23u, cpu, 100u), "StartHardTimer was not handled") ||
            !expect(cpu.gpr[2] == 0u, "StartHardTimer failed") ||
            !expect(timrman.nextEventCycle(1000u) == 200u, "timer compare was scheduled at the wrong cycle"))
            return false;

        RecordingExecutor executor;
        executor.callbackResult = 100u;
        timrman.serviceDue(199u, executor);
        if (!expect(executor.calls == 0u, "timer callback ran too early"))
            return false;
        timrman.serviceDue(200u, executor);
        return expect(executor.calls == 1u, "timer callback did not run") &&
               expect(executor.lastAddress == 0x12340u, "timer called the wrong handler") &&
               expect(executor.lastArgument == 0x45670u, "timer passed the wrong common argument") &&
               expect(executor.lastGp == 0x89AB0u, "timer callback lost the registering module GP") &&
               expect(timrman.nextEventCycle(1000u) == 300u, "timer callback return did not rearm compare");
    }
}

int main()
{
    if (!testCdvdIsoSearchAndRead() || !testCdvdTrayAndReadBuffer() || !testLoadcoreRebootLibraryMode() || !testCdvdSpecialControl() || !testCdvdSearchFile() ||
        !testSysclibFormatting() || !testTimrmanPeriodicCallback())
        return 1;
    std::cout << "ps2xIOP import tests passed\n";
    return 0;
}
