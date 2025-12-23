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

#include "mongo/unittest/golden_test.h"

#include "mongo/base/string_data.h"
#include "mongo/unittest/golden_test_base.h"
#include "mongo/unittest/test_info.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <exception>
#include <ostream>
#include <string>

#include <boost/filesystem.hpp>
#include <fmt/format.h>

namespace mongo::unittest {
namespace {

namespace fs = boost::filesystem;

GoldenTestConfig goldenTestConfig{"src/mongo/unittest/expected_output"};

class GoldenSelfTestException : public std::exception {};

// Verify the basic output comparison works.
TEST(GoldenSelfTest, SanityTest) {
    GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    os << "Output 1:\n";
    os << "test test test 1\n";
    os << "Output 2:\n";
    os << "test test\n";
    os << "test 2\n";
}

// Verify the basic output comparison works, when config is reused.
TEST(GoldenSelfTest2, SanityTest2) {
    GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();
    os << "Output 1:\n";
    os << "test 1\n";
}

// Verify that test path is correctly generated from TestInfo.
TEST(GoldenSelfTest, GoldenTestContextGetPath) {
    GoldenTestContext ctx(&goldenTestConfig);
    ASSERT_EQ(ctx.getTestPath(),
              fs::path("golden_self_test") / fs::path("golden_test_context_get_path.txt"));
}

// Verify the basic output comparison works, when config is reused.
TEST(GoldenSelfTest2, DoesNotCompareWhenExceptionThrown) {
    ASSERT_THROWS(
        [&] {
            GoldenTestContext ctx(&goldenTestConfig);
            ctx.outStream() << "No such output" << std::endl;
            throw GoldenSelfTestException();
        }(),
        GoldenSelfTestException);
}
}  // namespace
}  // namespace mongo::unittest
