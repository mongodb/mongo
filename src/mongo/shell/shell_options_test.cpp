/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault;

#include "shell_options.h"

#include <string>
#include <vector>

#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

TEST(ShellOptions, RedactPasswords) {
    std::vector<std::pair<std::vector<std::string>, const std::vector<StringData>>> testData = {
        // Check that just passwords get redacted correctly
        {{"-u", "admin", "-p", "password", "--port", "27017"},                     // NOLINT
         {"-u"_sd, "admin"_sd, "-p"_sd, "xxxxxxxx"_sd, "--port"_sd, "27017"_sd}},  // NOLINT
        // Check that passwords and URIs get redacted correctly
        {{"-p", "password", "mongodb://admin:password@localhost"},  // NOLINT
         {"-p"_sd, "xxxxxxxx", "mongodb://admin@localhost"_sd}},    // NOLINT
        // Check that just URIs get redacted correctly
        {{"mongodb://admin:password@localhost"},  // NOLINT
         {"mongodb://admin@localhost"_sd}},       // NOLINT
        // Sanity check that non-passwords don't get redacted
        {{"localhost"},      // NOLINT
         {"localhost"_sd}},  // NOLINT
        // Sanity check that we don't overflow argv
        {{"-p"},      // NOLINT
         {"-p"_sd}},  // NOLINT
        // Check for --passsword=foo
        {{"--password=foo"},      // NOLINT
         {"--password=xxx"_sd}},  // NOLINT
        {{"-ppassword"},          // NOLINT
         {"-pxxxxxxxx"}},         // NOLINT
        // Having --password at the end of the parameters list should do nothing since it means
        // prompt for password
        {{"mongodb://admin@localhost/admin", "--password"},         // NOLINT
         {"mongodb://admin@localhost/admin"_sd, "--password"_sd}},  // NOLINT
    };

    for (auto& testCase : testData) {
        // Sanity check for the test data
        ASSERT_EQ(testCase.first.size(), testCase.second.size());
        std::vector<char*> argv;
        for (const auto& arg : testCase.first) {
            argv.push_back(const_cast<char*>(&arg.front()));
        }

        redactPasswordOptions(argv.size(), &argv.front());
        for (size_t i = 0; i < testCase.first.size(); i++) {
            auto shrunkArg = testCase.first[i];
            shrunkArg.resize(::strlen(shrunkArg.c_str()));
            ASSERT_EQ(shrunkArg, testCase.second[i]);
        }
    }
}
}  // namespace
}  // namespace mongo
