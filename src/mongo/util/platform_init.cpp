// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#ifdef _WIN32
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include <crtdbg.h>
#include <mmsystem.h>
#endif

#include "mongo/base/init.h"        // IWYU pragma: keep
#include "mongo/logv2/log.h"        // IWYU pragma: keep
#include "mongo/util/stacktrace.h"  // IWYU pragma: keep

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


#ifdef _WIN32

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

std::string_view firstLine(const char* s) {
    const char* eol = s;
    while (*eol && *eol != '\n')
        ++eol;
    return std::string_view(s, eol - s);
}

std::string_view severity(int t) {
    switch (t) {
        case _CRT_WARN:
            return "WARN"sv;
        case _CRT_ERROR:
            return "ERROR"sv;
        case _CRT_ASSERT:
            return "ASSERT"sv;
    }
    return ""sv;
}

extern "C" {
// Print C runtime message, then fassert if it's more severe than a warning.
int __cdecl crtDebugCallback(int nRptType, char* originalMessage, int* returnValue) noexcept {
    *returnValue = 0;  // Returned by _CrtDbgReport. (1: starts the debugger).
    bool die = (nRptType != _CRT_WARN);
    LOGV2(23325,
          "*** C runtime debug report ***",
          "severity_nRptType"_attr = severity(nRptType),
          "firstLine_originalMessage"_attr = firstLine(originalMessage),
          "die_terminating_sd_sd"_attr = (die ? ", terminating"sv : ""sv));
    if (die) {
        fassertFailed(17006);
    }
    return TRUE;
}
}  // extern "C"
}  // namespace

MONGO_INITIALIZER(Behaviors_Win32)(InitializerContext*) {
    // do not display dialog on abort()
    _set_abort_behavior(0, _CALL_REPORTFAULT | _WRITE_ABORT_MSG);

    // hook the C runtime's error display
    _CrtSetReportHook(&crtDebugCallback);

    if (_setmaxstdio(8192) == -1) {
        LOGV2_WARNING(23326, "Failed to increase max open files limit from default of 512 to 8192");
    }

    // Let's try to set minimum Windows Kernel quantum length to smallest viable timer resolution in
    // order to allow sleepmillis() to support waiting periods below Windows default quantum length
    // (which can vary per Windows version)
    // See https://msdn.microsoft.com/en-us/library/windows/desktop/dd743626(v=vs.85).aspx
    TIMECAPS tc;
    int targetResolution = 1;
    int timerResolution;

    if (timeGetDevCaps(&tc, sizeof(TIMECAPS)) != TIMERR_NOERROR) {
        LOGV2_WARNING(23327, "Failed to read timer resolution range.");
        if (timeBeginPeriod(1) != TIMERR_NOERROR) {
            LOGV2_WARNING(23328, "Failed to set minimum timer resolution to 1 millisecond.");
        }
    } else {
        timerResolution =
            std::min(std::max(int(tc.wPeriodMin), targetResolution), int(tc.wPeriodMax));
        invariant(timeBeginPeriod(timerResolution) == TIMERR_NOERROR);
    }
}

}  // namespace mongo

#endif  // _WIN32
