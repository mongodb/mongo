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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <boost/range/size.hpp>
#include <string>
#include <vector>

#include "mongo/db/cmdline.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/options_parser/environment.h"

namespace mongo {

    CmdLine cmdLine;

namespace {

    namespace moe = mongo::optionenvironment;

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

    TEST(ParsedOptsTests, NormalValues) {
        moe::Environment environment;
        ASSERT_OK(environment.set(moe::Key("val1"), moe::Value(6)));
        ASSERT_OK(environment.set(moe::Key("val2"), moe::Value(std::string("string"))));
        ASSERT_OK(CmdLine::setParsedOpts(environment));
        BSONObj obj = BSON( "val1" << 6 << "val2" << "string" );
        // TODO: Put a comparison here that doesn't depend on the field order.  Right now it is
        // based on the sort order of keys in a std::map.
        ASSERT_EQUALS(obj, CmdLine::getParsedOpts());
    }

    TEST(ParsedOptsTests, DottedValues) {
        moe::Environment environment;
        ASSERT_OK(environment.set(moe::Key("val1.dotted1"), moe::Value(6)));
        ASSERT_OK(environment.set(moe::Key("val2"), moe::Value(true)));
        ASSERT_OK(environment.set(moe::Key("val1.dotted2"), moe::Value(std::string("string"))));
        ASSERT_OK(CmdLine::setParsedOpts(environment));
        BSONObj obj = BSON( "val1" << BSON( "dotted1" << 6 << "dotted2" << "string" )
                         << "val2" << true );
        // TODO: Put a comparison here that doesn't depend on the field order.  Right now it is
        // based on the sort order of keys in a std::map.
        ASSERT_EQUALS(obj, CmdLine::getParsedOpts());
    }

    TEST(ParsedOptsTests, DeepDottedValues) {
        moe::Environment environment;
        ASSERT_OK(environment.set(moe::Key("val1.first1.second1.third1"), moe::Value(6)));
        ASSERT_OK(environment.set(moe::Key("val1.first1.second2.third1"), moe::Value(false)));
        ASSERT_OK(environment.set(moe::Key("val1.first2"), moe::Value(std::string("string"))));
        ASSERT_OK(environment.set(moe::Key("val1.first1.second1.third2"), moe::Value(true)));
        ASSERT_OK(environment.set(moe::Key("val2"), moe::Value(6.0)));
        ASSERT_OK(CmdLine::setParsedOpts(environment));
        BSONObj obj = BSON( "val1" << BSON( "first1" <<
                                            BSON( "second1" <<
                                                  BSON( "third1" << 6 << "third2" << true ) <<
                                                  "second2" <<
                                                  BSON( "third1" << false ) ) <<
                                            "first2" << "string" ) <<
                            "val2" << 6.0 );
        // TODO: Put a comparison here that doesn't depend on the field order.  Right now it is
        // based on the sort order of keys in a std::map.
        ASSERT_EQUALS(obj, CmdLine::getParsedOpts());
    }

}  // namespace
}  // namespace mongo
