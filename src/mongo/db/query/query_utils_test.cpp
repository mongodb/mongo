// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_utils.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

using ::mongo::NE;

namespace {
TEST(QueryUtilsTest, IsSimpleQueryIdTest) {
    // empty object
    ASSERT_FALSE(mongo::isSimpleIdQuery(mongo::BSONObj()));
    // no _id entry
    ASSERT_FALSE(mongo::isSimpleIdQuery(BSON("foo" << "bar")));
    // duplicate _id entries
    ASSERT_FALSE(mongo::isSimpleIdQuery(BSON("_id" << "YES" << "_id" << "NO")));

    // _id comparison $eq with string
    ASSERT_TRUE(mongo::isSimpleIdQuery(BSON("_id" << "YES")));
    ASSERT_TRUE(mongo::isSimpleIdQuery(BSON("_id" << BSON("$eq" << "YES"))));

    // comparison with multiple fields
    ASSERT_FALSE(mongo::isSimpleIdQuery(BSON("_id" << "YES" << "another_field" << "foo")));

    // _id comparison $eq with array
    ASSERT_FALSE(mongo::isSimpleIdQuery(BSON("_id" << BSON("$eq" << BSON_ARRAY("YES" << "NO")))));
    ASSERT_FALSE(mongo::isSimpleIdQuery(BSON("_id" << BSON_ARRAY("YES" << "NO"))));

    // _id comparison $neq
    ASSERT_FALSE(mongo::isSimpleIdQuery(BSON("_id" << NE << "YES")));
}
}  // namespace
