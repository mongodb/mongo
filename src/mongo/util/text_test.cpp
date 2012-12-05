/*
 *    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <cstdarg>
#include <string>
#include <vector>

#include "mongo/unittest/unittest.h"
#include "mongo/util/text.h"

using namespace mongo;

static std::vector<std::string> svec(const char* first, ...) {
    std::vector<std::string> result;
    if (first) {
        result.push_back(first);
        va_list ap;
        va_start(ap, first);
        const char* curr;
        while (NULL != (curr = va_arg(ap, const char*))) {
            result.push_back(curr);
        }
        va_end(ap);
    }
    return result;
}

TEST(WindowsCommandLineConstruction, EmptyCommandLine) {
    ASSERT_EQUALS("", constructUtf8WindowsCommandLine(svec(NULL)));
}

TEST(WindowsCommandLineConstruction, NothingToQuote) {
    ASSERT_EQUALS("abc d \"\" e",
                  constructUtf8WindowsCommandLine(svec("abc", "d", "", "e", NULL)));
}

TEST(WindowsCommandLineConstruction, ThingsToQuote) {
    ASSERT_EQUALS("a\\\\\\b \"de fg\" h",
                  constructUtf8WindowsCommandLine(svec("a\\\\\\b", "de fg", "h", NULL)));
    ASSERT_EQUALS("\"a\\\\b c\" d e",
                  constructUtf8WindowsCommandLine(svec("a\\\\b c", "d" , "e", NULL)));
    ASSERT_EQUALS("\"a \\\\\" \\",
                  constructUtf8WindowsCommandLine(svec("a \\", "\\", NULL)));
    ASSERT_EQUALS("\"\\\\\\\\\\\"\"",
                  constructUtf8WindowsCommandLine(svec("\\\\\"", NULL)));
}

TEST(WindowsCommandLineConstruction, RegressionSERVER_7252) {
    ASSERT_EQUALS("mongod \"--serviceName=My Service\" --serviceDescription \"My Service\" "
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
                                                       "--port", "20001",
                                                       "--logpath",
                                                       "C:\\mongo\\logs\\mongo_config.log.txt",
                                                       "--configsvr",
                                                       "--service",
                                                       NULL)));

}
