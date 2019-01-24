
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

#include <string>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/cmdline_utils/censor_cmdline_test.h"
#include "mongo/util/options_parser/environment.h"

namespace moe = mongo::optionenvironment;

namespace mongo {
namespace {

TEST(ArgvVectorCensorTests, NothingCensored) {
    const std::vector<std::string> argv({"first",
                                         "second",
                                         "servicePassword=KEEP",
                                         "--servicePassword-",
                                         "KEEP",
                                         "--servicePasswordFake=KEEP"});
    test::censoringArgv(argv, argv);
    test::censoringVector(argv, argv);
}

TEST(ArgvCensorTests, SomeStuffCensoredDoubleHyphen) {
    const std::vector<std::string> argv({"first",
                                         "second",
                                         "--servicePassword=bad news",
                                         "--servicePassword-",
                                         "KEEP",
                                         "--servicePassword",
                                         "get out of dodge"});

    const std::vector<std::string> expected({"first",
                                             "second",
                                             "--servicePassword=xxxxxxxx",
                                             "--servicePassword-",
                                             "KEEP",
                                             "--servicePassword",
                                             "xxxxxxxxxxxxxxxx"});

    ASSERT_EQ(expected.size(), argv.size());
    test::censoringArgv(expected, argv);
}

TEST(ArgvCensorTests, SomeStuffCensoredSingleHyphen) {
    const std::vector<std::string> argv({"first",
                                         "second",
                                         "-servicePassword=bad news",
                                         "-servicePassword-",
                                         "KEEP",
                                         "-servicePassword",
                                         "get out of dodge"});

    const std::vector<std::string> expected({"first",
                                             "second",
                                             "-servicePassword=xxxxxxxx",
                                             "-servicePassword-",
                                             "KEEP",
                                             "-servicePassword",
                                             "xxxxxxxxxxxxxxxx"});

    ASSERT_EQ(expected.size(), argv.size());
    test::censoringArgv(expected, argv);
}

TEST(VectorCensorTests, SomeStuffCensoredDoubleHyphen) {
    const std::vector<std::string> argv({"first",
                                         "second",
                                         "--servicePassword=bad news",
                                         "--servicePassword-",
                                         "KEEP",
                                         "--servicePassword",
                                         "get out of dodge"});

    const std::vector<std::string> expected({"first",
                                             "second",
                                             "--servicePassword=<password>",
                                             "--servicePassword-",
                                             "KEEP",
                                             "--servicePassword",
                                             "<password>"});

    ASSERT_EQ(expected.size(), argv.size());
    test::censoringVector(expected, argv);
}

TEST(VectorCensorTests, SomeStuffCensoredSingleHyphen) {
    const std::vector<std::string> argv({"first",
                                         "second",
                                         "-servicePassword=bad news",
                                         "-servicePassword-",
                                         "KEEP",
                                         "-servicePassword",
                                         "get out of dodge"});

    const std::vector<std::string> expected({"first",
                                             "second",
                                             "-servicePassword=<password>",
                                             "-servicePassword-",
                                             "KEEP",
                                             "-servicePassword",
                                             "<password>"});

    ASSERT_EQ(expected.size(), argv.size());
    test::censoringVector(expected, argv);
}

TEST(BSONObjCensorTests, Strings) {
    BSONObj obj = BSON("firstarg"
                       << "not a password"
                       << "middlearg"
                       << "also not a password"
                       << "processManagement.windowsService.servicePassword"
                       << "this password should also be censored"
                       << "lastarg"
                       << false);

    BSONObj res = BSON("firstarg"
                       << "not a password"
                       << "middlearg"
                       << "also not a password"
                       << "processManagement.windowsService.servicePassword"
                       << "<password>"
                       << "lastarg"
                       << false);

    cmdline_utils::censorBSONObj(&obj);
    ASSERT_BSONOBJ_EQ(res, obj);
}

TEST(BSONObjCensorTests, Arrays) {
    BSONObj obj = BSON("firstarg"
                       << "not a password"
                       << "middlearg"
                       << "also not a password"
                       << "processManagement.windowsService.servicePassword"
                       << BSON_ARRAY("first censored password"
                                     << "next censored password")
                       << "lastarg"
                       << false);

    BSONObj res = BSON("firstarg"
                       << "not a password"
                       << "middlearg"
                       << "also not a password"
                       << "processManagement.windowsService.servicePassword"
                       << BSON_ARRAY("<password>"
                                     << "<password>")
                       << "lastarg"
                       << false);

    cmdline_utils::censorBSONObj(&obj);
    ASSERT_BSONOBJ_EQ(res, obj);
}

}  // namespace
}  // namespace mongo
