// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/modules.h"
#include "mongo/util/testing_proctor.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/** The quickExit function will call ::_exit and not return. Use this instead of calling ::_exit
 *  directly:
 *   - It offers a debugger hook to catch the process before leaving code under our control.
 *   - For some builds (leak sanitizer) it gives us an opportunity to dump leaks.
 *
 *  The quickExit function is named differently than quick_exit so that we can distinguish it from
 *  the C++11 function of the same name.
 *
 *  The quickExitWithoutLogging function is the same as quickExit, except that it doesn't do any
 *  pre-exit checks that might result in logging. This explains why quickExit is implemented as an
 *  inline wrapper around quickExitWithoutLogging - the pre-exit checks and logging need to refer
 *  to mongo symbols, which aren't permitted in quick_exit.cpp.
 */
MONGO_COMPILER_NORETURN void quickExitWithoutLogging(ExitCode);

MONGO_COMPILER_NORETURN inline void quickExit(ExitCode code) {
    warnIfTripwireAssertionsOccurred();
    if (code == ExitCode::clean) {
        TestingProctor::instance().exitAbruptlyIfDeferredErrors(false);
    }
    quickExitWithoutLogging(code);
}

MONGO_COMPILER_NORETURN inline void quickExit(int code) {
    quickExit(static_cast<ExitCode>(code));
}

}  // namespace mongo
