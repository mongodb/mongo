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
