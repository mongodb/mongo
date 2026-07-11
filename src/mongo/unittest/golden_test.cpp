// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/unittest/golden_test.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/shell_exec.h"

#include <cstdlib>
#include <iostream>

#include <boost/filesystem.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::unittest {

void GoldenTestContext::printTestHeader(HeaderFormat format) {
    switch (format) {
        case HeaderFormat::Text:
            outStream() << "# Golden test output of " << _testInfo->test_suite_name() << "/"
                        << _testInfo->name() << std::endl;
    }
}

void GoldenTestContext::onError(const std::string& message,
                                const std::string& actualStr,
                                const boost::optional<std::string>& expectedStr) {
    LOGV2_ERROR(6273501,
                "Test output verification failed",
                "message"_attr = message,
                "testPath"_attr = getTestPath().string(),
                "actualOutput"_attr = actualStr,
                "expectedOutput"_attr = expectedStr,
                "actualOutputPath"_attr = getActualOutputPath().string(),
                "expectedOutputPath"_attr = getExpectedOutputPath().string(),
                "actualOutputRoot"_attr = getEnv()->actualOutputRoot().string(),
                "expectedOutputRoot"_attr = getEnv()->expectedOutputRoot().string());

    auto diff_cmd = unittest::GoldenTestEnvironment::getInstance()->diffCmd(
        getExpectedOutputPath().string(), getActualOutputPath().string());

    // Execute a diff shell command and forward it's output to the current process output.
    // The output pipe of the subprocess is explictly captured to discourage the child process
    // from using interactive pagination, that otherwise might happen if the test process is
    // executed in interactive shell.
    const size_t kShellMaxLenBytes = 32 * 1024 * 1024;
    constexpr Seconds kShellDiffTimeout{60};
    auto diffOutput =
        shellExec(diff_cmd, kShellDiffTimeout, kShellMaxLenBytes, /*ignoreExitCode*/ true);
    if (diffOutput.isOK()) {
        std::cout << diffOutput.getValue() << std::endl;
    } else {
        LOGV2_ERROR(8336201,
                    "Failed to execute diff command",
                    "diff_cmd"_attr = diff_cmd,
                    "code"_attr = diffOutput.getStatus().codeString(),
                    "reason"_attr = diffOutput.getStatus().reason());
    }

    GTEST_FAIL_AT(_testInfo->file(), _testInfo->line()) << message;
}

}  // namespace mongo::unittest
