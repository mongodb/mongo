// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/ddl/remove_shard_from_zone_request_type.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {

namespace {

TEST(RemoveShardFromZoneRequest, BasicValidMongosCommand) {
    auto requestStatus =
        RemoveShardFromZoneRequest::parseFromMongosCommand(BSON("removeShardFromZone" << "a"
                                                                                      << "zone"
                                                                                      << "z"));
    ASSERT_OK(requestStatus.getStatus());

    auto request = requestStatus.getValue();
    ASSERT_EQ("a", request.getShardName());
    ASSERT_EQ("z", request.getZoneName());
}

TEST(RemoveShardFromZoneRequest, CommandBuilderShouldAlwaysCreateConfigCommand) {
    auto requestStatus =
        RemoveShardFromZoneRequest::parseFromMongosCommand(BSON("removeShardFromZone" << "a"
                                                                                      << "zone"
                                                                                      << "z"));
    ASSERT_OK(requestStatus.getStatus());

    auto request = requestStatus.getValue();

    BSONObjBuilder builder;
    request.appendAsConfigCommand(&builder);
    auto cmdObj = builder.obj();

    ASSERT_BSONOBJ_EQ(BSON("_configsvrRemoveShardFromZone" << "a"
                                                           << "zone"
                                                           << "z"),
                      cmdObj);
}

TEST(RemoveShardFromZoneRequest, MissingZoneErrors) {
    auto request =
        RemoveShardFromZoneRequest::parseFromMongosCommand(BSON("removeShardFromZone" << "a"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(RemoveShardFromZoneRequest, MissingShardNameErrors) {
    auto request = RemoveShardFromZoneRequest::parseFromMongosCommand(BSON("zone" << "z"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(RemoveShardFromZoneRequest, WrongShardNameTypeErrors) {
    auto request = RemoveShardFromZoneRequest::parseFromMongosCommand(BSON("removeShardFromZone"
                                                                           << 1234 << "zone"
                                                                           << "z"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(RemoveShardFromZoneRequest, WrongZoneNameTypeErrors) {
    auto request = RemoveShardFromZoneRequest::parseFromMongosCommand(BSON("removeShardFromZone"
                                                                           << "a"
                                                                           << "zone" << 1234));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(RemoveShardFromZoneRequest, CannotUseMongosToParseConfigCommand) {
    auto request = RemoveShardFromZoneRequest::parseFromMongosCommand(
        BSON("_configsvrRemoveShardFromZone" << "a"
                                             << "zone"
                                             << "z"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(CfgRemoveShardFromZoneRequest, BasicValidConfigCommand) {
    auto requestStatus = RemoveShardFromZoneRequest::parseFromConfigCommand(
        BSON("_configsvrRemoveShardFromZone" << "a"
                                             << "zone"
                                             << "z"));
    ASSERT_OK(requestStatus.getStatus());

    auto request = requestStatus.getValue();
    ASSERT_EQ("a", request.getShardName());
    ASSERT_EQ("z", request.getZoneName());

    BSONObjBuilder builder;
    request.appendAsConfigCommand(&builder);
    auto cmdObj = builder.obj();

    ASSERT_BSONOBJ_EQ(BSON("_configsvrRemoveShardFromZone" << "a"
                                                           << "zone"
                                                           << "z"),
                      cmdObj);
}

TEST(CfgRemoveShardFromZoneRequest, MissingZoneErrors) {
    auto request = RemoveShardFromZoneRequest::parseFromConfigCommand(
        BSON("_configsvrRemoveShardFromZone" << "a"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(CfgRemoveShardFromZoneRequest, MissingShardNameErrors) {
    auto request = RemoveShardFromZoneRequest::parseFromConfigCommand(BSON("zone" << "z"));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

TEST(CfgRemoveShardFromZoneRequest, WrongShardNameTypeErrors) {
    auto request = RemoveShardFromZoneRequest::parseFromConfigCommand(
        BSON("_configsvrRemoveShardFromZone" << 1234 << "zone"
                                             << "z"));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(CfgRemoveShardFromZoneRequest, WrongZoneNameTypeErrors) {
    auto request = RemoveShardFromZoneRequest::parseFromConfigCommand(
        BSON("_configsvrRemoveShardFromZone" << "a"
                                             << "zone" << 1234));
    ASSERT_EQ(ErrorCodes::TypeMismatch, request.getStatus());
}

TEST(CfgRemoveShardFromZoneRequest, CannotUseConfigToParseMongosCommand) {
    auto request = RemoveShardFromZoneRequest::parseFromConfigCommand(BSON("removeShardFromZone"
                                                                           << "a"
                                                                           << "zone" << 1234));
    ASSERT_EQ(ErrorCodes::NoSuchKey, request.getStatus());
}

}  // unnamed namespace
}  // namespace mongo
