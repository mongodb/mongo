/*
 *    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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
    ASSERT_EQUALS("abc d \"\" e", constructUtf8WindowsCommandLine(svec("abc", "d", "", "e", NULL)));
}

TEST(WindowsCommandLineConstruction, ThingsToQuote) {
    ASSERT_EQUALS("a\\\\\\b \"de fg\" h",
                  constructUtf8WindowsCommandLine(svec("a\\\\\\b", "de fg", "h", NULL)));
    ASSERT_EQUALS("\"a\\\\b c\" d e",
                  constructUtf8WindowsCommandLine(svec("a\\\\b c", "d", "e", NULL)));
    ASSERT_EQUALS("\"a \\\\\" \\", constructUtf8WindowsCommandLine(svec("a \\", "\\", NULL)));
    ASSERT_EQUALS("\"\\\\\\\\\\\"\"", constructUtf8WindowsCommandLine(svec("\\\\\"", NULL)));
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
                                             NULL)));
}
