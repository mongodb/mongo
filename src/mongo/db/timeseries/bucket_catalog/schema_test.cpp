/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/unittest/unittest.h"

namespace mongo::timeseries::bucket_catalog {
namespace {

TEST(Schema, UpdateValue) {
    Schema schema;
    ASSERT_EQ(schema.update(BSON("a" << 1), boost::none, nullptr), Schema::UpdateStatus::Updated);
    ASSERT_EQ(schema.update(BSON("a" << 2), boost::none, nullptr), Schema::UpdateStatus::Updated);
    ASSERT_EQ(schema.update(BSON("a" << 3 << "b" << 1), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    ASSERT_EQ(schema.update(BSON("b" << 2), boost::none, nullptr), Schema::UpdateStatus::Updated);
    ASSERT_EQ(schema.update(BSON("a" << BSONUndefined), boost::none, nullptr),
              Schema::UpdateStatus::Failed);
    ASSERT_EQ(schema.update(BSON("a" << BSON("b" << 1)), boost::none, nullptr),
              Schema::UpdateStatus::Failed);
    ASSERT_EQ(schema.update(BSON("a" << BSON_ARRAY(1)), boost::none, nullptr),
              Schema::UpdateStatus::Failed);
}

TEST(Schema, UpdateArray) {
    Schema schema;
    ASSERT_EQ(schema.update(BSON("a" << BSON_ARRAY(1)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    ASSERT_EQ(schema.update(BSON("a" << BSON_ARRAY(2)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    ASSERT_EQ(schema.update(BSON("a" << BSON_ARRAY(1 << 2)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    ASSERT_EQ(schema.update(BSON("a" << BSONUndefined), boost::none, nullptr),
              Schema::UpdateStatus::Failed);
    ASSERT_EQ(schema.update(BSON("a" << 1), boost::none, nullptr), Schema::UpdateStatus::Failed);
    ASSERT_EQ(schema.update(BSON("a" << BSON("b" << 1)), boost::none, nullptr),
              Schema::UpdateStatus::Failed);
}

TEST(Schema, UpdateObject) {
    Schema schema;
    ASSERT_EQ(schema.update(BSON("a" << BSON("b" << 1)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    ASSERT_EQ(schema.update(BSON("a" << BSON("b" << 2)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    ASSERT_EQ(schema.update(BSON("a" << 1), boost::none, nullptr), Schema::UpdateStatus::Failed);
    ASSERT_EQ(schema.update(BSON("a" << BSONUndefined), boost::none, nullptr),
              Schema::UpdateStatus::Failed);
    ASSERT_EQ(schema.update(BSON("a" << BSON_ARRAY(1)), boost::none, nullptr),
              Schema::UpdateStatus::Failed);
}

TEST(Schema, TypeChangeFailsBeforeArrayPlaceholderClaim) {
    // Characterization test related to the MinMax lookup-map desync fixed by re-keying the map
    // when an array placeholder is renamed. Schema cannot reach that claim at all: placeholders
    // exist only under array-typed nodes, and an object update to such a node fails in
    // _shouldUpdateObj before the walk descends into its child list. The claim, and with it the
    // desync, is unreachable for Schema.
    Schema schema;

    // 'a' starts as an array, which creates placeholder entries in its child list.
    ASSERT_EQ(schema.update(BSON("a" << BSON_ARRAY(1 << 2 << 3)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);

    // A type change to object is rejected at 'a' before the walk descends, so no placeholder is
    // claimed.
    ASSERT_EQ(schema.update(BSON("a" << BSON("f" << 1)), boost::none, nullptr),
              Schema::UpdateStatus::Failed);

    // The same rejection applies in the opposite order.
    Schema schema2;
    ASSERT_EQ(schema2.update(BSON("a" << BSON("f" << 1)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    ASSERT_EQ(schema2.update(BSON("a" << BSON_ARRAY(1)), boost::none, nullptr),
              Schema::UpdateStatus::Failed);

    // The tree is unaffected by the rejected update and keeps working: the array still binds to
    // the same placeholders, and new fields resolve normally.
    ASSERT_EQ(schema.update(BSON("a" << BSON_ARRAY(1 << 2 << 3 << 4)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    ASSERT_EQ(schema.update(BSON("b" << 1), boost::none, nullptr), Schema::UpdateStatus::Updated);
}

}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
