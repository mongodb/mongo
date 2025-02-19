/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include <cstdlib>
#include <fmt/format.h>
#include <iostream>

#include <boost/filesystem/path.hpp>
#include <boost/filesystem/path_traits.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/util/shell_exec.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::unittest {

using namespace fmt::literals;

void GoldenTestContext::printTestHeader(HeaderFormat format) {
    switch (format) {
        case HeaderFormat::Text:
            outStream() << "# Golden test output of " << _testInfo->suiteName() << "/"
                        << _testInfo->testName() << std::endl;
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

    throw TestAssertionFailureException(_testInfo->file().toString(), _testInfo->line(), message);
}

}  // namespace mongo::unittest
