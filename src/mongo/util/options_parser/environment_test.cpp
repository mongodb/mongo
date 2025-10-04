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

#include "mongo/util/options_parser/environment.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/options_parser/constraints.h"

namespace {

using mongo::Status;

namespace moe = mongo::optionenvironment;

TEST(Environment, EmptyValue) {
    moe::Environment environment;
    ASSERT_NOT_OK(environment.set(moe::Key("empty"), moe::Value()));
}

TEST(Environment, MutuallyExclusive) {
    moe::Environment environment;
    moe::MutuallyExclusiveKeyConstraint constraint(moe::Key("key"), moe::Key("otherKey"));
    ASSERT_OK(environment.addKeyConstraint(&constraint));
    ASSERT_OK(environment.set(moe::Key("key"), moe::Value(1)));
    ASSERT_OK(environment.set(moe::Key("otherKey"), moe::Value(1)));
    ASSERT_NOT_OK(environment.validate());
}

TEST(Environment, RequiresOther) {
    moe::Environment environment;
    moe::RequiresOtherKeyConstraint constraint(moe::Key("key"), moe::Key("otherKey"));
    ASSERT_OK(environment.addKeyConstraint(&constraint));
    ASSERT_OK(environment.set(moe::Key("key"), moe::Value(1)));
    ASSERT_NOT_OK(environment.validate());
    ASSERT_OK(environment.set(moe::Key("otherKey"), moe::Value(1)));
    ASSERT_OK(environment.validate());
}

TEST(Environment, DirectTypeAccess) {
    moe::Environment environment;
    ASSERT_OK(environment.set(moe::Key("number"), moe::Value(5)));
    std::string notNumber;
    ASSERT_NOT_OK(environment.get(moe::Key("number"), &notNumber));
    int number;
    ASSERT_OK(environment.get(moe::Key("number"), &number));
    ASSERT_EQUALS(number, 5);
}

TEST(ToBSONTests, NormalValues) {
    moe::Environment environment;
    ASSERT_OK(environment.set(moe::Key("val1"), moe::Value(6)));
    ASSERT_OK(environment.set(moe::Key("val2"), moe::Value(std::string("string"))));
    mongo::BSONObj obj = BSON("val1" << 6 << "val2"
                                     << "string");
    // TODO: Put a comparison here that doesn't depend on the field order.  Right now it is
    // based on the sort order of keys in a std::map.
    ASSERT_BSONOBJ_EQ(obj, environment.toBSON());
}

TEST(ToBSONTests, DottedValues) {
    moe::Environment environment;
    ASSERT_OK(environment.set(moe::Key("val1.dotted1"), moe::Value(6)));
    ASSERT_OK(environment.set(moe::Key("val2"), moe::Value(true)));
    ASSERT_OK(environment.set(moe::Key("val1.dotted2"), moe::Value(std::string("string"))));
    mongo::BSONObj obj = BSON("val1" << BSON("dotted1" << 6 << "dotted2"
                                                       << "string")
                                     << "val2" << true);
    // TODO: Put a comparison here that doesn't depend on the field order.  Right now it is
    // based on the sort order of keys in a std::map.
    ASSERT_BSONOBJ_EQ(obj, environment.toBSON());
}

TEST(ToBSONTests, DeepDottedValues) {
    moe::Environment environment;
    ASSERT_OK(environment.set(moe::Key("val1.first1.second1.third1"), moe::Value(6)));
    ASSERT_OK(environment.set(moe::Key("val1.first1.second2.third1"), moe::Value(false)));
    ASSERT_OK(environment.set(moe::Key("val1.first2"), moe::Value(std::string("string"))));
    ASSERT_OK(environment.set(moe::Key("val1.first1.second1.third2"), moe::Value(true)));
    ASSERT_OK(environment.set(moe::Key("val2"), moe::Value(6.0)));
    mongo::BSONObj obj =
        BSON("val1" << BSON("first1" << BSON("second1" << BSON("third1" << 6 << "third2" << true)
                                                       << "second2" << BSON("third1" << false))
                                     << "first2"
                                     << "string")
                    << "val2" << 6.0);
    // TODO: Put a comparison here that doesn't depend on the field order.  Right now it is
    // based on the sort order of keys in a std::map.
    ASSERT_BSONOBJ_EQ(obj, environment.toBSON());
}
}  // unnamed namespace
