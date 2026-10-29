// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/global_catalog/ddl/add_shard_to_zone_gen.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {

namespace {

TEST(AddShardToZone, BasicValidMongosCommand) {
    auto cmdObj = fromjson(R"BSON({
        addShardToZone: "a",
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("AddShardToZone");
    auto parsedRequest = AddShardToZone::parse(cmdObj, ctx);
    ASSERT_EQ("a", parsedRequest.getCommandParameter());
    ASSERT_EQ("z", parsedRequest.getZone());
}

TEST(AddShardToZone, MissingShardNameErrors) {
    auto cmdObj = fromjson(R"BSON({
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("AddShardToZone");
    ASSERT_THROWS_CODE(
        AddShardToZone::parse(cmdObj, ctx), mongo::DBException, ErrorCodes::IDLFailedToParse);
}

TEST(AddShardToZone, MissingZoneErrors) {
    auto cmdObj = fromjson(R"BSON({
        addShardToZone: "a",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("AddShardToZone");
    ASSERT_THROWS_CODE(
        AddShardToZone::parse(cmdObj, ctx), mongo::DBException, ErrorCodes::IDLFailedToParse);
}

TEST(AddShardToZone, WrongShardNameTypeErrors) {
    auto cmdObj = fromjson(R"BSON({
        addShardToZone: 1234,
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("AddShardToZone");
    ASSERT_THROWS_CODE(
        AddShardToZone::parse(cmdObj, ctx), mongo::DBException, ErrorCodes::TypeMismatch);
}

TEST(AddShardToZone, WrongZoneTypeErrors) {
    auto cmdObj = fromjson(R"BSON({
        addShardToZone: "a",
        zone: 1234,
        $db: "admin"
    })BSON");
    IDLParserContext ctx("AddShardToZone");
    ASSERT_THROWS_CODE(
        AddShardToZone::parse(cmdObj, ctx), mongo::DBException, ErrorCodes::TypeMismatch);
}

TEST(ConfigsvrAddShardToZone, BasicValidConfigsvrCommand) {
    auto cmdObj = fromjson(R"BSON({
        _configsvrAddShardToZone: "a",
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrAddShardToZone");
    auto parsedRequest = ConfigsvrAddShardToZone::parse(cmdObj, ctx);
    ASSERT_EQ("a", parsedRequest.getCommandParameter());
    ASSERT_EQ("z", parsedRequest.getZone());
}

TEST(ConfigsvrAddShardToZone, MissingShardNameErrors) {
    auto cmdObj = fromjson(R"BSON({
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrAddShardToZone");
    ASSERT_THROWS_CODE(ConfigsvrAddShardToZone::parse(cmdObj, ctx),
                       mongo::DBException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(ConfigsvrAddShardToZone, MissingZoneErrors) {
    auto cmdObj = fromjson(R"BSON({
        addShardToZone: "a",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrAddShardToZone");
    ASSERT_THROWS_CODE(ConfigsvrAddShardToZone::parse(cmdObj, ctx),
                       mongo::DBException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(ConfigsvrAddShardToZone, WrongShardNameTypeErrors) {
    auto cmdObj = fromjson(R"BSON({
        _configsvrAddShardToZone: 1234,
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrAddShardToZone");
    ASSERT_THROWS_CODE(
        ConfigsvrAddShardToZone::parse(cmdObj, ctx), mongo::DBException, ErrorCodes::TypeMismatch);
}

TEST(ConfigsvrAddShardToZone, WrongZoneTypeErrors) {
    auto cmdObj = fromjson(R"BSON({
        _configsvrAddShardToZone: "a",
        zone: 1234,
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrAddShardToZone");
    ASSERT_THROWS_CODE(
        ConfigsvrAddShardToZone::parse(cmdObj, ctx), mongo::DBException, ErrorCodes::TypeMismatch);
}

}  // unnamed namespace
}  // namespace mongo
