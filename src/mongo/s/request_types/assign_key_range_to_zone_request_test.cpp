/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/request_types/assign_key_range_to_zone_request_type.h"

#include "mongo/bson/json.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(AssignKeyRangeToZoneRequest, BasicValidMongosAssignCommand) {
    auto requestStatus = AssignKeyRangeToZoneRequest::parseFromMongosCommand(fromjson(R"BSON({
            assignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            max: { x: 100 },
            zone: "z"
        })BSON"));
    ASSERT_OK(requestStatus.getStatus());

    auto request = requestStatus.getValue();
    ASSERT_EQ("foo.bar", request.getNS().ns());
    ASSERT_EQ(BSON("x" << 1), request.getRange().getMin());
    ASSERT_EQ(BSON("x" << 100), request.getRange().getMax());
    ASSERT_FALSE(request.isRemove());
    ASSERT_EQ("z", request.getZoneName());
}

TEST(AssignKeyRangeToZoneRequest, BasicValidMongosRemoveCommand) {
    auto requestStatus = AssignKeyRangeToZoneRequest::parseFromMongosCommand(fromjson(R"BSON({
            assignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            max: { x: 100 },
            zone: null
        })BSON"));
    ASSERT_OK(requestStatus.getStatus());

    auto request = requestStatus.getValue();
    ASSERT_EQ("foo.bar", request.getNS().ns());
    ASSERT_EQ(BSON("x" << 1), request.getRange().getMin());
    ASSERT_EQ(BSON("x" << 100), request.getRange().getMax());
    ASSERT_TRUE(request.isRemove());
}

TEST(AssignKeyRangeToZoneRequest, InvalidNSMongosAssignCommand) {
    auto requestStatus = AssignKeyRangeToZoneRequest::parseFromMongosCommand(fromjson(R"BSON({
            assignKeyRangeToZone: "foo",
            min: { x: 1 },
            max: { x: 100 },
            zone: "z"
        })BSON"));
    ASSERT_EQ(ErrorCodes::InvalidNamespace, requestStatus.getStatus());
}

TEST(AssignKeyRangeToZoneRequest, CommandBuilderShouldAlwaysCreateConfigCommandForAssignType) {
    auto requestStatus = AssignKeyRangeToZoneRequest::parseFromMongosCommand(fromjson(R"BSON({
                assignKeyRangeToZone: "foo.bar",
                min: { x: 1 },
                max: { x: 100 },
                zone: "z"
            })BSON"));
    ASSERT_OK(requestStatus.getStatus());

    auto request = requestStatus.getValue();

    BSONObjBuilder builder;
    request.appendAsConfigCommand(&builder);
    auto configCmdObj = builder.obj();

    auto expectedObj = fromjson(R"BSON({
            _configsvrAssignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            max: { x: 100 },
            zone: "z"
        })BSON");
    ASSERT_EQ(expectedObj, configCmdObj);
}

TEST(AssignKeyRangeToZoneRequest, CommandBuilderShouldAlwaysCreateConfigCommandForRemoveType) {
    auto requestStatus = AssignKeyRangeToZoneRequest::parseFromMongosCommand(fromjson(R"BSON({
                assignKeyRangeToZone: "foo.bar",
                min: { x: 1 },
                max: { x: 100 },
                zone: null
            })BSON"));
    ASSERT_OK(requestStatus.getStatus());

    auto request = requestStatus.getValue();

    BSONObjBuilder builder;
    request.appendAsConfigCommand(&builder);
    auto configCmdObj = builder.obj();

    auto expectedObj = fromjson(R"BSON({
            _configsvrAssignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            max: { x: 100 },
            zone: null
        })BSON");
    ASSERT_EQ(expectedObj, configCmdObj);
}


