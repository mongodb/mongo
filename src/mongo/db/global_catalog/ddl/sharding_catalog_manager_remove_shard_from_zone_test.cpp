// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

namespace mongo {
namespace {


ReadPreferenceSetting kReadPref(ReadPreference::PrimaryOnly);

using RemoveShardFromZoneTest = ConfigServerTestFixture;

TEST_F(RemoveShardFromZoneTest, RemoveZoneThatNoLongerExistsShouldNotError) {
    ShardType shard;
    shard.setHandle(ShardHandle{ShardId("a"), boost::none});
    shard.setHost("a:1234");

    setupShards({shard});

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->removeShardFromZone(operationContext(), shard.getName(), "z"));
    auto shardDocStatus = getShardDoc(operationContext(), shard.getName());
    ASSERT_OK(shardDocStatus.getStatus());

    auto shardDoc = shardDocStatus.getValue();
    auto tags = shardDoc.getTags();
    ASSERT_TRUE(tags.empty());
}

TEST_F(RemoveShardFromZoneTest, RemovingZoneThatIsOnlyReferencedByAnotherShardShouldSucceed) {
    ShardType shardA;
    shardA.setHandle(ShardHandle{ShardId("a"), boost::none});
    shardA.setHost("a:1234");
    shardA.setTags({"z"});

    ShardType shardB;
    shardB.setHandle(ShardHandle{ShardId("b"), boost::none});
    shardB.setHost("b:1234");

    setupShards({shardA, shardB});

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->removeShardFromZone(operationContext(), shardB.getName(), "z"));

    // Shard A should still be in zone 'z'.
    auto shardADocStatus = getShardDoc(operationContext(), shardA.getName());
    ASSERT_OK(shardADocStatus.getStatus());

    auto shardADoc = shardADocStatus.getValue();
    auto shardATags = shardADoc.getTags();
    ASSERT_EQ(1u, shardATags.size());
    ASSERT_EQ("z", shardATags.front());

    // Shard B should not be in zone 'z'.
    auto shardBDocStatus = getShardDoc(operationContext(), shardB.getName());
    ASSERT_OK(shardBDocStatus.getStatus());

    auto shardBDoc = shardBDocStatus.getValue();
    auto shardBTags = shardBDoc.getTags();
    ASSERT_TRUE(shardBTags.empty());
}

TEST_F(RemoveShardFromZoneTest, RemoveLastZoneFromShardShouldSucceedWhenNoChunksReferToIt) {
    ShardType shardA;
    shardA.setHandle(ShardHandle{ShardId("a"), boost::none});
    shardA.setHost("a:1234");
    shardA.setTags({"z"});

    ShardType shardB;
    shardB.setHandle(ShardHandle{ShardId("b"), boost::none});
    shardB.setHost("b:1234");

    setupShards({shardA, shardB});

    // Insert a chunk range document referring to a different zone
    TagsType tagDoc;
    tagDoc.setNS(NamespaceString::createNamespaceString_forTest("test.foo"));
    tagDoc.setRange({BSON("x" << 0), BSON("x" << 10)});
    tagDoc.setTag("y");
    insertToConfigCollection(operationContext(), TagsType::ConfigNS, tagDoc.toBSON())
        .transitional_ignore();

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->removeShardFromZone(operationContext(), shardA.getName(), "z"));

    // Shard A should not be in zone 'z'.
    auto shardADocStatus = getShardDoc(operationContext(), shardA.getName());
    ASSERT_OK(shardADocStatus.getStatus());

    auto shardADoc = shardADocStatus.getValue();
    auto shardATags = shardADoc.getTags();
    ASSERT_TRUE(shardATags.empty());

    // Shard B should not be in zone 'z'.
    auto shardBDocStatus = getShardDoc(operationContext(), shardB.getName());
    ASSERT_OK(shardBDocStatus.getStatus());

    auto shardBDoc = shardBDocStatus.getValue();
    auto shardBTags = shardBDoc.getTags();
    ASSERT_TRUE(shardBTags.empty());
}

