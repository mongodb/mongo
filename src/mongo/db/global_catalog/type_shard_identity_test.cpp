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

#include "mongo/db/global_catalog/type_shard_identity.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

TEST(ShardIdentityType, RoundTrip) {
    auto clusterId(OID::gen());
    auto doc = BSON("_id" << "shardIdentity"
                          << "shardName"
                          << "s1"
                          << "clusterId" << clusterId << "configsvrConnectionString"
                          << "test/a:123");

    auto result = ShardIdentityType::fromShardIdentityDocument(doc);
    ASSERT_OK(result.getStatus());

    auto shardIdentity = result.getValue();
    ASSERT_EQ("test/a:123", shardIdentity.getConfigsvrConnectionString().toString());
    ASSERT_EQ("s1", shardIdentity.getShardName());
    ASSERT_EQ("s1", shardIdentity.getShardRef()->getString());
    ASSERT_EQ(clusterId, shardIdentity.getClusterId());

    ASSERT_BSONOBJ_EQ(doc, shardIdentity.toShardIdentityDocument());
}

TEST(ShardIdentityType, RoundTripWithShardRefString) {
    auto clusterId(OID::gen());
    auto doc = BSON("_id" << "shardIdentity"
                          << "shardName"
                          << "s1"
                          << "shardRef"
                          << "ref"
                          << "clusterId" << clusterId << "configsvrConnectionString"
                          << "test/a:123");

    auto result = ShardIdentityType::fromShardIdentityDocument(doc);
    ASSERT_OK(result.getStatus());

    auto shardIdentity = result.getValue();
    ASSERT_EQ("test/a:123", shardIdentity.getConfigsvrConnectionString().toString());
    ASSERT_EQ("s1", shardIdentity.getShardName());
    ASSERT_EQ("ref", shardIdentity.getShardRef()->getString());
    ASSERT_EQ(clusterId, shardIdentity.getClusterId());

    // toShardIdentityDocument strips shardRef (see SERVER-126210), so compare without it.
    ASSERT_BSONOBJ_EQ(doc.removeField("shardRef"), shardIdentity.toShardIdentityDocument());
}

TEST(ShardIdentityType, RoundTripWithShardRefUUID) {
    auto clusterId(OID::gen());
    auto uuid = UUID::gen();
    BSONObjBuilder builder;
    builder.append("_id", "shardIdentity");
    builder.append("shardName", "s1");
    uuid.appendToBuilder(&builder, "shardRef");
    builder.append("clusterId", clusterId);
    builder.append("configsvrConnectionString", "test/a:123");
    auto doc = builder.obj();

    auto result = ShardIdentityType::fromShardIdentityDocument(doc);
    ASSERT_OK(result.getStatus());

    auto shardIdentity = result.getValue();
    ASSERT_EQ("test/a:123", shardIdentity.getConfigsvrConnectionString().toString());
    ASSERT_EQ("s1", shardIdentity.getShardName());
    ASSERT_EQ(uuid, shardIdentity.getShardRef()->getUUID());
    ASSERT_EQ(clusterId, shardIdentity.getClusterId());

    // toShardIdentityDocument strips shardRef (see SERVER-126210), so compare without it.
    ASSERT_BSONOBJ_EQ(doc.removeField("shardRef"), shardIdentity.toShardIdentityDocument());
}

