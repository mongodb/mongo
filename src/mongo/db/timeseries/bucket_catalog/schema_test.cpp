// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/unittest/unittest.h"

namespace mongo::timeseries::bucket_catalog {
namespace {

TEST(Schema, UpdateValue) {
    tracking::Context trackingContext;
    Schema schema{trackingContext};
    EXPECT_EQ(schema.update(BSON("a" << 1), boost::none, nullptr), Schema::UpdateStatus::Updated);
    EXPECT_EQ(schema.update(BSON("a" << 2), boost::none, nullptr), Schema::UpdateStatus::Updated);
    EXPECT_EQ(schema.update(BSON("a" << 3 << "b" << 1), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    EXPECT_EQ(schema.update(BSON("b" << 2), boost::none, nullptr), Schema::UpdateStatus::Updated);
    EXPECT_EQ(schema.update(BSON("a" << BSONUndefined), boost::none, nullptr),
              Schema::UpdateStatus::Failed);
    EXPECT_EQ(schema.update(BSON("a" << BSON("b" << 1)), boost::none, nullptr),
              Schema::UpdateStatus::Failed);
    EXPECT_EQ(schema.update(BSON("a" << BSON_ARRAY(1)), boost::none, nullptr),
              Schema::UpdateStatus::Failed);
}

TEST(Schema, UpdateArray) {
    tracking::Context trackingContext;
    Schema schema{trackingContext};
    EXPECT_EQ(schema.update(BSON("a" << BSON_ARRAY(1)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    EXPECT_EQ(schema.update(BSON("a" << BSON_ARRAY(2)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    EXPECT_EQ(schema.update(BSON("a" << BSON_ARRAY(1 << 2)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    EXPECT_EQ(schema.update(BSON("a" << BSONUndefined), boost::none, nullptr),
              Schema::UpdateStatus::Failed);
    EXPECT_EQ(schema.update(BSON("a" << 1), boost::none, nullptr), Schema::UpdateStatus::Failed);
    EXPECT_EQ(schema.update(BSON("a" << BSON("b" << 1)), boost::none, nullptr),
              Schema::UpdateStatus::Failed);
}

TEST(Schema, UpdateObject) {
    tracking::Context trackingContext;
    Schema schema{trackingContext};
    EXPECT_EQ(schema.update(BSON("a" << BSON("b" << 1)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    EXPECT_EQ(schema.update(BSON("a" << BSON("b" << 2)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    EXPECT_EQ(schema.update(BSON("a" << 1), boost::none, nullptr), Schema::UpdateStatus::Failed);
    EXPECT_EQ(schema.update(BSON("a" << BSONUndefined), boost::none, nullptr),
              Schema::UpdateStatus::Failed);
    EXPECT_EQ(schema.update(BSON("a" << BSON_ARRAY(1)), boost::none, nullptr),
              Schema::UpdateStatus::Failed);
}

TEST(Schema, TypeChangeFailsBeforeArrayPlaceholderClaim) {
    // Characterization test related to the MinMax lookup-map desync fixed by re-keying the map
    // when an array placeholder is renamed. Schema cannot reach that claim at all: placeholders
    // exist only under array-typed nodes, and an object update to such a node fails in
    // _shouldUpdateObj before the walk descends into its child list. The claim, and with it the
    // desync, is unreachable for Schema.
    tracking::Context trackingContext;
    Schema schema{trackingContext};

    // 'a' starts as an array, which creates placeholder entries in its child list.
    EXPECT_EQ(schema.update(BSON("a" << BSON_ARRAY(1 << 2 << 3)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);

    // A type change to object is rejected at 'a' before the walk descends, so no placeholder is
    // claimed.
    EXPECT_EQ(schema.update(BSON("a" << BSON("f" << 1)), boost::none, nullptr),
              Schema::UpdateStatus::Failed);

    // The same rejection applies in the opposite order.
    Schema schema2{trackingContext};
    EXPECT_EQ(schema2.update(BSON("a" << BSON("f" << 1)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    EXPECT_EQ(schema2.update(BSON("a" << BSON_ARRAY(1)), boost::none, nullptr),
              Schema::UpdateStatus::Failed);

    // The tree is unaffected by the rejected update and keeps working: the array still binds to
    // the same placeholders, and new fields resolve normally.
    EXPECT_EQ(schema.update(BSON("a" << BSON_ARRAY(1 << 2 << 3 << 4)), boost::none, nullptr),
              Schema::UpdateStatus::Updated);
    EXPECT_EQ(schema.update(BSON("b" << 1), boost::none, nullptr), Schema::UpdateStatus::Updated);
}

}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