TEST(AssignKeyRangeToZoneRequest, MissingMinErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromMongosCommand(fromjson(R"BSON({
            assignKeyRangeToZone: "foo.bar",
            max: { x: 100 },
            zone: "z"
        })BSON"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(AssignKeyRangeToZoneRequest, MissingMaxErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromMongosCommand(fromjson(R"BSON({
            assignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            zone: "z"
        })BSON"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(AssignKeyRangeToZoneRequest, MissingZoneErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromMongosCommand(fromjson(R"BSON({
            assignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            max: { x: 100 }
        })BSON"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(AssignKeyRangeToZoneRequest, MissingShardNameErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromMongosCommand(fromjson(R"BSON({
            min: { x: 1 },
            max: { x: 100 },
            zone: "z"
        })BSON"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(AssignKeyRangeToZoneRequest, WrongShardNameTypeErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromMongosCommand(fromjson(R"BSON({
                assignKeyRangeToZone: 1234,
                min: { x: 1 },
                max: { x: 100 },
                zone: "z"
            })BSON"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(AssignKeyRangeToZoneRequest, WrongMinRangeTypeErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromMongosCommand(fromjson(R"BSON({
                assignKeyRangeToZone: "foo.bar",
                min: "1",
                max: { x: 100 },
                zone: "z"
            })BSON"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(AssignKeyRangeToZoneRequest, WrongMaxRangeTypeErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromMongosCommand(fromjson(R"BSON({
                assignKeyRangeToZone: "foo.bar",
                min: { x: 1 },
                max: "x",
                zone: "z"
            })BSON"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(AssignKeyRangeToZoneRequest, WrongZoneNameTypeErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromMongosCommand(fromjson(R"BSON({
            assignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            max: { x: 100 },
            zone: 123
        })BSON"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(AssignKeyRangeToZoneRequest, CannotUseMongosToParseConfigCommand) {
    auto request = AssignKeyRangeToZoneRequest::parseFromMongosCommand(fromjson(R"BSON({
            _configsvrAssignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            max: { x: 100 },
            zone: "z"
        })BSON"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(CfgAssignKeyRangeToZoneRequest, BasicValidMongosAssignCommand) {
    auto requestStatus = AssignKeyRangeToZoneRequest::parseFromConfigCommand(fromjson(R"BSON({
            _configsvrAssignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            max: { x: 100 },
            zone: "z"
        })BSON"));
    ASSERT_OK(requestStatus.getStatus());

    auto request = requestStatus.getValue();
    ASSERT_EQ("foo.bar", request.getNS().ns());
    ASSERT_EQ(BSON("x" << 1), request.getRange().getMin());
    ASSERT_EQ(BSON("x" << 100), request.getRange().getMax());
    ASSERT_FALSE(request.isRemove());
    ASSERT_EQ("z", request.getZoneName());
}

TEST(CfgAssignKeyRangeToZoneRequest, BasicValidMongosRemoveCommand) {
    auto requestStatus = AssignKeyRangeToZoneRequest::parseFromConfigCommand(fromjson(R"BSON({
            _configsvrAssignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            max: { x: 100 },
            zone: null
        })BSON"));
    ASSERT_OK(requestStatus.getStatus());

    auto request = requestStatus.getValue();
    ASSERT_EQ("foo.bar", request.getNS().ns());
    ASSERT_EQ(BSON("x" << 1), request.getRange().getMin());
    ASSERT_EQ(BSON("x" << 100), request.getRange().getMax());
    ASSERT_TRUE(request.isRemove());
}

TEST(CfgAssignKeyRangeToZoneRequest, InvalidNSConfigAssignCommand) {
    auto requestStatus = AssignKeyRangeToZoneRequest::parseFromConfigCommand(fromjson(R"BSON({
            _configsvrAssignKeyRangeToZone: "foo",
            min: { x: 1 },
            max: { x: 100 },
            zone: "z"
        })BSON"));
    ASSERT_EQ(ErrorCodes::InvalidNamespace, requestStatus.getStatus());
}

