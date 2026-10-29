// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/text.h"  // IWYU pragma: keep

#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <cstdarg>
#include <string>
#include <vector>

using namespace mongo;

static std::vector<std::string> svec(const char* first, ...) {
    std::vector<std::string> result;
    if (first) {
        result.push_back(first);
        va_list ap;
        va_start(ap, first);
        const char* curr;
        while (nullptr != (curr = va_arg(ap, const char*))) {
            result.push_back(curr);
        }
        va_end(ap);
    }
    return result;
}

TEST(WindowsCommandLineConstruction, EmptyCommandLine) {
    ASSERT_EQUALS("", constructUtf8WindowsCommandLine(svec(nullptr)));
}

TEST(WindowsCommandLineConstruction, NothingToQuote) {
    ASSERT_EQUALS("abc d \"\" e",
                  constructUtf8WindowsCommandLine(svec("abc", "d", "", "e", nullptr)));
}

TEST(WindowsCommandLineConstruction, ThingsToQuote) {
    ASSERT_EQUALS("a\\\\\\b \"de fg\" h",
                  constructUtf8WindowsCommandLine(svec("a\\\\\\b", "de fg", "h", nullptr)));
    ASSERT_EQUALS("\"a\\\\b c\" d e",
                  constructUtf8WindowsCommandLine(svec("a\\\\b c", "d", "e", nullptr)));
    ASSERT_EQUALS("\"a \\\\\" \\", constructUtf8WindowsCommandLine(svec("a \\", "\\", nullptr)));
    ASSERT_EQUALS("\"\\\\\\\\\\\"\"", constructUtf8WindowsCommandLine(svec("\\\\\"", nullptr)));
}

TEST(WindowsCommandLineConstruction, RegressionSERVER_7252) {
    ASSERT_EQUALS(
        "mongod \"--serviceName=My Service\" --serviceDescription \"My Service\" "
        "--serviceDisplayName \"My Service\" --dbpath C:\\mongo\\data\\config "
        "--port 20001 --logpath C:\\mongo\\logs\\mongo_config.log.txt "
        "--configsvr --service",
        constructUtf8WindowsCommandLine(svec("mongod",
                                             "--serviceName=My Service",
                                             "--serviceDescription",
                                             "My Service",
                                             "--serviceDisplayName",
                                             "My Service",
                                             "--dbpath",
                                             "C:\\mongo\\data\\config",
                                             "--port",
                                             "20001",
                                             "--logpath",
                                             "C:\\mongo\\logs\\mongo_config.log.txt",
                                             "--configsvr",
                                             "--service",
                                             nullptr)));
}
