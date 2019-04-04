/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include <cstdarg>
#include <cstdlib>
#include <shellapi.h>
#include <string>
#include <vector>

#include "merizo/db/client.h"
#include "merizo/unittest/unittest.h"
#include "merizo/util/ntservice.h"
#include "merizo/util/text.h"

using namespace merizo;

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
             "C:\\Program Files (x86)\\MerizoDB\\Logs\\MerizoDB.log",
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
                                             "MerizoDB",
                                             "-servicePassword==a\\b\\",
                                             "-servicePassword",
                                             "=a\\b\\",
                                             "-serviceUser",
                                             "andy",
                                             "-serviceName",
                                             "MerizoDB",
                                             "-logpath",
                                             "C:\\Program Files (x86)\\MerizoDB\\Logs\\MerizoDB.log",
                                             NULL)));
}

TEST(NtService, RegressionSERVER_7252) {
    // Test that we generate a correct service command line from the literal command line supplied
    // in ticket SERVER-7252.

    const wchar_t inputCommandLine[] =
        L"merizod --install --serviceName=\"My Service\" --serviceDescription \"My Service\" "
        L"--serviceDisplayName \"My Service\" --dbpath C:\\merizo\\data\\config --port 20001 "
        L"--logpath C:\\merizo\\logs\\merizo_config.log.txt --configsvr";

    const char expectedServiceCommandLine[] =
        "merizod --dbpath C:\\merizo\\data\\config --port 20001 "
        "--logpath C:\\merizo\\logs\\merizo_config.log.txt --configsvr --service";

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
