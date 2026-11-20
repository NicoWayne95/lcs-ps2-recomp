#pragma once

class PS2Runtime;

namespace lcs
{
    // LCS_SELFTEST=1: checks recompiled helper functions against native results.
    bool runSelfTest(PS2Runtime &runtime);
}
