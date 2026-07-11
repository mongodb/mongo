// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/shell/shell_options.h"

#include "mongo/unittest/unittest.h"

#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo {
namespace {

TEST(ShellOptions, RedactPasswords) {
    struct TestSpec {
        std::vector<std::string> in;
        std::vector<std::string_view> expected;
        std::string note;
    };
    std::vector<TestSpec> testData{
        {
            {"-u", "admin", "-p", "password", "--port", "27017"},
            {"-u", "admin", "-p", "xxxxxxxx", "--port", "27017"},
            "Check that just passwords get redacted correctly",
        },
        {
            {"-p", "password", "mongodb://admin:password@localhost"},
            {"-p", "xxxxxxxx", "mongodb://admin@localhost"},
            "Check that passwords and URIs get redacted correctly",
        },
        {
            {"mongodb://admin:password@localhost"},
            {"mongodb://admin@localhost"},
            "Check that just URIs get redacted correctly",
        },
        {
            {"localhost"},
            {"localhost"},
            "Sanity check that non-passwords don't get redacted",
        },
        {
            {"-p"},
            {"-p"},
            "Sanity check that we don't overflow argv",
        },
        {
            {"--password=foo"},
            {"--password=xxx"},
            "Check for --passsword=foo",
        },
        {
            {"-ppassword"},
            {"-pxxxxxxxx"},
        },
        {
            {"mongodb://admin@localhost/admin", "--password"},
            {"mongodb://admin@localhost/admin", "--password"},
            "Having --password at the end of the parameters list should do nothing since it means "
            "prompt for password",
        },
    };

    for (auto& [in, expected, note] : testData) {
        SCOPED_TRACE(note);
        // Sanity check for the test data
        ASSERT_EQ(in.size(), expected.size());
        std::vector<char*> argv;
        for (const auto& arg : in) {
            argv.push_back(const_cast<char*>(&arg.front()));
        }

        redactPasswordOptions(argv.size(), &argv.front());
        for (size_t i = 0; i < in.size(); i++) {
            SCOPED_TRACE(fmt::format("i: {}", i));
            auto shrunkArg = in[i];
            shrunkArg.resize(strlen(shrunkArg.c_str()));
            ASSERT_EQ(shrunkArg, expected[i]);
        }
    }
}
}  // namespace
}  // namespace mongo
