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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/golden_test_base.h"
#include "mongo/unittest/test_info.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::unittest {

namespace fs = ::boost::filesystem;

class GoldenTestContext : public GoldenTestContextBase {
public:
    /** Format of the test header*/
    enum HeaderFormat {
        /** A simple text header, suitable for unstructured text output.*/
        Text,
    };

    explicit GoldenTestContext(
        const GoldenTestConfig* config,
        const TestInfo* testInfo = UnitTest::getInstance()->currentTestInfo(),
        bool validateOnClose = true)
        : GoldenTestContextBase(
              config,
              fs::path(sanitizeName(testInfo->suiteName().toString())) /
                  fs::path(sanitizeName(testInfo->testName().toString()) + ".txt"),
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
    const TestInfo* _testInfo;
};

}  // namespace mongo::unittest
