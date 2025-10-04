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

#include "mongo/db/storage/wiredtiger/wiredtiger_storage_options_config_string_flags_parser.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

static BSONObj makeStorageEngineWithConfigString(StringData configString) {
    return BSON(kWiredTigerEngineName << BSON(WiredTigerUtil::kConfigStringField << configString));
}

static BSONObj addExtraFields(const BSONObj& storageEngineOptions) {
    auto wtObj = storageEngineOptions[kWiredTigerEngineName].Obj();
    return storageEngineOptions.addFields(BSON("dummy1"
                                               << "value1" << kWiredTigerEngineName
                                               << wtObj.addFields(BSON("dummy2" << "value2"))));
}

TEST(StorageEngineFlagsParserTest, GetEmptyOptionalWhenNoWiredTigerConfigString) {
    auto options = BSONObj();

    auto flag = getFlagFromWiredTigerStorageOptions(options, "flagA");

    ASSERT_EQ(boost::none, flag);
}

TEST(StorageEngineFlagsParserTest, GetEmptyOptionalWhenOptionsDoesNotContainMetadata) {
    auto options = makeStorageEngineWithConfigString("access_pattern_hint=random");

    auto flag = getFlagFromWiredTigerStorageOptions(options, "flagA");

    ASSERT_EQ(boost::none, flag);
}

TEST(StorageEngineFlagsParserTest, GetEmptyOptionalWhenMetadataDoesNotContainTheFlag) {
    auto options = makeStorageEngineWithConfigString("app_metadata=(formatVersion=1)");

    auto flags = getFlagsFromWiredTigerStorageOptions(options, {"flagA", "flagB"});

    ASSERT_EQ(boost::none, flags["flagA"]);
    ASSERT_EQ(boost::none, flags["flagB"]);
}

TEST(StorageEngineFlagsParserTest, GetValueWhenMetadataContainsASingleFlag) {
    auto options = makeStorageEngineWithConfigString("app_metadata=(formatVersion=1,flagA=true)");

    auto flags = getFlagsFromWiredTigerStorageOptions(options, {"flagA", "flagB"});

    ASSERT_EQ(true, flags["flagA"]);
    ASSERT_EQ(boost::none, flags["flagB"]);
}

TEST(StorageEngineFlagsParserTest, GetValueWhenMetadataContainsMultipleFlags) {
    auto options = makeStorageEngineWithConfigString("app_metadata=(flagB=true,flagA=false)");

    auto flags = getFlagsFromWiredTigerStorageOptions(options, {"flagA", "flagB"});

    ASSERT_EQ(false, flags["flagA"]);
    ASSERT_EQ(true, flags["flagB"]);
}

TEST(StorageEngineFlagsParserTest, GetEmptyOptionalWhenMetadataContainsAnInvalidValue) {
    auto options =
        makeStorageEngineWithConfigString("app_metadata=(flagB=(hello=world),flagA=true)");

    auto flags = getFlagsFromWiredTigerStorageOptions(options, {"flagA", "flagB"});

    ASSERT_EQ(true, flags["flagA"]);
    ASSERT_EQ(boost::none, flags["flagB"]);
}

TEST(StorageEngineFlagsParserTest, GetTrueWhenMetadataContainsAKeyWithNoValue) {
    auto options = makeStorageEngineWithConfigString("app_metadata=(formatVersion=1,flagA)");

    auto flag = getFlagFromWiredTigerStorageOptions(options, "flagA");

    ASSERT_EQ(true, flag);
}

TEST(StorageEngineFlagsParserTest, GetIgnoresUnknownStorageEngineFields) {
    auto options = addExtraFields(makeStorageEngineWithConfigString("app_metadata=(flagA=true)"));

    auto flag = getFlagFromWiredTigerStorageOptions(options, "flagA");

    ASSERT_EQ(true, flag);
}

TEST(StorageEngineFlagsParserTest, GetHandlesTrickyFormatting) {
    auto options = addExtraFields(makeStorageEngineWithConfigString(
        "   access_pattern_hint   =    random   ,    \"app_metadata\"    :   [    x=y   ,    "
        "\"flagB\":   true   ,   z :   t  ]"));

    auto flags = getFlagsFromWiredTigerStorageOptions(options, {"flagA", "flagB"});

    ASSERT_EQ(boost::none, flags["flagA"]);
    ASSERT_EQ(true, flags["flagB"]);
}

TEST(StorageEngineFlagsParserTest, AddFlagToEmptyStorageEngineBson) {
    auto options = BSONObj();

    auto newOptions = setFlagToWiredTigerStorageOptions(options, "flagA", true);

    ASSERT_BSONOBJ_EQ(newOptions, makeStorageEngineWithConfigString("app_metadata=(flagA=true)"));
}

TEST(StorageEngineFlagsParserTest, AddFlagToEmptyConfigString) {
    auto options = makeStorageEngineWithConfigString("");

    auto newOptions = setFlagToWiredTigerStorageOptions(options, "flagA", true);

    ASSERT_BSONOBJ_EQ(newOptions, makeStorageEngineWithConfigString("app_metadata=(flagA=true)"));
}

