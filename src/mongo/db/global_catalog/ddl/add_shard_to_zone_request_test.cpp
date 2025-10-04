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
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
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
