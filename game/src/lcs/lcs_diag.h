// [lcs] Diagnosis build switch (CMake option LCS_DIAGNOSTICS): ON (build/diag) keeps every diagnostic hook and log
// line; OFF (build/release, the performance build) leaves out the pure diagnostic hooks (list / clump checks, heap and
// model rings, boot and timer traces) and the log lines that run during play. Errors and warnings stay in both.
#pragma once

#ifndef LCS_DIAGNOSTICS
#define LCS_DIAGNOSTICS 1
#endif

namespace lcs_diag
{
    inline constexpr bool kEnabled = LCS_DIAGNOSTICS != 0;
}

#include <cstdlib>

namespace lcs_diag
{
    // [lcs] a play-time diagnosis behind an env (the user's rule 2026-11-18: in the release build diagnoses run only
    // when asked for): the diag build has it on unless <env>=0, the release build only with <env>=1. Read it once
    // (a static) at the call site.
    inline bool wanted(const char *env)
    {
        const char *v = std::getenv(env);
        if constexpr (kEnabled)
            return !(v && v[0] == '0');
        else
            return v && v[0] && v[0] != '0';
    }
}