TEST_F(RemoveShardFromZoneTest, RemoveLastZoneFromShardShouldFailWhenAChunkRefersToIt) {
    ShardType shardA;
    shardA.setHandle(ShardHandle{ShardId("a"), boost::none});
    shardA.setHost("a:1234");
    shardA.setTags({"y", "z"});

    ShardType shardB;
    shardB.setHandle(ShardHandle{ShardId("b"), boost::none});
    shardB.setHost("b:1234");

    setupShards({shardA, shardB});

    TagsType tagDoc;
    tagDoc.setNS(NamespaceString::createNamespaceString_forTest("test.foo"));
    tagDoc.setRange({BSON("x" << 0), BSON("x" << 10)});
    tagDoc.setTag("z");
    insertToConfigCollection(operationContext(), TagsType::ConfigNS, tagDoc.toBSON())
        .transitional_ignore();

    auto status = ShardingCatalogManager::get(operationContext())
                      ->removeShardFromZone(operationContext(), shardA.getName(), "z");
    ASSERT_EQ(ErrorCodes::ZoneStillInUse, status);

    // Shard A should still be in zone 'z'.
    auto shardADocStatus = getShardDoc(operationContext(), shardA.getName());
    ASSERT_OK(shardADocStatus.getStatus());

    auto shardADoc = shardADocStatus.getValue();
    auto shardATags = shardADoc.getTags();
    ASSERT_EQ(2u, shardATags.size());
    ASSERT_EQ("y", shardATags.front());
    ASSERT_EQ("z", shardATags.back());

    // Shard B should not be in zone 'z'.
    auto shardBDocStatus = getShardDoc(operationContext(), shardB.getName());
    ASSERT_OK(shardBDocStatus.getStatus());

    auto shardBDoc = shardBDocStatus.getValue();
    auto shardBTags = shardBDoc.getTags();
    ASSERT_TRUE(shardBTags.empty());
}

TEST_F(RemoveShardFromZoneTest, RemoveZoneShouldFailIfShardDoesntExist) {
    ShardType shardA;
    shardA.setHandle(ShardHandle{ShardId("a"), boost::none});
    shardA.setHost("a:1234");
    shardA.setTags({"z"});

    setupShards({shardA});

    auto status = ShardingCatalogManager::get(operationContext())
                      ->removeShardFromZone(operationContext(), "b", "z");
    ASSERT_EQ(ErrorCodes::ShardNotFound, status);

    // Shard A should still be in zone 'z'.
    auto shardADocStatus = getShardDoc(operationContext(), shardA.getName());
    ASSERT_OK(shardADocStatus.getStatus());

    auto shardADoc = shardADocStatus.getValue();
    auto shardATags = shardADoc.getTags();
    ASSERT_EQ(1u, shardATags.size());
    ASSERT_EQ("z", shardATags.front());
}

TEST_F(RemoveShardFromZoneTest, RemoveZoneFromShardShouldOnlyRemoveZoneOnSpecifiedShard) {
    ShardType shardA;
    shardA.setHandle(ShardHandle{ShardId("a"), boost::none});
    shardA.setHost("a:1234");
    shardA.setTags({"z"});

    ShardType shardB;
    shardB.setHandle(ShardHandle{ShardId("b"), boost::none});
    shardB.setHost("b:1234");
    shardB.setTags({"y", "z"});

    setupShards({shardA, shardB});

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->removeShardFromZone(operationContext(), shardB.getName(), "z"));

    // Shard A should still be in zone 'z'.
    auto shardADocStatus = getShardDoc(operationContext(), shardA.getName());
    ASSERT_OK(shardADocStatus.getStatus());

    auto shardADoc = shardADocStatus.getValue();
    auto shardATags = shardADoc.getTags();
    ASSERT_EQ(1u, shardATags.size());
    ASSERT_EQ("z", shardATags.front());

    // Shard B should not be in zone 'z'.
    auto shardBDocStatus = getShardDoc(operationContext(), shardB.getName());
    ASSERT_OK(shardBDocStatus.getStatus());

    auto shardBDoc = shardBDocStatus.getValue();
    auto shardBTags = shardBDoc.getTags();
    ASSERT_EQ(1u, shardBTags.size());
    ASSERT_EQ("y", shardBTags.front());
}

/*
// TODO: This test fails while an OpObserver is present, since the insert of the invalid shard
// doc fails.
TEST_F(RemoveShardFromZoneTest, RemoveZoneFromShardShouldErrorIfShardDocIsMalformed) {
    // Note: invalid because tags is in string instead of array.
    BSONObj invalidShardDoc(BSON("_id"
                                 << "a"
                                 << "host"
                                 << "a:1"
                                 << "tags"
                                 << "z"));

    insertToConfigCollection(
        operationContext(), NamespaceString::kConfigsvrShardsNamespace, invalidShardDoc);


    auto status =
ShardingCatalogManager::get(operationContext())->removeShardFromZone(operationContext(), "a", "z");
    ASSERT_EQ(ErrorCodes::TypeMismatch, status);
}
*/
}  // unnamed namespace
}  // namespace mongo
