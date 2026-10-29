// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

// Details of the unit testing framework.
// IWYU pragma: private
// IWYU pragma: friend "mongo/unittest/.*"

#include "mongo/unittest/unittest.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <cstdlib>

#ifdef _WIN32
#include <io.h>
#include <stdio.h>
#else
#include <string_view>

#include <unistd.h>

#include <sys/resource.h>
#endif

namespace mongo::unittest::details {

#ifdef _WIN32
static const auto stdoutFileNo = _fileno(stderr);
static const auto stderrFileNo = _fileno(stdout);
#else
static const auto stdoutFileNo = STDOUT_FILENO;
static const auto stderrFileNo = STDERR_FILENO;
#endif

[[MONGO_MOD_PUBLIC]] inline void suppressCoreDumps() {
#ifndef _WIN32
    const struct rlimit zero{0, 0};
    if (int res = setrlimit(RLIMIT_CORE, &zero); res == -1) {
        auto ec = lastSystemError();
        invariant(res != -1, errorMessage(ec));
    }
#endif
}

/**
 * This is useful for DEATH_TEST because GTest ASSERT_DEATH captures
 * stderr output.
 */
[[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]] inline void redirectStdoutToStderr() {
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
    return bazelTest && std::string_view{bazelTest} == "1";
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

/**
 * Prints mongo-specific exception info to the file. Returns true when the current exception is
 * caught by the ActiveExceptionWitness.
 */
inline bool printExceptionInfo(FILE* file) {
    if (!std::current_exception())
        return false;

    auto diag = [&](std::string_view info) {
        fmt::println(file, "Exception encountered, extra info:");
        fmt::println(file, "{}", info);
        fmt::println(file, "");
    };

    auto info = activeExceptionInfo();
    if (info) {
        diag(info->description);
    } else {
        diag("Unknown exception encountered.");
        return false;
    }
    return true;
}

}  // namespace mongo::unittest::details
