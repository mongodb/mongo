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


#include "mongo/util/net/hostandport.h"

#include "mongo/db/server_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <fmt/format.h>

namespace mongo {
namespace {

TEST(HostAndPort, BasicLessThanComparison) {
    // Not less than self.
    ASSERT_FALSE(HostAndPort("a", 1) < HostAndPort("a", 1));

    // Lex order by name.
    ASSERT_LESS_THAN(HostAndPort("a", 1), HostAndPort("b", 1));
    ASSERT_FALSE(HostAndPort("b", 1) < HostAndPort("a", 1));

    // Then, order by port number.
    ASSERT_LESS_THAN(HostAndPort("a", 1), HostAndPort("a", 2));
    ASSERT_FALSE(HostAndPort("a", 2) < HostAndPort("a", 1));
}

TEST(HostAndPort, BasicEquality) {
    // Comparison on host field
    ASSERT_EQUALS(HostAndPort("a", 1), HostAndPort("a", 1));
    ASSERT_FALSE(HostAndPort("b", 1) == HostAndPort("a", 1));
    ASSERT_FALSE(HostAndPort("a", 1) != HostAndPort("a", 1));
    ASSERT_NOT_EQUALS(HostAndPort("b", 1), HostAndPort("a", 1));

    // Comparison on port field
    ASSERT_FALSE(HostAndPort("a", 1) == HostAndPort("a", 2));
    ASSERT_NOT_EQUALS(HostAndPort("a", 1), HostAndPort("a", 2));
}

TEST(HostAndPort, ImplicitPortSelection) {
    ASSERT_EQUALS(HostAndPort("a", -1), HostAndPort("a", int(ServerGlobalParams::DefaultDBPort)));
    ASSERT_EQUALS(int(ServerGlobalParams::DefaultDBPort), HostAndPort("a", -1).port());
    ASSERT_FALSE(HostAndPort("a", -1).empty());
}

TEST(HostAndPort, ConstructorParsing) {
    ASSERT_THROWS(HostAndPort(""), AssertionException);
    ASSERT_THROWS(HostAndPort("a:"), AssertionException);
    ASSERT_THROWS(HostAndPort("a:0xa"), AssertionException);
    ASSERT_THROWS(HostAndPort(":123"), AssertionException);
    ASSERT_THROWS(HostAndPort("[124d:"), AssertionException);
    ASSERT_THROWS(HostAndPort("[124d:]asdf:34"), AssertionException);
    ASSERT_THROWS(HostAndPort("frim[124d:]:34"), AssertionException);
    ASSERT_THROWS(HostAndPort("[124d:]12:34"), AssertionException);
    ASSERT_THROWS(HostAndPort("124d:12:34"), AssertionException);

    ASSERT_EQUALS(HostAndPort("abc"), HostAndPort("abc", -1));
    ASSERT_EQUALS(HostAndPort("abc.def:3421"), HostAndPort("abc.def", 3421));
    ASSERT_EQUALS(HostAndPort("[124d:]:34"), HostAndPort("124d:", 34));
    ASSERT_EQUALS(HostAndPort("[124d:efg]:34"), HostAndPort("124d:efg", 34));
    ASSERT_EQUALS(HostAndPort("[124d:]"), HostAndPort("124d:", -1));
}

TEST(HostAndPort, StaticParseFunction) {
    ASSERT_EQUALS(ErrorCodes::FailedToParse, HostAndPort::parse("").getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, HostAndPort::parse("a:").getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, HostAndPort::parse("a:0").getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, HostAndPort::parse("a:0xa").getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, HostAndPort::parse(":123").getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, HostAndPort::parse("[124d:").getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, HostAndPort::parse("[124d:]asdf:34").getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, HostAndPort::parse("124d:asdf:34").getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, HostAndPort::parse("1234:").getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, HostAndPort::parse("[[124d]]").getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse, HostAndPort::parse("[[124d]:34]").getStatus());

    ASSERT_EQUALS(unittest::assertGet(HostAndPort::parse("abc")), HostAndPort("abc", -1));
    ASSERT_EQUALS(unittest::assertGet(HostAndPort::parse("abc.def:3421")),
                  HostAndPort("abc.def", 3421));
    ASSERT_EQUALS(unittest::assertGet(HostAndPort::parse("[243:1bc]:21")),
                  HostAndPort("243:1bc", 21));
}

TEST(HostAndPort, RoundTripAbility) {
    ASSERT_EQUALS(HostAndPort("abc"), HostAndPort(HostAndPort("abc").toString()));
    ASSERT_EQUALS(HostAndPort("abc.def:3421"), HostAndPort(HostAndPort("abc.def:3421").toString()));
    ASSERT_EQUALS(HostAndPort("[124d:]:34"), HostAndPort(HostAndPort("[124d:]:34").toString()));
    ASSERT_EQUALS(HostAndPort("[124d:]"), HostAndPort(HostAndPort("[124d:]").toString()));
}

TEST(HostAndPort, CanIdentifyDefaultRoutes) {
    ASSERT_TRUE(HostAndPort("0.0.0.0").isDefaultRoute());
    ASSERT_FALSE(HostAndPort("127.0.0.1").isDefaultRoute());
    ASSERT_TRUE(HostAndPort("[::]").isDefaultRoute());
    ASSERT_FALSE(HostAndPort("[::1]").isDefaultRoute());
    ASSERT_TRUE(HostAndPort("[0:0:0:0:0:0:0:0]").isDefaultRoute());
    ASSERT_TRUE(HostAndPort("[0:0:0::0:0:0]").isDefaultRoute());
    ASSERT_TRUE(HostAndPort("[0:0:0::00:0:0]").isDefaultRoute());
}

TEST(HostAndPort, Fmt) {
    const std::string specs[] = {
        "1.2.3.4",           //
        "1.2.3.4:123",       //
        "[1:2:3:4]",         //
        "[1:2:3:4]:123",     //
        "/dev/mongod.sock",  //
    };
    for (const auto& spec : specs) {
        const HostAndPort hp(spec);
        const std::string hps = hp.toString();
        ASSERT_EQUALS(fmt::format("{}", hp), hps);
        ASSERT_EQUALS(fmt::format("<{}>", hp), "<" + hps + ">");
        ASSERT_EQUALS(fmt::format("{}", hp), hps);
        ASSERT_EQUALS(fmt::format("{1:} says {0:}", "hello", hp), hps + " says hello");
        // Reject garbage modifiers, but an empty modifier should be okay.
        ASSERT_THROWS(fmt::format(fmt::runtime("{:x}"), hp), fmt::format_error);
        ASSERT_EQUALS(fmt::format("{:}", hp), hps);
    }
}

}  // namespace
}  // namespace mongo
