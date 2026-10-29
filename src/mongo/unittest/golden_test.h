// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/unittest/golden_test_base.h"
#include "mongo/unittest/test_info.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::unittest {

namespace fs = ::boost::filesystem;

class [[MONGO_MOD_PUBLIC]] GoldenTestContext : public GoldenTestContextBase {
public:
    /** Format of the test header*/
    enum HeaderFormat {
        /** A simple text header, suitable for unstructured text output.*/
        Text,
    };

    explicit GoldenTestContext(
        const GoldenTestConfig* config,
        const testing::TestInfo* testInfo = testing::UnitTest::GetInstance()->current_test_info(),
        bool validateOnClose = true)
        : GoldenTestContextBase(config,
                                fs::path(sanitizeName(std::string{testInfo->test_suite_name()})) /
                                    fs::path(sanitizeName(std::string{testInfo->name()}) + ".txt"),
                                validateOnClose,
                                [this](auto const&... args) { return onError(args...); }),
          _testInfo(testInfo) {}

    /**
     * Prints the test header in a specified format.
     */
    void printTestHeader(HeaderFormat format);

    // Disable move/copy because onError captures 'this' address.
    GoldenTestContext(GoldenTestContext&&) = delete;

protected:
    void onError(const std::string& message,
                 const std::string& actualStr,
                 const boost::optional<std::string>& expectedStr);

private:
    const testing::TestInfo* _testInfo;
};

}  // namespace mongo::unittest