TEST(StorageEngineFlagsParserTest, AddFlagToExistingConfigStringWithNoMetadata) {
    auto options = makeStorageEngineWithConfigString("access_pattern_hint=random");

    auto newOptions = setFlagToWiredTigerStorageOptions(options, "flagA", true);

    ASSERT_BSONOBJ_EQ(
        newOptions,
        makeStorageEngineWithConfigString("access_pattern_hint=random,app_metadata=(flagA=true)"));
}

TEST(StorageEngineFlagsParserTest, AddFlagToExistingConfigStringWithEmptyMetadata) {
    auto options = makeStorageEngineWithConfigString("access_pattern_hint=random,app_metadata=()");

    auto newOptions = setFlagToWiredTigerStorageOptions(options, "flagA", false);

    ASSERT_BSONOBJ_EQ(
        newOptions,
        makeStorageEngineWithConfigString("access_pattern_hint=random,app_metadata=(flagA=false)"));
}

TEST(StorageEngineFlagsParserTest, AddFlagToExistingConfigStringWithOtherFlags) {
    auto options = makeStorageEngineWithConfigString(
        "access_pattern_hint=random,app_metadata=(hello2=world2,flagB=true)");

    auto newOptions = setFlagToWiredTigerStorageOptions(options, "flagA", false);

    ASSERT_BSONOBJ_EQ(
        newOptions,
        makeStorageEngineWithConfigString(
            "access_pattern_hint=random,app_metadata=(flagA=false,hello2=world2,flagB=true)"));
}

TEST(StorageEngineFlagsParserTest, SetExistingFlag) {
    auto options =
        makeStorageEngineWithConfigString("access_pattern_hint=random,app_metadata=(flagA=false)");

    auto newOptions = setFlagToWiredTigerStorageOptions(options, "flagA", true);

    ASSERT_BSONOBJ_EQ(
        newOptions,
        makeStorageEngineWithConfigString("access_pattern_hint=random,app_metadata=(flagA=true)"));
}

TEST(StorageEngineFlagsParserTest, RemoveExistingFlag) {
    auto options = makeStorageEngineWithConfigString(
        "access_pattern_hint=random,app_metadata=(flagB=true,x=y,z=t)");

    auto newOptions = setFlagToWiredTigerStorageOptions(options, "flagB", boost::none);

    ASSERT_BSONOBJ_EQ(
        newOptions,
        makeStorageEngineWithConfigString("access_pattern_hint=random,app_metadata=(x=y,z=t)"));
}

TEST(StorageEngineFlagsParserTest, SetMultipleFlags) {
    auto options = makeStorageEngineWithConfigString(
        "access_pattern_hint=random,app_metadata=(x=y,flagB=true,z=t,flagC=true)");

    auto newOptions = setFlagsToWiredTigerStorageOptions(
        options, {{"flagB", false}, {"flagA", true}, {"flagC", boost::none}});

    ASSERT_BSONOBJ_EQ(
        newOptions,
        makeStorageEngineWithConfigString(
            "access_pattern_hint=random,app_metadata=(flagA=true,x=y,flagB=false,z=t)"));
}

TEST(StorageEngineFlagsParserTest, SetFlagWhenMetadataContainsAKeyWithNoValue) {
    auto options = makeStorageEngineWithConfigString("app_metadata=(formatVersion=1,flagA)");

    auto newOptions = setFlagToWiredTigerStorageOptions(options, "flagA", false);

    ASSERT_BSONOBJ_EQ(
        newOptions,
        makeStorageEngineWithConfigString("app_metadata=(formatVersion=1,flagA=false)"));
}

TEST(StorageEngineFlagsParserTest, SetPreservesUnknownStorageEngineFields) {
    auto options = addExtraFields(makeStorageEngineWithConfigString(
        "access_pattern_hint=random,app_metadata=(x=y,flagB=false,z=t)"));

    auto newOptions = setFlagToWiredTigerStorageOptions(options, "flagB", true);

    auto expected = addExtraFields(makeStorageEngineWithConfigString(
        "access_pattern_hint=random,app_metadata=(x=y,flagB=true,z=t)"));
    ASSERT_BSONOBJ_EQ(newOptions, expected);
}

TEST(StorageEngineFlagsParserTest, SetHandlesTrickyFormatting) {
    auto options = addExtraFields(makeStorageEngineWithConfigString(
        "   access_pattern_hint   =    random   ,    \"app_metadata\"    :   [    x=y   ,    "
        "\"flagB\":   false   ,   z :   t   , flagC    :  true ]"));

    auto newOptions = setFlagsToWiredTigerStorageOptions(
        options, {{"flagA", false}, {"flagB", true}, {"flagC", boost::none}});

    auto expected = addExtraFields(makeStorageEngineWithConfigString(
        "   access_pattern_hint   =    random   ,    \"app_metadata\"    :   [    flagA=false,x=y  "
        " ,flagB=true,   z :   t   ]"));
    ASSERT_BSONOBJ_EQ(newOptions, expected);
}

}  // namespace
}  // namespace mongo
