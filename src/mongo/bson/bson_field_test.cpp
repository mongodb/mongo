// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bson_field.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/mutable_bson/mutable_bson_test_utils.h"
#include "mongo/unittest/unittest.h"

namespace {

using mongo::BSONField;
using mongo::BSONObj;

TEST(Assignment, Simple) {
    BSONField<int> x("x");
    BSONObj o = BSON(x << 5);
    ASSERT_BSONOBJ_EQ(BSON("x" << 5), o);
}

TEST(Make, Simple) {
    BSONField<int> x("x");
    BSONObj o = BSON(x.make(5));
    ASSERT_BSONOBJ_EQ(BSON("x" << 5), o);
}

TEST(Query, GreaterThan) {
    BSONField<int> x("x");
    BSONObj o = BSON(x(5));
    ASSERT_BSONOBJ_EQ(BSON("x" << 5), o);

    o = BSON(x.gt(5));
    ASSERT_BSONOBJ_EQ(BSON("x" << BSON("$gt" << 5)), o);
}

TEST(Query, NotEqual) {
    BSONField<int> x("x");
    BSONObj o = BSON(x(10));
    ASSERT_BSONOBJ_EQ(BSON("x" << 10), o);

    o = BSON(x.ne(5));
    ASSERT_BSONOBJ_EQ(BSON("x" << BSON("$ne" << 5)), o);
}

}  // unnamed namespace
