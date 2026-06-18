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
