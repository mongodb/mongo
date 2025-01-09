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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/s/request_types/update_zone_key_range_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
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
    auto parsedRequest = UpdateZoneKeyRange::parse(ctx, cmdObj);
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
    auto parsedRequest = UpdateZoneKeyRange::parse(ctx, cmdObj);
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
        UpdateZoneKeyRange::parse(ctx, cmdObj), mongo::DBException, ErrorCodes::IDLFailedToParse);
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
        UpdateZoneKeyRange::parse(ctx, cmdObj), mongo::DBException, ErrorCodes::IDLFailedToParse);
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
        UpdateZoneKeyRange::parse(ctx, cmdObj), mongo::DBException, ErrorCodes::IDLFailedToParse);
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
        UpdateZoneKeyRange::parse(ctx, cmdObj), mongo::DBException, ErrorCodes::IDLFailedToParse);
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
        UpdateZoneKeyRange::parse(ctx, cmdObj), mongo::DBException, ErrorCodes::TypeMismatch);
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
        UpdateZoneKeyRange::parse(ctx, cmdObj), mongo::DBException, ErrorCodes::TypeMismatch);
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
        UpdateZoneKeyRange::parse(ctx, cmdObj), mongo::DBException, ErrorCodes::TypeMismatch);
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
        UpdateZoneKeyRange::parse(ctx, cmdObj), mongo::DBException, ErrorCodes::TypeMismatch);
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
    auto parsedRequest = ConfigsvrUpdateZoneKeyRange::parse(ctx, cmdObj);
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
    auto parsedRequest = ConfigsvrUpdateZoneKeyRange::parse(ctx, cmdObj);
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
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(ctx, cmdObj),
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
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(ctx, cmdObj),
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
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(ctx, cmdObj),
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
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(ctx, cmdObj),
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
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(ctx, cmdObj),
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
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(ctx, cmdObj),
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
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(ctx, cmdObj),
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
    ASSERT_THROWS_CODE(ConfigsvrUpdateZoneKeyRange::parse(ctx, cmdObj),
                       mongo::DBException,
                       ErrorCodes::TypeMismatch);
}

}  // unnamed namespace
}  // namespace mongo
