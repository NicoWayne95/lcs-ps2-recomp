#include "iop_sysclib.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"

#include <cctype>
#include <charconv>
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace ps2x::iop::detail
{
    IopSysclib::IopSysclib(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    bool IopSysclib::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        const auto compare = [&](uint32_t lhs, uint32_t rhs, uint32_t count) -> int32_t
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t left = m_memory.read8(lhs + i);
                const uint8_t right = m_memory.read8(rhs + i);
                if (left != right)
                    return static_cast<int32_t>(left) - static_cast<int32_t>(right);
            }
            return 0;
        };
        const auto copy = [&](uint32_t destination, uint32_t source, uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i)
                m_memory.write8(destination + i, m_memory.read8(source + i));
        };
        const auto appendString = [&](uint32_t destination, uint32_t source, std::optional<uint32_t> maxAppend = std::nullopt)
        {
            uint32_t destinationOffset = 0;
            while (m_memory.read8(destination + destinationOffset) != 0u && destinationOffset < (1u << 20))
                ++destinationOffset;

            uint32_t sourceOffset = 0;
            while (sourceOffset < (1u << 20) && (!maxAppend || sourceOffset < *maxAppend))
            {
                const uint8_t character = m_memory.read8(source + sourceOffset);
                m_memory.write8(destination + destinationOffset + sourceOffset, character);
                ++sourceOffset;
                if (character == 0u)
                    return;
            }
            m_memory.write8(destination + destinationOffset + sourceOffset, 0u);
        };

        switch (ordinal)
        {
        case 4: // setjmp - enough for callers which only test the initial return.
            setV0(0);
            return true;
        case 5: // longjmp, TODO bc w can do it without the BIOS jmp_buf ABI.
            setV0(a1 == 0u ? 1u : a1);
            return true;
        case 6:
            setV0(static_cast<uint32_t>(std::toupper(static_cast<unsigned char>(a0))));
            return true;
        case 7:
            setV0(static_cast<uint32_t>(std::tolower(static_cast<unsigned char>(a0))));
            return true;
        case 8:
        case 9: // ctype table is optional for most IRXs.
            setV0(0);
            return true;
        case 10: // memchr
            for (uint32_t i = 0; i < a2; ++i)
            {
                if (m_memory.read8(a0 + i) == static_cast<uint8_t>(a1))
                {
                    setV0(a0 + i);
                    return true;
                }
            }
            setV0(0);
            return true;
        case 11:
            setV0(static_cast<uint32_t>(compare(a0, a1, a2)));
            return true;
        case 12:
            copy(a0, a1, a2);
            setV0(a0);
            return true;
        case 13:
        {
            std::vector<uint8_t> temporary(a2);
            for (uint32_t i = 0; i < a2; ++i)
                temporary[i] = m_memory.read8(a1 + i);
            (void)m_memory.writeRam(a0, temporary.data(), temporary.size());
            setV0(a0);
            return true;
        }
        case 14:
            for (uint32_t i = 0; i < a2; ++i)
                m_memory.write8(a0 + i, static_cast<uint8_t>(a1));
            setV0(a0);
            return true;
        case 15: // bcmp
            setV0(static_cast<uint32_t>(compare(a0, a1, a2)));
            return true;
        case 16: // bcopy(src,dst,n)
            copy(a1, a0, a2);
            setV0(0);
            return true;
        case 17:
            for (uint32_t i = 0; i < a1; ++i)
                m_memory.write8(a0 + i, 0u);
            setV0(0);
            return true;
        case 18: // prnt
            setV0(0);
            return true;
        case 19: // sprintf: o32 varargs start at a2, then a3, then sp+16.
        case 42: // vsprintf: a2 points to the o32 argument area.
        {
            constexpr size_t limit=4095;
            const std::string format=m_memory.readString(a1,limit+1);
            std::string output;bool valid=m_memory.ownsRamRange(a1,1);
            uint32_t index=2, cursor=a2;
            auto word=[&]() -> uint32_t {
                if(ordinal==19 && index<4)return cpu.gpr[4+index++];
                const uint32_t address=ordinal==42 ? cursor : cpu.gpr[29]+4*index++;
                if(ordinal==42)cursor+=4;
                if(!m_memory.ownsRamRange(address,4)){valid=false;return 0;}
                return m_memory.read32(address);
            };
            auto argument=[&](bool wide) -> uint64_t {
                if(wide){if(ordinal==42)cursor=(cursor+7u)&~7u;else index=(index+1u)&~1u;}
                const uint64_t low=word();return wide ? low | (uint64_t(word())<<32) : low;
            };
            for(size_t pos=0;valid && pos<format.size();++pos)
            {
                if(format[pos]!='%'){output+=format[pos];if(output.size()>limit)valid=false;continue;}
                if(++pos==format.size()){valid=false;break;}
                if(format[pos]=='%'){output+='%';continue;}
                bool left=false,zero=false,plus=false,space=false,alternate=false;
                for(;pos<format.size();++pos){char c=format[pos];
                    if(c=='-')left=true;else if(c=='0')zero=true;else if(c=='+')plus=true;
                    else if(c==' ')space=true;else if(c=='#')alternate=true;else break;}
                int64_t width=0;int precision=-1;
                if(pos<format.size() && format[pos]=='*'){width=int32_t(word());++pos;if(width<0){left=true;width=-width;}}
                else while(pos<format.size() && std::isdigit(static_cast<unsigned char>(format[pos]))){width=width*10+format[pos++]-'0';if(width>limit){valid=false;break;}}
                if(pos<format.size() && format[pos]=='.'){
                    ++pos;precision=0;
                    if(pos<format.size() && format[pos]=='*'){precision=int32_t(word());++pos;}
                    else while(pos<format.size() && std::isdigit(static_cast<unsigned char>(format[pos]))){precision=precision*10+format[pos++]-'0';if(precision>int(limit)){valid=false;break;}}
                }
                bool wide=false;char length=0;
                if(pos<format.size() && (format[pos]=='l'||format[pos]=='h')){length=format[pos++];if(pos<format.size() && format[pos]==length){wide=length=='l';length=length=='h'?'b':length;++pos;}}
                else if(pos<format.size() && (format[pos]=='z'||format[pos]=='t'))++pos;
                if(!valid || pos>=format.size() || width>limit || precision>int(limit)){valid=false;break;}
                const char spec=format[pos];std::string field,prefix;bool numeric=false;
                if(spec=='s'){
                    const uint32_t ptr=word();
                    if(ptr && !m_memory.ownsRamRange(ptr,1)){valid=false;break;}
                    field=ptr ? m_memory.readString(ptr,precision<0 ? limit+1 : size_t(precision)) : "(null)";
                    if(precision>=0 && field.size()>size_t(precision))field.resize(precision);
                } else if(spec=='c')field=char(word());
                else if(spec=='d'||spec=='i'||spec=='u'||spec=='x'||spec=='X'||spec=='o'||spec=='p'){
                    numeric=true;uint64_t value=argument(wide);
                    const bool signedValue=spec=='d'||spec=='i';
                    if(!wide){
                        if(length=='h')value=signedValue ? uint64_t(int64_t(int16_t(value))) : uint16_t(value);
                        else if(length=='b')value=signedValue ? uint64_t(int64_t(int8_t(value))) : uint8_t(value);
                        else if(signedValue)value=uint64_t(int64_t(int32_t(value)));
                    }
                    if(signedValue && int64_t(value)<0){prefix="-";value=uint64_t(0)-value;}
                    else if(signedValue && (plus||space))prefix=plus?"+":" ";
                    const int base=(spec=='x'||spec=='X'||spec=='p')?16:spec=='o'?8:10;
                    char digits[65];auto converted=std::to_chars(digits,digits+sizeof(digits),value,base);
                    field.assign(digits,converted.ptr);
                    if(value==0 && precision==0)field.clear();
                    if(spec=='X')for(char& c:field)c=char(std::toupper(static_cast<unsigned char>(c)));
                    if(precision>int(field.size()))field.insert(0,size_t(precision)-field.size(),'0');
                    if(spec=='p' || (alternate && value!=0 && base==16))prefix+=spec=='X'?"0X":"0x";
                    if(alternate && base==8 && (field.empty()||field[0]!='0'))prefix+='0';
                } else {valid=false;break;} // Unsupported conversions must not fake formatted output.
                if(!valid)break;
                const size_t used=prefix.size()+field.size();
                const size_t padding=width>int64_t(used)?size_t(width)-used:0;
                if(left)output+=prefix+field+std::string(padding,' ');
                else if(zero && numeric && precision<0)output+=prefix+std::string(padding,'0')+field;
                else output+=std::string(padding,' ')+prefix+field;
                if(output.size()>limit)valid=false;
            }
            const uint32_t destination=IopMemory::physicalAddress(a0);
            const bool outputFits=destination<=IopMemory::RamSize && output.size()+1<=IopMemory::RamSize-destination;
            if(!valid || output.size()>limit || !outputFits){
                if(destination<IopMemory::RamSize)m_memory.write8(a0,0);
                setV0(0xffffffffu);return true;
            }
            (void)m_memory.writeRam(a0,output.c_str(),output.size()+1);
            setV0(static_cast<uint32_t>(output.size()));return true;
        }
        case 20:
            appendString(a0, a1);
            setV0(a0);
            return true;
        case 21: // strchr
        case 25: // index
        {
            const uint8_t needle = static_cast<uint8_t>(a1);
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const uint8_t character = m_memory.read8(a0 + i);
                if (character == needle)
                {
                    setV0(a0 + i);
                    return true;
                }
                if (character == 0u)
                    break;
            }
            setV0(0);
            return true;
        }
        case 22: // strcmp
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const uint8_t left = m_memory.read8(a0 + i);
                const uint8_t right = m_memory.read8(a1 + i);
                if (left != right)
                {
                    setV0(static_cast<uint32_t>(static_cast<int32_t>(left) - static_cast<int32_t>(right)));
                    return true;
                }
                if (left == 0u)
                    break;
            }
            setV0(0);
            return true;
        case 23: // strcpy
        {
            uint32_t i = 0;
            for (;; ++i)
            {
                const uint8_t character = m_memory.read8(a1 + i);
                m_memory.write8(a0 + i, character);
                if (character == 0u)
                    break;
            }
            setV0(a0);
            return true;
        }
        case 24: // strcspn
        {
            const std::string reject = m_memory.readString(a1, 4096u);
            uint32_t count = 0;
            for (; count < (1u << 20); ++count)
            {
                const char character = static_cast<char>(m_memory.read8(a0 + count));
                if (character == 0 || reject.find(character) != std::string::npos)
                    break;
            }
            setV0(count);
            return true;
        }
        case 26: // rindex
        case 32: // strrchr
        {
            const uint8_t needle = static_cast<uint8_t>(a1);
            uint32_t found = 0u;
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const uint8_t character = m_memory.read8(a0 + i);
                if (character == needle)
                    found = a0 + i;
                if (character == 0u)
                    break;
            }
            setV0(found);
            return true;
        }
        case 27:
            setV0(static_cast<uint32_t>(m_memory.readString(a0, 1u << 20).size()));
            return true;
        case 28:
            appendString(a0, a1, a2);
            setV0(a0);
            return true;
        case 29: // strncmp
            for (uint32_t i = 0; i < a2; ++i)
            {
                const uint8_t left = m_memory.read8(a0 + i);
                const uint8_t right = m_memory.read8(a1 + i);
                if (left != right)
                {
                    setV0(static_cast<uint32_t>(static_cast<int32_t>(left) - static_cast<int32_t>(right)));
                    return true;
                }
                if (left == 0u)
                    break;
            }
            setV0(0);
            return true;
        case 30: // strncpy
        {
            bool ended = false;
            for (uint32_t i = 0; i < a2; ++i)
            {
                const uint8_t character = ended ? 0u : m_memory.read8(a1 + i);
                if (character == 0u)
                    ended = true;
                m_memory.write8(a0 + i, character);
            }
            setV0(a0);
            return true;
        }
        case 31: // strpbrk
        {
            const std::string accept = m_memory.readString(a1, 4096u);
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const char character = static_cast<char>(m_memory.read8(a0 + i));
                if (character == 0)
                    break;
                if (accept.find(character) != std::string::npos)
                {
                    setV0(a0 + i);
                    return true;
                }
            }
            setV0(0);
            return true;
        }
        case 33: // strspn
        {
            const std::string accept = m_memory.readString(a1, 4096u);
            uint32_t count = 0;
            for (; count < (1u << 20); ++count)
            {
                const char character = static_cast<char>(m_memory.read8(a0 + count));
                if (character == 0 || accept.find(character) == std::string::npos)
                    break;
            }
            setV0(count);
            return true;
        }
        case 34: // strstr
        {
            const std::string needle = m_memory.readString(a1, 4096u);
            if (needle.empty())
            {
                setV0(a0);
                return true;
            }
            const std::string haystack = m_memory.readString(a0, 1u << 20);
            const size_t position = haystack.find(needle);
            setV0(position == std::string::npos
                      ? 0u
                      : a0 + static_cast<uint32_t>(position));
            return true;
        }
        case 35: // strtok state is intentionally not shared across modules yet.
            setV0(0);
            return true;
        case 36:
        case 38: // strtol / strtoul
        {
            const std::string value = m_memory.readString(a0, 4096u);
            char *end = nullptr;
            const int base = static_cast<int>(a2);
            const unsigned long parsed = ordinal == 36
                                             ? static_cast<unsigned long>(std::strtol(value.c_str(), &end, base))
                                             : std::strtoul(value.c_str(), &end, base);
            if (a1 != 0u)
            {
                m_memory.write32(a1, a0 + static_cast<uint32_t>(end - value.c_str()));
            }
            setV0(static_cast<uint32_t>(parsed));
            return true;
        }
        case 37: // atob
            setV0(0);
            return true;
        case 40: // _wmemcopy, count is 32-bit words
            for (uint32_t i = 0; i < a2; ++i)
                m_memory.write32(a0 + i * 4u, m_memory.read32(a1 + i * 4u));
            setV0(a0);
            return true;
        case 41:
            for (uint32_t i = 0; i < a2; ++i)
                m_memory.write32(a0 + i * 4u, a1);
            setV0(a0);
            return true;
        case 43:
            setV0(0);
            return true;
        default:
            return false;
        }
    }
}
