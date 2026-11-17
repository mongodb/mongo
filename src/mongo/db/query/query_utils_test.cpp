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

    // _id operator object with more than just $eq
    ASSERT_FALSE(mongo::isSimpleIdQuery(BSON("_id" << BSON("$eq" << 1 << "a" << 1))));

    // _id literal object equality
    ASSERT_TRUE(mongo::isSimpleIdQuery(BSON("_id" << mongo::BSONObj())));
    ASSERT_TRUE(mongo::isSimpleIdQuery(BSON("_id" << BSON("a" << 1))));
    ASSERT_TRUE(mongo::isSimpleIdQuery(BSON("_id" << BSON("a" << BSON("$eq" << 1)))));

    // '$'-prefixed fields after the first field are part of a literal object, not operators
    ASSERT_TRUE(mongo::isSimpleIdQuery(BSON("_id" << BSON("a" << 1 << "$eq" << 2))));
    ASSERT_TRUE(mongo::isSimpleIdQuery(BSON("_id" << BSON("a" << 1 << "$gt" << 2))));

    // _id comparison $eq with object
    ASSERT_TRUE(mongo::isSimpleIdQuery(BSON("_id" << BSON("$eq" << BSON("a" << 1)))));
}

TEST(QueryUtilsTest, TryUnwrapIdEqTest) {
    // exactly {$eq: <value>}
    auto eqInt = BSON("_id" << BSON("$eq" << 1));
    ASSERT_EQ(mongo::tryUnwrapIdEq(eqInt["_id"])->numberInt(), 1);
    auto eqObj = BSON("_id" << BSON("$eq" << BSON("a" << 1)));
    ASSERT_BSONOBJ_EQ(mongo::tryUnwrapIdEq(eqObj["_id"])->Obj(), BSON("a" << 1));

    // non-object and literal object values
    ASSERT_FALSE(mongo::tryUnwrapIdEq(BSON("_id" << 1)["_id"]));
    ASSERT_FALSE(mongo::tryUnwrapIdEq(BSON("_id" << mongo::BSONObj())["_id"]));
    ASSERT_FALSE(mongo::tryUnwrapIdEq(BSON("_id" << BSON("a" << 1))["_id"]));

    // '$eq' that is not the only field
    ASSERT_FALSE(mongo::tryUnwrapIdEq(BSON("_id" << BSON("a" << 1 << "$eq" << 2))["_id"]));
    ASSERT_FALSE(mongo::tryUnwrapIdEq(BSON("_id" << BSON("$eq" << 1 << "a" << 1))["_id"]));
}
}  // namespace
