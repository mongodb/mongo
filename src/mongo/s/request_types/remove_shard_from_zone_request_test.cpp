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

#include "mongo/s/request_types/remove_shard_from_zone_request_type.h"

#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(RemoveShardFromZoneRequest, BasicValidMongosCommand) {
    auto requestStatus =
        RemoveShardFromZoneRequest::parseFromMongosCommand(BSON("removeShardFromZone"
                                                                << "a"
                                                                << "zone"
                                                                << "z"));
    ASSERT_OK(requestStatus.getStatus());

    auto request = requestStatus.getValue();
    ASSERT_EQ("a", request.getShardName());
    ASSERT_EQ("z", request.getZoneName());
}

TEST(RemoveShardFromZoneRequest, CommandBuilderShouldAlwaysCreateConfigCommand) {
    auto requestStatus =
        RemoveShardFromZoneRequest::parseFromMongosCommand(BSON("removeShardFromZone"
                                                                << "a"
                                                                << "zone"
                                                                << "z"));
    ASSERT_OK(requestStatus.getStatus());

    auto request = requestStatus.getValue();

    BSONObjBuilder builder;
    request.appendAsConfigCommand(&builder);
    auto cmdObj = builder.obj();

    ASSERT_EQ(BSON("_configsvrRemoveShardFromZone"
                   << "a"
                   << "zone"
                   << "z"),
              cmdObj);
}

TEST(RemoveShardFromZoneRequest, MissingZoneErrors) {
    auto request = RemoveShardFromZoneRequest::parseFromMongosCommand(BSON("removeShardFromZone"
                                                                           << "a"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(RemoveShardFromZoneRequest, MissingShardNameErrors) {
    auto request = RemoveShardFromZoneRequest::parseFromMongosCommand(BSON("zone"
                                                                           << "z"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(RemoveShardFromZoneRequest, WrongShardNameTypeErrors) {
    auto request = RemoveShardFromZoneRequest::parseFromMongosCommand(
        BSON("removeShardFromZone" << 1234 << "zone"
                                   << "z"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(RemoveShardFromZoneRequest, WrongZoneNameTypeErrors) {
    auto request = RemoveShardFromZoneRequest::parseFromMongosCommand(BSON("removeShardFromZone"
                                                                           << "a"
                                                                           << "zone"
                                                                           << 1234));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(RemoveShardFromZoneRequest, CannotUseMongosToParseConfigCommand) {
    auto request =
        RemoveShardFromZoneRequest::parseFromMongosCommand(BSON("_configsvrRemoveShardFromZone"
                                                                << "a"
                                                                << "zone"
                                                                << "z"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(CfgRemoveShardFromZoneRequest, BasicValidConfigCommand) {
    auto requestStatus =
        RemoveShardFromZoneRequest::parseFromConfigCommand(BSON("_configsvrRemoveShardFromZone"
                                                                << "a"
                                                                << "zone"
                                                                << "z"));
    ASSERT_OK(requestStatus.getStatus());

    auto request = requestStatus.getValue();
    ASSERT_EQ("a", request.getShardName());
    ASSERT_EQ("z", request.getZoneName());

    BSONObjBuilder builder;
    request.appendAsConfigCommand(&builder);
    auto cmdObj = builder.obj();

    ASSERT_EQ(BSON("_configsvrRemoveShardFromZone"
                   << "a"
                   << "zone"
                   << "z"),
              cmdObj);
}

TEST(CfgRemoveShardFromZoneRequest, MissingZoneErrors) {
    auto request =
        RemoveShardFromZoneRequest::parseFromConfigCommand(BSON("_configsvrRemoveShardFromZone"
                                                                << "a"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(CfgRemoveShardFromZoneRequest, MissingShardNameErrors) {
    auto request = RemoveShardFromZoneRequest::parseFromConfigCommand(BSON("zone"
                                                                           << "z"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(CfgRemoveShardFromZoneRequest, WrongShardNameTypeErrors) {
    auto request = RemoveShardFromZoneRequest::parseFromConfigCommand(
        BSON("_configsvrRemoveShardFromZone" << 1234 << "zone"
                                             << "z"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(CfgRemoveShardFromZoneRequest, WrongZoneNameTypeErrors) {
    auto request =
        RemoveShardFromZoneRequest::parseFromConfigCommand(BSON("_configsvrRemoveShardFromZone"
                                                                << "a"
                                                                << "zone"
                                                                << 1234));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(CfgRemoveShardFromZoneRequest, CannotUseConfigToParseMongosCommand) {
    auto request = RemoveShardFromZoneRequest::parseFromConfigCommand(BSON("removeShardFromZone"
                                                                           << "a"
                                                                           << "zone"
                                                                           << 1234));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

}  // unnamed namespace
}  // namespace mongo
