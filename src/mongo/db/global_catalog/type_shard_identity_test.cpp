// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/type_shard_identity.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/client/connection_string.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

TEST(ShardIdentityType, RoundTrip) {
    auto clusterId(OID::gen());
    auto uuid = UUID::gen();
    BSONObjBuilder docBuilder;
    docBuilder.append("_id", "shardIdentity");
    docBuilder.append("shardName", "s1");
    docBuilder.append("clusterId", clusterId);
    docBuilder.append("configsvrConnectionString", "test/a:123");
    uuid.appendToBuilder(&docBuilder, "uuid");
    auto doc = docBuilder.obj();

    auto result = ShardIdentityType::fromShardIdentityDocument(doc);
    ASSERT_OK(result.getStatus());

    auto shardIdentity = result.getValue();
    ASSERT_EQ("test/a:123", shardIdentity.getConfigsvrConnectionString().toString());
    ASSERT_EQ("s1", shardIdentity.getShardName());
    ASSERT_EQ(clusterId, shardIdentity.getClusterId());
    ASSERT_EQ(uuid, shardIdentity.getUuid());

    ASSERT_BSONOBJ_EQ(doc, shardIdentity.toShardIdentityDocument());
}

TEST(ShardIdentityType, ParseMissingUuid) {
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
    ASSERT_FALSE(shardIdentity.getUuid().has_value());
    ASSERT_EQ(clusterId, shardIdentity.getClusterId());

    ASSERT_BSONOBJ_EQ(doc, shardIdentity.toShardIdentityDocument());
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

}  // namespace
}  // namespace mongo
