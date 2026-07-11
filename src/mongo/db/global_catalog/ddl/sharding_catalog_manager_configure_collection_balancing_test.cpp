// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/chunk_constraints.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <cassert>
#include <cstdint>
#include <string>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

using unittest::assertGet;

class ConfigureCollectionBalancingTest : public ConfigServerTestFixture {
protected:
    std::string _shardName = "shard0000";
    std::string _shardHostName = "host01";
    void setUp() override {
        ConfigServerTestFixture::setUp();

        // Create config.transactions collection
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        LogicalSessionCache::set(getServiceContext(), std ::make_unique<LogicalSessionCacheNoop>());

        const auto connStr = assertGet(ConnectionString::parse(_shardHostName));
        std::unique_ptr<RemoteCommandTargeterMock> targeter(
            std::make_unique<RemoteCommandTargeterMock>());
        targeter->setConnectionStringReturnValue(connStr);
        targeter->setFindHostReturnValue(connStr.getServers()[0]);
        targeterFactory()->addTargeterToReturn(connStr, std::move(targeter));

        auto epoch = OID::gen();
        const auto uuid = UUID::gen();
        const auto timestamp = Timestamp(1);
        ChunkType chunkType1(uuid,
                             ChunkRange(BSON("_id" << MINKEY), BSON("_id" << MAXKEY)),
                             ChunkVersion({epoch, timestamp}, {1, 1}),
                             ShardRef{_shardName});

        setupShards({ShardType(_shardName, _shardHostName)});
        setupDatabase(_dbName, _shardName);
        setupCollection(_nss, _keyPattern, {chunkType1});
    }

    const DatabaseName _dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const NamespaceString _nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const UUID _collUuid = UUID::gen();
    const KeyPattern _keyPattern{BSON("x" << 1)};

    void configureCollectionBalancing(NamespaceString nss,
                                      boost::optional<int32_t> chunkSizeMB,
                                      boost::optional<bool> defragmentCollection,
                                      boost::optional<bool> enableAutoMerger,
                                      boost::optional<bool> enableBalancing) {
        auto opCtx = operationContext();
        ShardingCatalogManager::get(opCtx)->configureCollectionBalancing(
            opCtx, nss, chunkSizeMB, defragmentCollection, enableAutoMerger, enableBalancing);
    }

    BSONObj getCollectionDocument(NamespaceString nss) {
        return findOneOnConfigCollection(operationContext(),
                                         NamespaceString::kConfigsvrCollectionsNamespace,
                                         BSON(CollectionType::kNssFieldName << nss.ns_forTest()))
            .getValue();
    }
};

TEST_F(ConfigureCollectionBalancingTest, SettingChunkSizeMBOutOfBounds) {
    auto opCtx = operationContext();

    // Errors if chunkSize < 1MB but not 0
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(opCtx)->configureCollectionBalancing(
                           opCtx, _nss, -1, boost::none, boost::none, boost::none),
                       DBException,
                       ErrorCodes::InvalidOptions);

    // Errors if chunkSize > 1GB
    auto chunkSize = 2 * 1024;  // 2 GB
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(opCtx)->configureCollectionBalancing(
                           opCtx, _nss, chunkSize, boost::none, boost::none, boost::none),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(ConfigureCollectionBalancingTest, SettingChunkSizeMBInBounds) {
    configureCollectionBalancing(_nss, 1, boost::none, boost::none, boost::none);
    auto configDoc = getCollectionDocument(_nss);
    ASSERT_TRUE(configDoc[CollectionType::kMaxChunkSizeBytesFieldName].Long() == 1024 * 1024);
}

TEST_F(ConfigureCollectionBalancingTest, SettingChunkSizeMBToZero) {
    // Set maxChunkSizeBytes to 1MB
    configureCollectionBalancing(_nss, 1, boost::none, boost::none, boost::none);
    auto configDoc = getCollectionDocument(_nss);
    ASSERT_TRUE(configDoc[CollectionType::kMaxChunkSizeBytesFieldName].Long() == 1024 * 1024);

    // Unset maxChunkSizeBytes
    configureCollectionBalancing(_nss, 0, boost::none, boost::none, boost::none);
    configDoc = getCollectionDocument(_nss);
    ASSERT_FALSE(configDoc.hasField(CollectionType::kMaxChunkSizeBytesFieldName));
}

