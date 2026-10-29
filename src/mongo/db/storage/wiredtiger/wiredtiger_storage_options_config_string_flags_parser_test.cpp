// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_storage_options_config_string_flags_parser.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo {
namespace {

static BSONObj makeStorageEngineWithConfigString(std::string_view configString) {
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