TEST(ShardIdentityType, ParseMissingId) {
    auto doc = BSON("configsvrConnectionString" << "test/a:123"
                                                << "shardName"
                                                << "s1"
                                                << "clusterId" << OID::gen());

    auto result = ShardIdentityType::fromShardIdentityDocument(doc);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ShardIdentityType, ParseMissingConfigsvrConnString) {
    auto doc = BSON("_id" << "shardIdentity"
                          << "shardName"
                          << "s1"
                          << "clusterId" << OID::gen());

    auto result = ShardIdentityType::fromShardIdentityDocument(doc);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ShardIdentityType, ParseMissingShardName) {
    auto doc = BSON("_id" << "shardIdentity"
                          << "configsvrConnectionString"
                          << "test/a:123"
                          << "clusterId" << OID::gen());

    auto result = ShardIdentityType::fromShardIdentityDocument(doc);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ShardIdentityType, ParseMissingClusterId) {
    auto doc = BSON("_id" << "shardIdentity"
                          << "configsvrConnectionString"
                          << "test/a:123"
                          << "shardName"
                          << "s1");

    auto result = ShardIdentityType::fromShardIdentityDocument(doc);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(ShardIdentityType, InvalidConnectionString) {
    auto clusterId(OID::gen());
    auto doc = BSON("_id" << "shardIdentity"
                          << "configsvrConnectionString"
                          << "test/,,,"
                          << "shardName"
                          << "s1"
                          << "clusterId" << clusterId);

    ASSERT_EQ(ErrorCodes::FailedToParse,
              ShardIdentityType::fromShardIdentityDocument(doc).getStatus());
}

TEST(ShardIdentityType, NonReplSetConnectionString) {
    auto clusterId(OID::gen());
    auto doc = BSON("_id" << "shardIdentity"
                          << "configsvrConnectionString"
                          << "local:123"
                          << "shardName"
                          << "s1"
                          << "clusterId" << clusterId);

    ASSERT_EQ(ErrorCodes::UnsupportedFormat,
              ShardIdentityType::fromShardIdentityDocument(doc).getStatus());
}

TEST(ShardIdentityType, CreateUpdateObject) {
    auto updateObj = ShardIdentityType::createConfigServerUpdateObject("test/a:1,b:2");
    auto expectedObj = BSON("$set" << BSON("configsvrConnectionString" << "test/a:1,b:2"));
    ASSERT_BSONOBJ_EQ(expectedObj, updateObj);
}

// Helpers for validate() tests that need a specific cluster role.
namespace {
struct ScopedClusterRole {
    explicit ScopedClusterRole(ClusterRole role) : _saved(serverGlobalParams.clusterRole) {
        serverGlobalParams.clusterRole = role;
    }
    ~ScopedClusterRole() {
        serverGlobalParams.clusterRole = _saved;
    }
    ClusterRole _saved;
};
}  // namespace

TEST(ShardIdentityType, ValidateShardRefConfigServerStringConfig) {
    ScopedClusterRole role({ClusterRole::ShardServer, ClusterRole::ConfigServer});
    auto clusterId(OID::gen());
    auto doc = BSON("_id" << "shardIdentity"
                          << "shardName"
                          << "config"
                          << "shardRef"
                          << "config"
                          << "clusterId" << clusterId << "configsvrConnectionString"
                          << "test/a:123");
    auto result = ShardIdentityType::fromShardIdentityDocument(doc);
    ASSERT_OK(result.getStatus());
    ASSERT_OK(result.getValue().validate());
}

TEST(ShardIdentityType, ValidateShardRefConfigServerWrongString) {
    ScopedClusterRole role({ClusterRole::ShardServer, ClusterRole::ConfigServer});
    auto clusterId(OID::gen());
    auto doc = BSON("_id" << "shardIdentity"
                          << "shardName"
                          << "config"
                          << "shardRef"
                          << "other"
                          << "clusterId" << clusterId << "configsvrConnectionString"
                          << "test/a:123");
    auto result = ShardIdentityType::fromShardIdentityDocument(doc);
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(ErrorCodes::UnsupportedFormat, result.getValue().validate());
}

TEST(ShardIdentityType, ValidateShardRefConfigServerUUID) {
    ScopedClusterRole role({ClusterRole::ShardServer, ClusterRole::ConfigServer});
    auto clusterId(OID::gen());
    auto uuid = UUID::gen();
    BSONObjBuilder builder;
    builder.append("_id", "shardIdentity");
    builder.append("shardName", "config");
    uuid.appendToBuilder(&builder, "shardRef");
    builder.append("clusterId", clusterId);
    builder.append("configsvrConnectionString", "test/a:123");
    auto doc = builder.obj();
    auto result = ShardIdentityType::fromShardIdentityDocument(doc);
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(ErrorCodes::UnsupportedFormat, result.getValue().validate());
}

TEST(ShardIdentityType, ValidateShardRefShardServerCannotBeConfig) {
    ScopedClusterRole role(ClusterRole::ShardServer);
    auto clusterId(OID::gen());
    auto doc = BSON("_id" << "shardIdentity"
                          << "shardName"
                          << "s1"
                          << "shardRef" << ShardId::kConfigServerId.toString() << "clusterId"
                          << clusterId << "configsvrConnectionString"
                          << "test/a:123");
    auto result = ShardIdentityType::fromShardIdentityDocument(doc);
    ASSERT_OK(result.getStatus());
    ASSERT_EQ(ErrorCodes::UnsupportedFormat, result.getValue().validate());
}

TEST(ShardIdentityType, ValidateShardRefShardServerUUIDIsValid) {
    ScopedClusterRole role(ClusterRole::ShardServer);
    auto clusterId(OID::gen());
    auto uuid = UUID::gen();
    BSONObjBuilder builder;
    builder.append("_id", "shardIdentity");
    builder.append("shardName", "s1");
    uuid.appendToBuilder(&builder, "shardRef");
    builder.append("clusterId", clusterId);
    builder.append("configsvrConnectionString", "test/a:123");
    auto doc = builder.obj();
    auto result = ShardIdentityType::fromShardIdentityDocument(doc);
    ASSERT_OK(result.getStatus());
    ASSERT_OK(result.getValue().validate());
}

}  // namespace
}  // namespace mongo
