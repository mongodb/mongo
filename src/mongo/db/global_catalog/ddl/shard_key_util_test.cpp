/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/shard_key_util.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TimeseriesOptions makeTimeseriesOptions(StringData timeField,
                                        boost::optional<StringData> metaField) {
    TimeseriesOptions options{std::string{timeField}};
    if (metaField) {
        options.setMetaField(*metaField);
    }
    return options;
}

// Tests for isRawTimeseriesShardKey

TEST(IsRawTimeseriesShardKeyTest, UserFacingTimeFieldKeyIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("time" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, UserFacingMetaFieldKeyIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("hostId" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, UserFacingMetaSubfieldKeyIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("hostId.x" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, UserFacingCompoundKeyIsNotTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    ASSERT_FALSE(
        shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("hostId" << 1 << "time" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, TranslatedMetaFieldKeyIsTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    ASSERT_TRUE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("meta" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, TranslatedMetaSubfieldKeyIsTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    ASSERT_TRUE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("meta.x" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, TranslatedTimeFieldKeyIsTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    ASSERT_TRUE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("control.min.time" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, TranslatedCompoundKeyIsTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    ASSERT_TRUE(shardkeyutil::isRawTimeseriesShardKey(
        tsOptions, BSON("meta" << 1 << "control.min.time" << 1)));
}

TEST(IsRawTimeseriesShardKeyTest, MetaFieldNamedMetaIsHandledCorrectly) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("meta"));
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
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    ASSERT_FALSE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("hostId.x" << "hashed")));
}

TEST(IsRawTimeseriesShardKeyTest, TranslatedHashedMetaFieldKeyIsTranslated) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    ASSERT_TRUE(shardkeyutil::isRawTimeseriesShardKey(tsOptions, BSON("meta.x" << "hashed")));
}

// Tests for validateAndTranslateTimeseriesShardKey

TEST(ValidateAndTranslateTimeseriesShardKeyTest, MetaFieldKeyTranslatesCorrectly) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    auto result =
        shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, BSON("hostId" << 1));
    ASSERT_BSONOBJ_EQ(result, BSON("meta" << 1));
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, MetaSubfieldKeyTranslatesCorrectly) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    auto result =
        shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, BSON("hostId.x" << 1));
    ASSERT_BSONOBJ_EQ(result, BSON("meta.x" << 1));
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, TimeFieldKeyTranslatesCorrectly) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    auto result =
        shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, BSON("time" << 1));
    ASSERT_BSONOBJ_EQ(result, BSON("control.min.time" << 1));
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, CompoundKeyTranslatesCorrectly) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    auto result = shardkeyutil::validateAndTranslateTimeseriesShardKey(
        tsOptions, BSON("hostId" << 1 << "time" << 1));
    ASSERT_BSONOBJ_EQ(result, BSON("meta" << 1 << "control.min.time" << 1));
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, HashedMetaFieldKeyTranslatesCorrectly) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    auto result = shardkeyutil::validateAndTranslateTimeseriesShardKey(
        tsOptions, BSON("hostId.x" << "hashed"));
    ASSERT_BSONOBJ_EQ(result, BSON("meta.x" << "hashed"));
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, DescendingTimeFieldKeyTranslatesCorrectly) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    auto result =
        shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, BSON("time" << -1));
    ASSERT_BSONOBJ_EQ(result, BSON("control.max.time" << -1 << "control.min.time" << -1));
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, InvalidFieldThrows5914001) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    ASSERT_THROWS_CODE(
        shardkeyutil::validateAndTranslateTimeseriesShardKey(tsOptions, BSON("_id" << 1)),
        AssertionException,
        5914001);
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, InvalidArbitraryFieldThrows5914001) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    ASSERT_THROWS_CODE(shardkeyutil::validateAndTranslateTimeseriesShardKey(
                           tsOptions, BSON("someRandomField" << 1)),
                       AssertionException,
                       5914001);
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, TimeFieldNotAtEndThrows5914000) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
    ASSERT_THROWS_CODE(shardkeyutil::validateAndTranslateTimeseriesShardKey(
                           tsOptions, BSON("time" << 1 << "hostId" << 1)),
                       AssertionException,
                       5914000);
}

TEST(ValidateAndTranslateTimeseriesShardKeyTest, TimeFieldWithHashedThrows880031) {
    auto tsOptions = makeTimeseriesOptions("time", StringData("hostId"));
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

}  // namespace
}  // namespace mongo
