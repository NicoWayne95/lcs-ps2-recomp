#include "iop_sysmem.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <sstream>
#include <string>

namespace ps2x::iop::detail
{
    IopSysmem::IopSysmem(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    bool IopSysmem::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        switch (ordinal)
        {
        case 4: // AllocSysMemory
        {
            const uint32_t address = a0 == 2u
                                         ? m_memory.allocate(a1, 16u, a2)
                                         : m_memory.allocate(a1, 16u);
            setV0(address);
            return true;
        }
        case 5: // FreeSysMemory
            setV0(m_memory.freeAllocation(a0) ? 0u : 0xFFFFFFFFu);
            return true;
        case 6: // QueryMemSize
            setV0(IopMemory::RamSize);
            return true;
        case 7: // QueryMaxFreeMemSize
        case 8: // QueryTotalFreeMemSize
            setV0(m_memory.maxFreeMemory());
            return true;
        case 9: // QueryBlockTopAddress
            if (const auto block = m_memory.allocationContaining(a0))
                setV0(block->address);
            else
                setV0(0u);
            return true;
        case 10: // QueryBlockSize
            if (const auto block = m_memory.allocationContaining(a0))
                setV0(block->size);
            else
                setV0(0xFFFFFFFFu);
            return true;
        case 14: // Kprintf
        {
            // Diagnostic: expand %s/%d/%u/%x arguments from $a1..$a4 so guest
            // diagnostics (e.g. IOMAN "Unknown device") are readable. Logging
            // only - no behavioural change.
            const std::string format = m_memory.readString(a0, 512u);
            std::string expanded;
            size_t argIndex = 0u;
            for (size_t i = 0u; i < format.size(); ++i)
            {
                if (format[i] == '%' && i + 1u < format.size())
                {
                    const char spec = format[i + 1u];
                    if (spec == 's' || spec == 'd' || spec == 'u' || spec == 'x' ||
                        spec == '%')
                    {
                        if (spec == '%')
                        {
                            expanded += '%';
                            ++i;
                            continue;
                        }
                        const uint32_t value = cpu.gpr[5u + (argIndex % 4u)];
                        ++argIndex;
                        if (spec == 's')
                        {
                            expanded += value
                                ? m_memory.readString(value, 128u)
                                : std::string("(null)");
                        }
                        else if (spec == 'x')
                        {
                            std::ostringstream hex;
                            hex << "0x" << std::hex << value;
                            expanded += hex.str();
                        }
                        else if (spec == 'u')
                            expanded += std::to_string(value);
                        else
                            expanded += std::to_string(static_cast<int32_t>(value));
                        ++i;
                        continue;
                    }
                }
                expanded += format[i];
            }
            m_host.log(LogLevel::Info, std::string("[IOP Kprintf] ") + expanded);
            setV0(static_cast<uint32_t>(format.size()));
            return true;
        }
        case 15:
            setV0(0u);
            return true;
        default:
            return false;
        }
    }
}
