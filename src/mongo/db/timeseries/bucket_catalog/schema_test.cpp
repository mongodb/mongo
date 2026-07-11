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

}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
