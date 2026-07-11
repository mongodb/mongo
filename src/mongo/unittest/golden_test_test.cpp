// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/unittest/golden_test.h"

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