TEST_F(ConfigureCollectionBalancingTest, SettingChunkSizeMBToZeroLogicalSessions) {
    // setup kLogicalSessionsNamespace
    auto epoch = OID::gen();
    const auto uuid = UUID::gen();
    const auto timestamp = Timestamp(1);
    ChunkType chunkType(uuid,
                        ChunkRange(BSON("_id" << MINKEY), BSON("_id" << MAXKEY)),
                        ChunkVersion({epoch, timestamp}, {1, 1}),
                        ShardRef{_shardName});
    setupCollection(NamespaceString::kLogicalSessionsNamespace, _keyPattern, {chunkType});

    // Set maxChunkSizeBytes to 1MB
    configureCollectionBalancing(
        NamespaceString::kLogicalSessionsNamespace, 1, boost::none, boost::none, boost::none);
    auto configDoc = getCollectionDocument(NamespaceString::kLogicalSessionsNamespace);
    ASSERT_TRUE(configDoc[CollectionType::kMaxChunkSizeBytesFieldName].Long() == 1024 * 1024);

    // Setting maxChunkSizeBytes to 0 sets to default maxChunkSizeBytes for the logical sessions
    // collection
    configureCollectionBalancing(
        NamespaceString::kLogicalSessionsNamespace, 0, boost::none, boost::none, boost::none);
    configDoc = getCollectionDocument(NamespaceString::kLogicalSessionsNamespace);
    ASSERT_TRUE(configDoc[CollectionType::kMaxChunkSizeBytesFieldName].Long() ==
                logical_sessions::kMaxChunkSizeBytes);
}

TEST_F(ConfigureCollectionBalancingTest, SettingDefragmentCollection) {
    // defragmentCollection starts as unset
    auto configDoc = getCollectionDocument(_nss);
    ASSERT_FALSE(configDoc.hasField(CollectionType::kDefragmentCollectionFieldName));

    // set defragmentCollection to true
    configureCollectionBalancing(_nss, boost::none, true, boost::none, boost::none);
    configDoc = getCollectionDocument(_nss);

    ASSERT_TRUE(configDoc[CollectionType::kDefragmentCollectionFieldName].Bool());

    // turn off defragmentCollection
    const auto opCtx = operationContext();
    ShardingCatalogManager::get(opCtx)->configureCollectionBalancing(
        opCtx, _nss, boost::none, false, boost::none, boost::none);

    // defragmentationPhase is set to finished
    configDoc = getCollectionDocument(_nss);
    auto storedDefragmentationPhase = idl::deserialize<DefragmentationPhaseEnum>(
        configDoc.getStringField(CollectionType::kDefragmentationPhaseFieldName),
        IDLParserContext("ConfigureCollectionBalancingTest"));

    ASSERT_TRUE(storedDefragmentationPhase == DefragmentationPhaseEnum::kFinished);
}

TEST_F(ConfigureCollectionBalancingTest, SettingEnableAutoMerger) {
    // enableAutoMerge starts as unset
    auto configDoc = getCollectionDocument(_nss);
    ASSERT_FALSE(configDoc.hasField(CollectionType::kEnableAutoMergeFieldName));

    // set enableAutoMerge to true
    configureCollectionBalancing(_nss, boost::none, boost::none, true, boost::none);
    configDoc = getCollectionDocument(_nss);

    ASSERT_TRUE(configDoc[CollectionType::kEnableAutoMergeFieldName].Bool());
}

TEST_F(ConfigureCollectionBalancingTest, SettingEnableBalancing) {
    // kNoBalanceFieldName starts as false.
    auto configDoc = getCollectionDocument(_nss);
    ASSERT_FALSE(configDoc[CollectionType::kNoBalanceFieldName].Bool());

    // Set enableBalancing to false which results in kNoBalanceFieldName to be set to true.
    configureCollectionBalancing(_nss, boost::none, boost::none, boost::none, false);
    configDoc = getCollectionDocument(_nss);

    ASSERT_TRUE(configDoc[CollectionType::kNoBalanceFieldName].Bool());
}

TEST_F(ConfigureCollectionBalancingTest, CollectionNotSharded) {
    const auto unshardedNSS = NamespaceString::createNamespaceString_forTest("test.unsharded");
    const auto opCtx = operationContext();
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(opCtx)->configureCollectionBalancing(
                           opCtx, unshardedNSS, 1, boost::none, boost::none, boost::none),
                       DBException,
                       ErrorCodes::NamespaceNotSharded);
}

}  // namespace
}  // namespace mongo
