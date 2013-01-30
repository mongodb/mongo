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

#include "mongo/platform/basic.h"

#include <cstdarg>
#include <cstdlib>
#include <shellapi.h>
#include <string>
#include <vector>


#include "mongo/db/client.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/ntservice.h"
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

TEST(NtService, ConstructServiceCommandLine) {
    ASSERT_TRUE(svec("--dbpath=C:\\Data\\",
                       "-logpath",
                       "C:\\Program Files (x86)\\MongoDB\\Logs\\MongoDB.log",
                       "--service",
                       NULL) ==
                  ntservice::constructServiceArgv(
                          svec("-service", "--service",
                               "--dbpath=C:\\Data\\",
                               "--install", "-install",
                               "--reinstall", "-reinstall",
                               "--servicePassword==a\\b\\",
                               "--servicePassword", "=a\\b\\",
                               "--serviceUser", "andy",
                               "--serviceName", "MongoDB",
                               "-servicePassword==a\\b\\",
                               "-servicePassword", "=a\\b\\",
                               "-serviceUser", "andy",
                               "-serviceName", "MongoDB",
                               "-logpath",
                               "C:\\Program Files (x86)\\MongoDB\\Logs\\MongoDB.log",
                               NULL)));
}

TEST(NtService, RegressionSERVER_7252) {
    // Test that we generate a correct service command line from the literal command line supplied
    // in ticket SERVER-7252.

    const wchar_t inputCommandLine[] =
        L"mongod --install --serviceName=\"My Service\" --serviceDescription \"My Service\" "
        L"--serviceDisplayName \"My Service\" --dbpath C:\\mongo\\data\\config --port 20001 "
        L"--logpath C:\\mongo\\logs\\mongo_config.log.txt --configsvr";

    const char expectedServiceCommandLine[] =
        "mongod --dbpath C:\\mongo\\data\\config --port 20001 "
        "--logpath C:\\mongo\\logs\\mongo_config.log.txt --configsvr --service";

    // Convert the input wide-character command line into a UTF-8 vector of std::string.
    int inputArgc;
    LPWSTR* inputArgvWide = CommandLineToArgvW(inputCommandLine, &inputArgc);
    ASSERT_TRUE(NULL != inputArgvWide);
    ASSERT_GREATER_THAN_OR_EQUALS(inputArgc, 0);
    std::vector<std::string> inputArgvUtf8(inputArgc);
    ASSERT_TRUE(inputArgvUtf8.end() == std::transform(inputArgvWide,
                                                      inputArgvWide + inputArgc,
                                                      inputArgvUtf8.begin(),
                                                      toUtf8String));
    LocalFree(inputArgvWide);

    // Finally, confirm that we properly transform the argument vector and from it construct a legit
    // service command line.
    ASSERT_EQUALS(expectedServiceCommandLine,
                  constructUtf8WindowsCommandLine(ntservice::constructServiceArgv(inputArgvUtf8)));
}

// CRUTCHES!
namespace mongo {
    enum ExitCode;
    void exitCleanly(ExitCode ignored) { std::abort(); }
    Client& Client::initThread(const char* desc, AbstractMessagingPort* mp) {
        std::abort(); return *reinterpret_cast<Client*>(NULL);
    }
}  // namespace mongo