TEST(CfgAssignKeyRangeToZoneRequest, CommandBuilderShouldAlwaysCreateConfigCommandForAssignType) {
    auto requestStatus = AssignKeyRangeToZoneRequest::parseFromConfigCommand(fromjson(R"BSON({
                _configsvrAssignKeyRangeToZone: "foo.bar",
                min: { x: 1 },
                max: { x: 100 },
                zone: "z"
            })BSON"));
    ASSERT_OK(requestStatus.getStatus());

    auto request = requestStatus.getValue();

    BSONObjBuilder builder;
    request.appendAsConfigCommand(&builder);
    auto configCmdObj = builder.obj();

    auto expectedObj = fromjson(R"BSON({
            _configsvrAssignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            max: { x: 100 },
            zone: "z"
        })BSON");
    ASSERT_EQ(expectedObj, configCmdObj);
}

TEST(CfgAssignKeyRangeToZoneRequest, CommandBuilderShouldAlwaysCreateConfigCommandForRemoveType) {
    auto requestStatus = AssignKeyRangeToZoneRequest::parseFromConfigCommand(fromjson(R"BSON({
                _configsvrAssignKeyRangeToZone: "foo.bar",
                min: { x: 1 },
                max: { x: 100 },
                zone: null
            })BSON"));
    ASSERT_OK(requestStatus.getStatus());

    auto request = requestStatus.getValue();

    BSONObjBuilder builder;
    request.appendAsConfigCommand(&builder);
    auto configCmdObj = builder.obj();

    auto expectedObj = fromjson(R"BSON({
            _configsvrAssignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            max: { x: 100 },
            zone: null
        })BSON");
    ASSERT_EQ(expectedObj, configCmdObj);
}


TEST(CfgAssignKeyRangeToZoneRequest, MissingMinErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromConfigCommand(fromjson(R"BSON({
            _configsvrAssignKeyRangeToZone: "foo.bar",
            max: { x: 100 },
            zone: "z"
        })BSON"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(CfgAssignKeyRangeToZoneRequest, MissingMaxErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromConfigCommand(fromjson(R"BSON({
            _configsvrAssignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            zone: "z"
        })BSON"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(CfgAssignKeyRangeToZoneRequest, MissingZoneErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromConfigCommand(fromjson(R"BSON({
            _configsvrAssignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            max: { x: 100 }
        })BSON"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(CfgAssignKeyRangeToZoneRequest, MissingShardNameErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromConfigCommand(fromjson(R"BSON({
            min: { x: 1 },
            max: { x: 100 },
            zone: "z"
        })BSON"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(CfgAssignKeyRangeToZoneRequest, WrongShardNameTypeErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromConfigCommand(fromjson(R"BSON({
                _configsvrAssignKeyRangeToZone: 1234,
                min: { x: 1 },
                max: { x: 100 },
                zone: "z"
            })BSON"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(CfgAssignKeyRangeToZoneRequest, WrongMinRangeTypeErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromConfigCommand(fromjson(R"BSON({
                _configsvrAssignKeyRangeToZone: "foo.bar",
                min: "1",
                max: { x: 100 },
                zone: "z"
            })BSON"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(CfgAssignKeyRangeToZoneRequest, WrongMaxRangeTypeErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromConfigCommand(fromjson(R"BSON({
                _configsvrAssignKeyRangeToZone: "foo.bar",
                min: { x: 1 },
                max: "x",
                zone: "z"
            })BSON"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(CfgAssignKeyRangeToZoneRequest, WrongZoneNameTypeErrors) {
    auto request = AssignKeyRangeToZoneRequest::parseFromConfigCommand(fromjson(R"BSON({
            _configsvrAssignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            max: { x: 100 },
            zone: 123
        })BSON"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(CfgAssignKeyRangeToZoneRequest, CannotUseConfigToParseMongosCommand) {
    auto request = AssignKeyRangeToZoneRequest::parseFromConfigCommand(fromjson(R"BSON({
            assignKeyRangeToZone: "foo.bar",
            min: { x: 1 },
            max: { x: 100 },
            zone: "z"
        })BSON"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

}  // unnamed namespace
}  // namespace mongo
