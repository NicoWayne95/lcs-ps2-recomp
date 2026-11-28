#pragma once
// ============================================================
// LCS RPC Diagnostic Trace
//   OFF by default.  Enable with  LCS_DIAG_RPC=1
//   Log file:         LCS_DIAG_RPC_LOG  (default "diag_rpc_trace.log")
//
//   Purely additive – NO behavior changes, no completion signals,
//   no WaitSema modifications, no ABI changes, no new mechanisms.
//   Writes to a separate log file; stderr is untouched.
// ============================================================

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace lcs_rpc_diag {

    // ---- constants ---------------------------------------------------
    constexpr uint32_t kFileioClient  = 0x646200u;
    constexpr uint32_t kFileioSid     = 0x80000001u;
    constexpr uint32_t kLoadfileSid   = 0x80000006u;
    constexpr int      kMaxFileio     = 10;   // first N FILEIO calls get detailed trace
    constexpr int      kMaxLoadfile   = 4;    // first N LOADFILE calls (control group)

    // ---- enable / path -----------------------------------------------
    inline bool enabled() noexcept
    {
        static const bool e = [] {
            const char* v = std::getenv("LCS_DIAG_RPC");
            if (!v || !*v) return false;
            return std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 ||
                   std::strcmp(v, "on")  == 0 || std::strcmp(v, "yes") == 0;
        }();
        return e;
    }

    inline const char* logPath() noexcept
    {
        static const char* p = [] {
            const char* v = std::getenv("LCS_DIAG_RPC_LOG");
            return (v && *v) ? v : "diag_rpc_trace.log";
        }();
        return p;
    }

    // ---- thread-safe emit ---------------------------------------------
    // C++17 inline namespace variables: a SINGLE entity shared by every
    // translation unit that includes this header (rpc_diag.obj / iop_rpc.obj /
    // lcs_bootstrap.obj). Function-local statics would be per-TU and would
    // break the shared budget flag, so we use inline globals instead.
    inline std::mutex g_mtx;
    inline int g_traceActive = 0;
    inline int g_fileioCount = 0;
    inline int g_loadfileCount = 0;

    inline void emit(const char* tag, const char* fmt, ...)
    {
        if (!enabled()) return;
        char body[1900];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(body, sizeof(body), fmt, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(g_mtx);
        FILE* f = std::fopen(logPath(), "a");
        if (!f) return;
        std::fprintf(f, "%s %s\n", tag, body);
        std::fflush(f);
        std::fclose(f);
    }

    // ---- per-call trace budget ----------------------------------------
    // The EE side calls beginTrace() once per SifCallRpc; all subsequent
    // trace points (EE + IOP) simply check tracing().
    // The IOP side never increments – it only reads the flag.
    inline void beginTrace(uint32_t client, uint32_t sid)
    {
        if (!enabled()) { g_traceActive = 0; return; }
        if (client == kFileioClient || sid == kFileioSid)
        {
            int n = ++g_fileioCount;
            g_traceActive = (n <= kMaxFileio) ? 1 : 0;
        }
        else if (sid == kLoadfileSid)
        {
            int n = ++g_loadfileCount;
            g_traceActive = (n <= kMaxLoadfile) ? 1 : 0;
        }
        else
        {
            g_traceActive = 0;
        }
    }

    inline bool tracing() noexcept { return g_traceActive != 0; }

    // ---- small helpers -------------------------------------------------
    inline const char* boolStr(bool b) noexcept { return b ? "1" : "0"; }

    inline const char* abiName(uint32_t abi) noexcept
    {
        switch (abi)
        {
        case 0: return "runtime-default";
        case 1: return "registers";
        case 2: return "stack";
        default: return "?";
        }
    }

} // namespace lcs_rpc_diag
