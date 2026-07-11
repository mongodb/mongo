// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/shard_key_util.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TimeseriesOptions makeTimeseriesOptions(std::string_view timeField,
                                        boost::optional<std::string_view> metaField) {
    TimeseriesOptions options{std::string{timeField}};
    if (metaField) {
        options.setMetaField(*metaField);
    }
    return options;
}

// Tests for isRawTimeseriesShardKey

TEST(IsRawTimeseriesShardKeyTest, UserFacingTimeFieldKeyIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("time" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, UserFacingMetaFieldKeyIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("hostId" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, UserFacingMetaSubfieldKeyIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("hostId.x" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, UserFacingCompoundKeyIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_FALSE(
        shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("hostId" << 1 << "time" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, TranslatedMetaFieldKeyIsTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_TRUE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("meta" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, TranslatedMetaSubfieldKeyIsTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_TRUE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("meta.x" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, TranslatedTimeFieldKeyIsTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_TRUE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("control.min.time" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, TranslatedCompoundKeyIsTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_TRUE(shardkeyutil::isRawTimeseriesShardKey(
        tsOptions, BSON("meta" << 1 << "control.min.time" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, MetaFieldNamedMetaIsHandledCorrectly) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("meta"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("meta" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, TimeFieldOnlyNoMetaFieldIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("ts", boost::none);
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("ts" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, TranslatedTimeFieldNoMetaFieldIsTranslated) {
    auto tsOptions = makeTimeseriesOptions("ts", boost::none);
    ASSERT_TRUE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("control.min.ts" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, HashedMetaFieldKeyIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("hostId.x" << "hashed")));
}

TEST(IsRawTimeseriesShardKeyTest, TranslatedHashedMetaFieldKeyIsTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_TRUE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("meta.x" << "hashed")));
}

TEST(IsRawTimeseriesShardKeyTest, InvalidIdFieldIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("_id" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, InvalidArbitraryFieldIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("someRandomField" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, EmptyKeyIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSONObj()));
}

TEST(IsRawTimeseriesShardKeyTest, ControlMaxTimeFieldIsTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_TRUE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("control.max.time" << -1)));
}

TEST(IsRawTimeseriesShardKeyTest, ControlWithoutMinMaxIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("control" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, ControlMinWithWrongFieldIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_FALSE(
        shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("control.min.wrongField" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, MixedValidAndInvalidFieldsIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("meta" << 1 << "_id" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, MixedUserFacingAndTranslatedFieldsIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_FALSE(
        shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("hostId" << 1 << "meta" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, MixedTranslatedTimeAndInvalidFieldIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(
        tsOptions, BSON("control.min.time" << 1 << "_id" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, MixedValidMetaAndInvalidControlFieldIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(
        tsOptions, BSON("meta" << 1 << "control.min.wrongField" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, TranslatedDescendingCompoundKeyIsTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_TRUE(shardkeyutil::isRawTimeseriesShardKey(
        tsOptions, BSON("meta" << 1 << "control.max.time" << -1 << "control.min.time" << -1)));
}

TEST(IsRawTimeseriesShardKeyTest, NoMetaFieldWithInvalidFieldIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("ts", boost::none);
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("_id" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, NoMetaFieldWithMetaKeyIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("ts", boost::none);
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("meta" << 1)));
}

// Tests for validateAndTranslateTimeseriesShardKey

TEST(ValidateAndTranslateTimeseriesShardKeyTest, MetaFieldKeyTranslatesCorrectly) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    auto result =
        shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, BSON("hostId" << 1));
    ASSERT_BSONOBJ_EQ(result, BSON("meta" << 1));
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, MetaSubfieldKeyTranslatesCorrectly) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    auto result =
        shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, BSON("hostId.x" << 1));
    ASSERT_BSONOBJ_EQ(result, BSON("meta.x" << 1));
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, TimeFieldKeyTranslatesCorrectly) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    auto result =
        shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, BSON("time" << 1));
    ASSERT_BSONOBJ_EQ(result, BSON("control.min.time" << 1));
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, CompoundKeyTranslatesCorrectly) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    auto result = shardkeyutil::validateAndTranslateTimeseriesShardKey(
        tsOptions, BSON("hostId" << 1 << "time" << 1));
    ASSERT_BSONOBJ_EQ(result, BSON("meta" << 1 << "control.min.time" << 1));
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, HashedMetaFieldKeyTranslatesCorrectly) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    auto result = shardkeyutil::validateAndTranslateTimeseriesShardKey(
        tsOptions, BSON("hostId.x" << "hashed"));
    ASSERT_BSONOBJ_EQ(result, BSON("meta.x" << "hashed"));
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, DescendingTimeFieldKeyTranslatesCorrectly) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    auto result =
        shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, BSON("time" << -1));
    ASSERT_BSONOBJ_EQ(result, BSON("control.max.time" << -1 << "control.min.time" << -1));
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, InvalidFieldThrows5914001) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_THROWS_CODE(
        shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, BSON("_id" << 1)),
        AssertionException,
        5914001);
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, InvalidArbitraryFieldThrows5914001) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_THROWS_CODE(shardkeyutil::validateAndTranslateTimeseriesShardKey(
                           tsOptions, BSON("someRandomField" << 1)),
                       AssertionException,
                       5914001);
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, TimeFieldNotAtEndThrows5914000) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_THROWS_CODE(shardkeyutil::validateAndTranslateTimeseriesShardKey(
                           tsOptions, BSON("time" << 1 << "hostId" << 1)),
                       AssertionException,
                       5914000);
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, TimeFieldWithHashedThrows880031) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));
    ASSERT_THROWS_CODE(
        shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, BSON("time" << "hashed")),
        AssertionException,
        880031);
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, NoMetaFieldWithMetaKeyThrows5914001) {
    auto tsOptions = makeTimeseriesOptions("time", boost::none);
    ASSERT_THROWS_CODE(
        shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, BSON("someField" << 1)),
        AssertionException,
        5914001);
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, NoMetaFieldTimeKeyTranslatesCorrectly) {
    auto tsOptions = makeTimeseriesOptions("ts", boost::none);
    auto result = shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, BSON("ts" << 1));
    ASSERT_BSONOBJ_EQ(result, BSON("control.min.ts" << 1));
}

TEST(TimeseriesShardKeyValidationFlowTest, InvalidKeyIsRejectedByValidationFlow) {
    auto tsOptions = makeTimeseriesOptions("time", std::string_view("hostId"));

    auto invalidKey = BSON("_id" << 1);
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, invalidKey));
    ASSERT_THROWS_CODE(shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, invalidKey),
                       AssertionException,
                       5914001);

    auto userFacingKey = BSON("hostId" << 1);
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, userFacingKey));
    auto result = shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, userFacingKey);
    ASSERT_BSONOBJ_EQ(result, BSON("meta" << 1));

    auto translatedKey = BSON("meta" << 1);
    ASSERT_TRUE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, translatedKey));
}

}  // namespace
}  // namespace mongo
