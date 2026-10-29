// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/s/request_types/update_zone_key_range_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

TEST(UpdateZoneKeyRange, BasicValidMongosAssignCommand) {
    auto cmdObj = fromjson(R"BSON({
        updateZoneKeyRange: "foo.bar",
        min: { x: 1 },
        max: { x: 100 },
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("UpdateZoneKeyRange");
    auto parsedRequest = UpdateZoneKeyRange::parse(cmdObj, ctx);
    ASSERT_EQ("foo.bar", parsedRequest.getCommandParameter().toString_forTest());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1), parsedRequest.getMin());
    ASSERT_BSONOBJ_EQ(BSON("x" << 100), parsedRequest.getMax());
    ASSERT_EQ("z", *parsedRequest.getZone());
}

TEST(UpdateZoneKeyRange, BasicValidMongosAssignCommandRemoveZone) {
    auto cmdObj = fromjson(R"BSON({
        updateZoneKeyRange: "foo.bar",
        min: { x: 1 },
        max: { x: 100 },
        zone: null,
        $db: "admin"
    })BSON");
    IDLParserContext ctx("UpdateZoneKeyRange");
    auto parsedRequest = UpdateZoneKeyRange::parse(cmdObj, ctx);
    ASSERT_EQ("foo.bar", parsedRequest.getCommandParameter().toString_forTest());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1), parsedRequest.getMin());
    ASSERT_BSONOBJ_EQ(BSON("x" << 100), parsedRequest.getMax());
    ASSERT_EQ(boost::none, parsedRequest.getZone());
}

