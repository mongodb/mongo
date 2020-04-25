/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/platform/basic.h"

#ifdef _WIN32
#include <crtdbg.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#endif

#include "mongo/base/init.h"
#include "mongo/logv2/log.h"
#include "mongo/util/stacktrace.h"

#ifdef _WIN32

namespace mongo {
namespace {

StringData firstLine(const char* s) {
    const char* eol = s;
    while (*eol && *eol != '\n')
        ++eol;
    return StringData(s, eol - s);
}

StringData severity(int t) {
    switch (t) {
        case _CRT_WARN:
            return "WARN"_sd;
        case _CRT_ERROR:
            return "ERROR"_sd;
        case _CRT_ASSERT:
            return "ASSERT"_sd;
    }
    return ""_sd;
}

extern "C" {
// Print C runtime message, then fassert if it's more severe than a warning.
int __cdecl crtDebugCallback(int nRptType, char* originalMessage, int* returnValue) noexcept {
    *returnValue = 0;  // Returned by _CrtDbgReport. (1: starts the debugger).
    bool die = (nRptType != _CRT_WARN);
    LOGV2(23325,
          "*** C runtime {severity_nRptType}: {firstLine_originalMessage}{die_terminating_sd_sd}",
          "severity_nRptType"_attr = severity(nRptType),
          "firstLine_originalMessage"_attr = firstLine(originalMessage),
          "die_terminating_sd_sd"_attr = (die ? ", terminating"_sd : ""_sd));
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

    if (_setmaxstdio(2048) == -1) {
        LOGV2_WARNING(23326, "Failed to increase max open files limit from default of 512 to 2048");
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

    return Status::OK();
}

}  // namespace mongo

#endif  // _WIN32
