/**
 *    Copyright (C) 2012 10gen Inc.
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
 */

#include <boost/range/size.hpp>
#include <string>
#include <vector>

#include "mongo/db/cmdline.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    CmdLine cmdLine;

namespace {

    void testCensoringArgv(const char* const * expected,
                           const char* const * toCensor,
                           int elementCount) {

        std::vector<std::string> toCensorStringVec(toCensor, toCensor + elementCount);
        std::vector<char*> arrayStandin;
        for (size_t i = 0; i < toCensorStringVec.size(); ++i)
            arrayStandin.push_back(&*toCensorStringVec[i].begin());

        char** argv = &*arrayStandin.begin();

        CmdLine::censor(elementCount, argv);

        for (int i = 0; i < elementCount; ++i) {
            ASSERT_EQUALS(std::string(expected[i]), std::string(argv[i]));
        }
    }

    void testCensoringVector(const char* const * expected,
                             const char* const * toCensor,
                             int elementCount) {

        std::vector<std::string> actual(toCensor, toCensor + elementCount);

        CmdLine::censor(&actual);

        for (int i = 0; i < elementCount; ++i) {
            ASSERT_EQUALS(std::string(expected[i]), actual[i]);
        }
    }

    TEST(ArgvCensorTests, NothingCensored) {
        const char* const argv[] = {
            "first",
            "second",
            "sslPEMKeyPassword=KEEP",
            "---sslPEMKeyPassword=KEEP",
            "sslPEMKeyPassword",
            "KEEP",
            "servicePassword=KEEP",
            "--servicePassword-",
            "KEEP",
            "--servicePasswordFake=KEEP"
        };
        const int argc = boost::size(argv);
        testCensoringArgv(argv, argv, argc);
    }

    TEST(ArgvCensorTests, SomeStuffCensoredDoubleHyphen) {
        const char* const argv[] = {
            "first",
            "second",
            "--sslPEMKeyPassword=LOSEME",
            "--sslPEMKeyPassword",
            "Really, loose me!",
            "--servicePassword=bad news",
            "--servicePassword-",
            "KEEP",
            "--servicePassword",
            "get out of dodge"
        };
        const int argc = boost::size(argv);

        const char* const expected[] = {
            "first",
            "second",
            "--sslPEMKeyPassword=xxxxxx",
            "--sslPEMKeyPassword",
            "xxxxxxxxxxxxxxxxx",
            "--servicePassword=xxxxxxxx",
            "--servicePassword-",
            "KEEP",
            "--servicePassword",
            "xxxxxxxxxxxxxxxx"
        };
        ASSERT_EQUALS(static_cast<int>(boost::size(expected)), argc);

        testCensoringArgv(expected, argv, argc);
    }

    TEST(ArgvCensorTests, SomeStuffCensoredSingleHyphen) {
        const char* const argv[] = {
            "first",
            "second",
            "-sslPEMKeyPassword=LOSEME",
            "-sslPEMKeyPassword",
            "Really, loose me!",
            "-servicePassword=bad news",
            "-servicePassword-",
            "KEEP",
            "-servicePassword",
            "get out of dodge"
        };
        const int argc = boost::size(argv);

        const char* const expected[] = {
            "first",
            "second",
            "-sslPEMKeyPassword=xxxxxx",
            "-sslPEMKeyPassword",
            "xxxxxxxxxxxxxxxxx",
            "-servicePassword=xxxxxxxx",
            "-servicePassword-",
            "KEEP",
            "-servicePassword",
            "xxxxxxxxxxxxxxxx"
        };
        ASSERT_EQUALS(static_cast<int>(boost::size(expected)), argc);

        testCensoringArgv(expected, argv, argc);
    }

    TEST(VectorCensorTests, NothingCensored) {
        const char* const argv[] = {
            "first",
            "second",
            "sslPEMKeyPassword=KEEP",
            "---sslPEMKeyPassword=KEEP",
            "sslPEMKeyPassword",
            "KEEP",
            "servicePassword=KEEP",
            "--servicePassword-",
            "KEEP",
            "--servicePasswordFake=KEEP"
        };
        const int argc = boost::size(argv);
        testCensoringVector(argv, argv, argc);
    }

    TEST(VectorCensorTests, SomeStuffCensoredDoubleHyphen) {
        const char* const argv[] = {
            "first",
            "second",
            "--sslPEMKeyPassword=LOSEME",
            "--sslPEMKeyPassword",
            "Really, loose me!",
            "--servicePassword=bad news",
            "--servicePassword-",
            "KEEP",
            "--servicePassword",
            "get out of dodge"
        };
        const int argc = boost::size(argv);

        const char* const expected[] = {
            "first",
            "second",
            "--sslPEMKeyPassword=<password>",
            "--sslPEMKeyPassword",
            "<password>",
            "--servicePassword=<password>",
            "--servicePassword-",
            "KEEP",
            "--servicePassword",
            "<password>"
        };
        ASSERT_EQUALS(static_cast<int>(boost::size(expected)), argc);

        testCensoringVector(expected, argv, argc);
    }

    TEST(VectorCensorTests, SomeStuffCensoredSingleHyphen) {
        const char* const argv[] = {
            "first",
            "second",
            "-sslPEMKeyPassword=LOSEME",
            "-sslPEMKeyPassword",
            "Really, loose me!",
            "-servicePassword=bad news",
            "-servicePassword-",
            "KEEP",
            "-servicePassword",
            "get out of dodge"
        };
        const int argc = boost::size(argv);

        const char* const expected[] = {
            "first",
            "second",
            "-sslPEMKeyPassword=<password>",
            "-sslPEMKeyPassword",
            "<password>",
            "-servicePassword=<password>",
            "-servicePassword-",
            "KEEP",
            "-servicePassword",
            "<password>"
        };
        ASSERT_EQUALS(static_cast<int>(boost::size(expected)), argc);

        testCensoringVector(expected, argv, argc);
    }

}  // namespace
}  // namespace mongo