TEST(UpdateZoneKeyRange, MissingMinErrors) {
    auto cmdObj = fromjson(R"BSON({
        updateZoneKeyRange: "foo.bar",
        max: { x: 100 },
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("UpdateZoneKeyRange");
    ASSERT_THROWS_CODE(
        UpdateZoneKeyRange::parse(cmdObj, ctx), mongo::DBException, ErrorCodes::IDLFailedToParse);
}

TEST(UpdateZoneKeyRange, MissingMaxErrors) {
    auto cmdObj = fromjson(R"BSON({
        updateZoneKeyRange: "foo.bar",
        min: { x: 1 },
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("UpdateZoneKeyRange");
    ASSERT_THROWS_CODE(
        UpdateZoneKeyRange::parse(cmdObj, ctx), mongo::DBException, ErrorCodes::IDLFailedToParse);
}

TEST(UpdateZoneKeyRange, MissingZoneErrors) {
    auto cmdObj = fromjson(R"BSON({
        updateZoneKeyRange: "foo.bar",
        min: { x: 1 },
        max: { x: 100 },
        $db: "admin"
    })BSON");
    IDLParserContext ctx("UpdateZoneKeyRange");
    ASSERT_THROWS_CODE(
        UpdateZoneKeyRange::parse(cmdObj, ctx), mongo::DBException, ErrorCodes::IDLFailedToParse);
}

TEST(UpdateZoneKeyRange, MissingShardNameErrors) {
    auto cmdObj = fromjson(R"BSON({
        min: { x: 1 },
        max: { x: 100 },
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("UpdateZoneKeyRange");
    ASSERT_THROWS_CODE(
        UpdateZoneKeyRange::parse(cmdObj, ctx), mongo::DBException, ErrorCodes::IDLFailedToParse);
}

TEST(UpdateZoneKeyRange, WrongShardNameTypeErrors) {
    auto cmdObj = fromjson(R"BSON({
        updateZoneKeyRange: 1234,
        min: { x: 1 },
        max: { x: 100 },
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("UpdateZoneKeyRange");
    ASSERT_THROWS_CODE(
        UpdateZoneKeyRange::parse(cmdObj, ctx), mongo::DBException, ErrorCodes::TypeMismatch);
}

TEST(UpdateZoneKeyRange, WrongMinRangeTypeErrors) {
    auto cmdObj = fromjson(R"BSON({
        updateZoneKeyRange: "foo.bar",
        min: 1,
        max: { x: 100 },
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("UpdateZoneKeyRange");
    ASSERT_THROWS_CODE(
        UpdateZoneKeyRange::parse(cmdObj, ctx), mongo::DBException, ErrorCodes::TypeMismatch);
}

TEST(UpdateZoneKeyRange, WrongMaxRangeTypeErrors) {
    auto cmdObj = fromjson(R"BSON({
        updateZoneKeyRange: "foo.bar",
        min: { x: 1 },
        max: 100,
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("UpdateZoneKeyRange");
    ASSERT_THROWS_CODE(
        UpdateZoneKeyRange::parse(cmdObj, ctx), mongo::DBException, ErrorCodes::TypeMismatch);
}

TEST(UpdateZoneKeyRange, WrongZoneNameTypeErrors) {
    auto cmdObj = fromjson(R"BSON({
        updateZoneKeyRange: "foo.bar",
        min: { x: 1 },
        max: { x: 100 },
        zone: 1234,
        $db: "admin"
    })BSON");
    IDLParserContext ctx("UpdateZoneKeyRange");
    ASSERT_THROWS_CODE(
        UpdateZoneKeyRange::parse(cmdObj, ctx), mongo::DBException, ErrorCodes::TypeMismatch);
}

TEST(ConfigsvrUpdateZoneKeyRange, BasicValidConfigsvrAssignCommand) {
    auto cmdObj = fromjson(R"BSON({
        _configsvrUpdateZoneKeyRange: "foo.bar",
        min: { x: 1 },
        max: { x: 100 },
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrUpdateZoneKeyRange");
    auto parsedRequest = ConfigsvrUpdateZoneKeyRange::parse(cmdObj, ctx);
    ASSERT_EQ("foo.bar", parsedRequest.getCommandParameter().toString_forTest());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1), parsedRequest.getMin());
    ASSERT_BSONOBJ_EQ(BSON("x" << 100), parsedRequest.getMax());
    ASSERT_EQ("z", *parsedRequest.getZone());
}

TEST(ConfigsvrUpdateZoneKeyRange, BasicValidConfigsvrAssignCommandRemoveZone) {
    auto cmdObj = fromjson(R"BSON({
        _configsvrUpdateZoneKeyRange: "foo.bar",
        min: { x: 1 },
        max: { x: 100 },
        zone: null,
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrUpdateZoneKeyRange");
    auto parsedRequest = ConfigsvrUpdateZoneKeyRange::parse(cmdObj, ctx);
    ASSERT_EQ("foo.bar", parsedRequest.getCommandParameter().toString_forTest());
    ASSERT_BSONOBJ_EQ(BSON("x" << 1), parsedRequest.getMin());
    ASSERT_BSONOBJ_EQ(BSON("x" << 100), parsedRequest.getMax());
    ASSERT_EQ(boost::none, parsedRequest.getZone());
}

TEST(ConfigsvrUpdateZoneKeyRange, MissingMinErrors) {
    auto cmdObj = fromjson(R"BSON({
        _configsvrUpdateZoneKeyRange: "foo.bar",
        max: { x: 100 },
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrUpdateZoneKeyRange");
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(cmdObj, ctx),
                       mongo::DBException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(ConfigsvrUpdateZoneKeyRange, MissingMaxErrors) {
    auto cmdObj = fromjson(R"BSON({
        _configsvrUpdateZoneKeyRange: "foo.bar",
        min: { x: 1 },
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrUpdateZoneKeyRange");
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(cmdObj, ctx),
                       mongo::DBException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(ConfigsvrUpdateZoneKeyRange, MissingZoneErrors) {
    auto cmdObj = fromjson(R"BSON({
        _configsvrUpdateZoneKeyRange: "foo.bar",
        min: { x: 1 },
        max: { x: 100 },
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrUpdateZoneKeyRange");
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(cmdObj, ctx),
                       mongo::DBException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(ConfigsvrUpdateZoneKeyRange, MissingShardNameErrors) {
    auto cmdObj = fromjson(R"BSON({
        min: { x: 1 },
        max: { x: 100 },
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrUpdateZoneKeyRange");
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(cmdObj, ctx),
                       mongo::DBException,
                       ErrorCodes::IDLFailedToParse);
}

TEST(ConfigsvrUpdateZoneKeyRange, WrongShardNameTypeErrors) {
    auto cmdObj = fromjson(R"BSON({
        _configsvrUpdateZoneKeyRange: 1234,
        min: { x: 1 },
        max: { x: 100 },
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrUpdateZoneKeyRange");
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(cmdObj, ctx),
                       mongo::DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(ConfigsvrUpdateZoneKeyRange, WrongMinRangeTypeErrors) {
    auto cmdObj = fromjson(R"BSON({
        _configsvrUpdateZoneKeyRange: "foo.bar",
        min: 1,
        max: { x: 100 },
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrUpdateZoneKeyRange");
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(cmdObj, ctx),
                       mongo::DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(ConfigsvrUpdateZoneKeyRange, WrongMaxRangeTypeErrors) {
    auto cmdObj = fromjson(R"BSON({
        _configsvrUpdateZoneKeyRange: "foo.bar",
        min: { x: 1 },
        max: 100,
        zone: "z",
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrUpdateZoneKeyRange");
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(cmdObj, ctx),
                       mongo::DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(ConfigsvrUpdateZoneKeyRange, WrongZoneNameTypeErrors) {
    auto cmdObj = fromjson(R"BSON({
        _configsvrUpdateZoneKeyRange: "foo.bar",
        min: { x: 1 },
        max: { x: 100 },
        zone: 1234,
        $db: "admin"
    })BSON");
    IDLParserContext ctx("ConfigsvrUpdateZoneKeyRange");
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(cmdObj, ctx),
                       mongo::DBException,
                       ErrorCodes::TypeMismatch);
}

}  // unnamed namespace
}  // namespace mongo
