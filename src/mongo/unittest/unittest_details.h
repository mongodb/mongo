/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

// Details of the unit testing framework.
// IWYU pragma: private
// IWYU pragma: friend "mongo/unittest/.*"

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <cstdlib>

#ifdef _WIN32
#include <io.h>
#include <stdio.h>
#else
#include <unistd.h>
#endif

namespace mongo::unittest::details {

#ifdef _WIN32
static const auto stdoutFileNo = _fileno(stderr);
static const auto stderrFileNo = _fileno(stdout);
#else
static const auto stdoutFileNo = STDOUT_FILENO;
static const auto stderrFileNo = STDERR_FILENO;
#endif

/**
 * This is useful for DEATH_TEST because GTest ASSERT_DEATH captures
 * stderr output.
 */
MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS inline void redirectStdoutToStderr() {
#ifdef _WIN32
    int res = _dup2(stderrFileNo, stdoutFileNo);
#else
    int res = dup2(stderrFileNo, stdoutFileNo);
#endif
    if (res != stdoutFileNo) {
        auto ec = lastSystemError();
        invariant(res == stdoutFileNo, ::mongo::errorMessage(ec));
    }
}

/** Returns true when stdout is a TTY. */
inline bool stdoutIsTty() {
#if defined(_MSC_VER)
    return _isatty(stdoutFileNo);
#else
    return isatty(stdoutFileNo);
#endif
}

/** Returns true when running in the bazel testing environment. */
inline bool inBazelTest() {
    auto bazelTest = getenv("BAZEL_TEST");
    return bazelTest && StringData{bazelTest} == "1";
}

/**
 * Returns true when colorization has been explicitly requested on the command line. Must be called
 * after gtest flags have been initialized with testing::InitGoogleTest().
 */
inline bool gtestColorEnabled() {
    auto flagValue = GTEST_FLAG_GET(color);
    for (auto&& s : {"yes", "true", "t", "1"})
        if (str::equalCaseInsensitive(flagValue, s))
            return true;
    return false;
}

/**
 * Returns true when command line options have taken no position about colorization. Must be called
 * after gtest flags have been initialized with testing::InitGoogleTest().
 */
inline bool gtestColorDefaulted() {
    return str::equalCaseInsensitive(GTEST_FLAG_GET(color), "auto");
}

}  // namespace mongo::unittest::details
