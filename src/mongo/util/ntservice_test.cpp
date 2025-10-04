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


#include "mongo/util/ntservice.h"

#include "mongo/db/client.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/text.h"

#include <cstdarg>
#include <cstdlib>
#include <string>
#include <vector>

#include <shellapi.h>

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
    ASSERT_TRUE(
        svec("--dbpath=C:\\Data\\",
             "-logpath",
             "C:\\Program Files (x86)\\MongoDB\\Logs\\MongoDB.log",
             "--service",
             NULL) ==
        ntservice::constructServiceArgv(svec("-service",
                                             "--service",
                                             "--dbpath=C:\\Data\\",
                                             "--install",
                                             "-install",
                                             "--reinstall",
                                             "-reinstall",
                                             "--servicePassword==a\\b\\",
                                             "--servicePassword",
                                             "=a\\b\\",
                                             "--serviceUser",
                                             "andy",
                                             "--serviceName",
                                             "MongoDB",
                                             "-servicePassword==a\\b\\",
                                             "-servicePassword",
                                             "=a\\b\\",
                                             "-serviceUser",
                                             "andy",
                                             "-serviceName",
                                             "MongoDB",
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
    ASSERT_TRUE(inputArgvUtf8.end() ==
                std::transform(
                    inputArgvWide, inputArgvWide + inputArgc, inputArgvUtf8.begin(), toUtf8String));
    LocalFree(inputArgvWide);

    // Finally, confirm that we properly transform the argument vector and from it construct a legit
    // service command line.
    ASSERT_EQUALS(expectedServiceCommandLine,
                  constructUtf8WindowsCommandLine(ntservice::constructServiceArgv(inputArgvUtf8)));
}
