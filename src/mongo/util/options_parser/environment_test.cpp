// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/options_parser/environment.h"

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
