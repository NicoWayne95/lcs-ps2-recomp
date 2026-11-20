#include "../iop_service.h"
#include "../rpc_reply.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"
#include "ps2x/iop/ps2_path.h"

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kCdSearchSid = 0x80000597u;
        constexpr uint32_t kSectorSize = 2048u;
        constexpr uint32_t kMaxDirectoryBytes = 4u * 1024u * 1024u;
        constexpr uint32_t kMaxNameLen = 255u;
        constexpr uint32_t kPathOffset = 0x24u;

        uint32_t le32(const uint8_t* p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        // Mirrors iop_cdvd.cpp normalizedIsoComponent: strip a trailing
        // ';<digits>' (ISO-9660 file version) and upper-case the remainder.
        std::string normalizedIsoComponent(std::string_view component)
        {
            std::string result(component);
            const auto semicolon = result.rfind(';');
            if (semicolon != std::string::npos && semicolon + 1 < result.size())
            {
                bool allDigits = true;
                for (size_t i = semicolon + 1; i < result.size(); i++)
                {
                    if (!std::isdigit(static_cast<unsigned char>(result[i])))
                    {
                        allDigits = false;
                        break;
                    }
                }
                if (allDigits)
                {
                    result.resize(semicolon);
                }
            }
            std::transform(result.begin(), result.end(), result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return result;
        }

        bool readImageRange(IopHost &host, uint64_t &imageHandle, uint64_t offset,
                            std::span<uint8_t> output, const std::string &path)
        {
            if (imageHandle == 0u)
            {
                imageHandle = host.openHostFile(host.hostPath(HostPathKind::CdImage));
            }
            size_t read = 0u;
            return imageHandle != 0u &&
                   host.readHostFile(imageHandle, offset, output.data(), output.size(), read) &&
                   read == output.size();
        }

        // Mirrors iop_cdvd.cpp findImageEntry: PVD at LSN 16, then per-component
        // descent through directory records (no suffix-matching sector walk).
        bool findImageEntry(IopHost &host, uint64_t &imageHandle, std::string_view guestPath,
                            uint64_t &lsn, uint32_t &size)
        {
            if (guestPath.find(':') == std::string_view::npos)
            {
                guestPath = std::string("cdrom0:") + std::string(guestPath);
            }

            const auto parsed = parsePs2Path(std::string(guestPath));
            if (!parsed || parsed.device != Ps2PathDevice::Cdrom)
            {
                return false;
            }

            std::array<uint8_t, kSectorSize> pvd{};
            if (!readImageRange(host, imageHandle, 16u * kSectorSize, pvd, std::string(guestPath)))
            {
                return false;
            }
            if (pvd[0] != 1u || memcmp(pvd.data() + 1, "CD001", 5) != 0 || pvd[6] != 1u || pvd[156] < 34u)
            {
                return false;
            }

            lsn = le32(pvd.data() + 158);
            size = le32(pvd.data() + 166);
            bool directory = true;

            const std::string path = parsed.path;
            size_t begin = 0;
            while (begin < path.size())
            {
                const size_t end = path.find('/', begin);
                const std::string_view wanted = end == std::string::npos
                    ? std::string_view(path).substr(begin)
                    : std::string_view(path).substr(begin, end - begin);
                begin = (end == std::string::npos) ? path.size() : end + 1;

                if (!directory || size == 0u || size > kMaxDirectoryBytes)
                {
                    return false;
                }

                std::vector<uint8_t> records(size);
                if (!readImageRange(host, imageHandle, uint64_t(lsn) * kSectorSize,
                                    records, std::string(guestPath)))
                {
                    return false;
                }

                bool found = false;
                uint64_t nextLsn = 0u;
                uint32_t nextSize = 0u;
                bool nextDirectory = false;
                size_t pos = 0;
                while (pos < records.size())
                {
                    const uint8_t length = records[pos];
                    if (length == 0u)
                    {
                        pos = (pos / kSectorSize + 1u) * kSectorSize;
                        continue;
                    }
                    if (length < 34u || length > records.size() - pos ||
                        (pos % kSectorSize) + length > kSectorSize)
                    {
                        return false;
                    }
                    const uint8_t nameSize = records[32];
                    if (nameSize == 0u || nameSize > length - 33u)
                    {
                        return false;
                    }
                    const std::string_view name(reinterpret_cast<const char *>(records.data() + pos + 33), nameSize);
                    if (normalizedIsoComponent(name) == normalizedIsoComponent(std::string(wanted)))
                    {
                        nextLsn = le32(records.data() + pos + 2);
                        nextSize = le32(records.data() + pos + 10);
                        nextDirectory = (records[pos + 25] & 2u) != 0u;
                        found = true;
                        break;
                    }
                    pos += length;
                }
                if (!found)
                {
                    return false;
                }
                lsn = nextLsn;
                size = nextSize;
                directory = nextDirectory;
            }
            return true;
        }

        bool sendReply(IopHost &host, const RpcRequest &request, uint32_t value, const std::string &detail)
        {
            (void)detail;
            return writeRpcWords(host, request.receive, std::array<uint32_t, 1>{value});
        }
    }

    class CdSearchService final : public IopService
    {
    public:
        explicit CdSearchService(IopHost &host) : m_host(host)
        {
        }

        [[nodiscard]] std::string_view name() const override
        {
            return "cdsearch";
        }

        [[nodiscard]] std::span<const uint32_t> sids() const override
        {
            return m_sids;
        }

        void reset() override
        {
            std::lock_guard lock(m_mutex);
            if (m_imageHandle != 0u)
            {
                m_host.closeHostFile(m_imageHandle);
                m_imageHandle = 0u;
            }
        }

        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
        {
            std::lock_guard lock(m_mutex);
            RpcResult response;
            response.handled = true;
            response.resultAddress = request.receive.address;

            if (request.send.address == 0 || request.send.size < kPathOffset + 4u)
            {
                m_host.log(LogLevel::Warning, std::string("cdsearch: rejected malformed request send=") +
                    std::to_string(request.send.address) + " size=" + std::to_string(request.send.size));
                sendReply(m_host, request, 0u, "bad-request");
                return response;
            }

            // Request layout (game FUN_00365458): NUL-terminated file name at send.address + 0x24.
            char nameBuffer[kMaxNameLen + 1] = {};
            for (uint32_t i = 0u; i <= kMaxNameLen; i++)
            {
                char c = 0;
                if (!m_host.readGuest(request.send.address + kPathOffset + i, &c, 1) || c == '\0')
                {
                    nameBuffer[i] = '\0';
                    break;
                }
                nameBuffer[i] = c;
            }
            const std::string name(nameBuffer);
            if (name.empty() || name.size() > kMaxNameLen)
            {
                m_host.log(LogLevel::Warning, "cdsearch: empty or over-length name");
                sendReply(m_host, request, 0u, "bad-name");
                return response;
            }

            uint64_t lsn = 0u;
            uint32_t size = 0u;
            const bool found = findImageEntry(m_host, m_imageHandle, name, lsn, size);
            if (!found)
            {
                m_host.log(LogLevel::Warning, "cdsearch: lookup failed name=" + name);
                sendReply(m_host, request, 0u, "not-found");
                return response;
            }

            // Result layout: written into the SAME send buffer by the IOP.
            const uint32_t sizeValue = size;
            const uint64_t namePointer = request.send.address + kPathOffset;
            const uint64_t zero1 = 0u;
            const uint64_t zero2 = 0u;
            const bool wroteResult = m_host.writeGuest(request.send.address, &lsn, sizeof(lsn)) &&
                m_host.writeGuest(request.send.address + 8u, &namePointer, sizeof(namePointer)) &&
                m_host.writeGuest(request.send.address + 0x10u, &zero1, sizeof(zero1)) &&
                m_host.writeGuest(request.send.address + 0x18u, &zero2, sizeof(zero2)) &&
                m_host.writeGuest(request.send.address + 0x20u, &sizeValue, sizeof(sizeValue));
            if (!wroteResult)
            {
                m_host.log(LogLevel::Warning, "cdsearch: failed to write result for " + name);
                sendReply(m_host, request, 0u, "write-failed");
                return response;
            }

            m_host.log(LogLevel::Warning, "cdsearch: found " + name + " lbn=" + std::to_string(lsn) + " size=" + std::to_string(size));
            sendReply(m_host, request, 1u, name);
            return response;
        }

    private:
        IopHost &m_host;
        mutable std::mutex m_mutex;
        uint64_t m_imageHandle = 0u;
        const std::array<uint32_t, 1> m_sids = {kCdSearchSid};
    };

    std::unique_ptr<IopService> createCdSearchService(IopHost &host)
    {
        return std::make_unique<CdSearchService>(host);
    }
}
