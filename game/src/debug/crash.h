#pragma once

namespace lcs
{
    // Print a reason and symbolized stack on uncaught exceptions, abort() and crashes.
    void installCrashHandlers();
}
