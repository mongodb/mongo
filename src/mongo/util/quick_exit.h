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

#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/testing_proctor.h"

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
