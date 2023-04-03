/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/balancer/auto_merger_policy.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/idl/server_parameter_test_util.h"

namespace mongo {

class AutoMergerPolicyTest : public ConfigServerTestFixture {
protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();
        setupShards(_shards);

        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        // ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->onShardingInitialization(operationContext(), true);
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->onStepDown();
        ConfigServerTestFixture::tearDown();
    }

    static Timestamp getCurrentTime(OperationContext* opCtx) {
        const auto currTime = VectorClock::get(opCtx)->getTime();
        auto currTimeSeconds = currTime.clusterTime().asTimestamp().getSecs();
        return Timestamp(currTimeSeconds, 0);
    }

    /* Generates 3 chunks per shard for the given collection */
    std::vector<ChunkType> generateChunks(const UUID& collUuid,
                                          bool allAreSupportedForHistoryOnShard0,
                                          bool allAreSupportedForHistoryOnShard1) {
        ASSERT_EQ(2, _shards.size());

        std::vector<ChunkType> chunks;
        ChunkVersion collVersion{{_epoch, _ts}, {1, 1}};

        // Generate  3 chunks for shard0
        {
            ChunkType chunkRef;
            chunkRef.setShard(_shard0);
            chunkRef.setCollectionUUID(collUuid);

            auto onCurrentShardSince = getCurrentTime(operationContext());
            if (!allAreSupportedForHistoryOnShard0) {
                onCurrentShardSince =
                    Timestamp(ShardingCatalogManager::getOldestTimestampSupportedForSnapshotHistory(
                                  operationContext())
                                      .getSecs() -
                                  1U,
                              0);
            }
            chunkRef.setOnCurrentShardSince(onCurrentShardSince);
            chunkRef.setHistory({ChunkHistory(*chunkRef.getOnCurrentShardSince(), _shard0)});

            auto min = _keyPattern.globalMin();
            for (auto i = 0; i < 3; ++i) {
                auto chunk = chunkRef;
                chunk.setVersion(collVersion);
                collVersion.incMinor();

                auto max = BSON("x" << i);
                chunk.setMin(min);
                chunk.setMax(max);
                min = max;

                chunks.push_back(chunk);
            }
        }

        // Generate 3 chunks for shard1
        {
            ChunkType chunkRef;
            chunkRef.setShard(_shard1);
            chunkRef.setCollectionUUID(collUuid);

            auto onCurrentShardSince = getCurrentTime(operationContext());
            if (!allAreSupportedForHistoryOnShard1) {
                onCurrentShardSince =
                    Timestamp(ShardingCatalogManager::getOldestTimestampSupportedForSnapshotHistory(
                                  operationContext())
                                      .getSecs() -
                                  1U,
                              0);
            }
            chunkRef.setOnCurrentShardSince(onCurrentShardSince);
            chunkRef.setHistory({ChunkHistory(*chunkRef.getOnCurrentShardSince(), _shard1)});

            auto min = _keyPattern.globalMin();
            for (auto i = 0; i < 3; ++i) {
                auto chunk = chunkRef;
                chunk.setVersion(collVersion);
                collVersion.incMinor();

                auto max = (i == 2 ? _keyPattern.globalMax() : BSON("x" << i));
                chunk.setMin(min);
                chunk.setMax(max);
                min = max;

                chunks.push_back(chunk);
            }
        }

        return chunks;
    }

    /* Generates 3 mergeable chunks per shard for the given collection */
    std::vector<ChunkType> generateMergeableChunks(const UUID& collUuid) {
        return generateChunks(collUuid,
                              false /*allChunksAreSupportedForHistoryOnShard0*/,
                              false /*allChunksAreSupportedForHistoryOnShard1*/);
    }

    void setupCollectionWithCustomBalancingFields(const NamespaceString& nss,
                                                  const std::vector<ChunkType>& chunks,
                                                  bool defragmentCollection = false,
                                                  bool enableAutoMerge = true) {
        ConfigServerTestFixture::setupCollection(nss, _keyPattern, chunks);

        // Update the collection's 'defragmentCollection' and 'enableAutoMerge' fields.
        BSONObjBuilder setBuilder;
        if (defragmentCollection) {
            setBuilder.appendBool(CollectionType::kDefragmentCollectionFieldName, true);
        }
        if (!enableAutoMerge) {
            setBuilder.appendBool(CollectionType::kEnableAutoMergeFieldName, false);
        }
        ASSERT_OK(updateToConfigCollection(operationContext(),
                                           CollectionType::ConfigNS,
                                           BSON(CollectionType::kNssFieldName << nss.toString()),
                                           BSON("$set" << setBuilder.obj()),
                                           false /*upsert*/));
    }

    void assertAutomergerConsidersCollectionsWithMergeableChunks(
        const std::map<ShardId, std::vector<NamespaceString>>& expectedNamespacesPerShard) {

        const auto fetchedNamespacesPerShard =
            _automerger._getNamespacesWithMergeableChunksPerShard(operationContext());

        ASSERT_EQ(expectedNamespacesPerShard.size(), _shards.size());
        ASSERT_EQ(expectedNamespacesPerShard.size(), fetchedNamespacesPerShard.size());

        for (const auto& [shardId, expectedNamespaces] : expectedNamespacesPerShard) {
            ASSERT_EQ(fetchedNamespacesPerShard.count(shardId), 1);

            const auto& nssWithMergeableChunks = fetchedNamespacesPerShard.at(shardId);

            for (const auto& expectedNss : expectedNamespaces) {
                bool expectedNssIsFetched = std::find(nssWithMergeableChunks.begin(),
                                                      nssWithMergeableChunks.end(),
                                                      expectedNss) != nssWithMergeableChunks.end();
                ASSERT_EQ(true, expectedNssIsFetched)
                    << "expected collection " << expectedNss << " on shard " << shardId
                    << " wasn't fetched";
            }

            ASSERT_EQ(expectedNamespaces.size(), nssWithMergeableChunks.size())
                << "shardId: " << shardId;
        }
    }

    int getMaxNumberOfConcurrentMergeActions() {
        return AutoMergerPolicy::MAX_NUMBER_OF_CONCURRENT_MERGE_ACTIONS;
    }

protected:
    AutoMergerPolicy _automerger{[]() {
    }};

    inline const static auto _shards =
        std::vector<ShardType>{ShardType{"shard0", "host0:123"}, ShardType{"shard1", "host1:123"}};

    const std::string _shard0 = _shards[0].getName();
    const std::string _shard1 = _shards[1].getName();

    const OID _epoch = OID::gen();
    const Timestamp _ts = Timestamp(43);

    const KeyPattern _keyPattern{BSON("x" << 1)};
};

TEST_F(AutoMergerPolicyTest, FetchCollectionsWithMergeableChunks) {
    ASSERT_EQ(2, _shards.size());

    std::map<ShardId, std::vector<NamespaceString>> expectedNamespacesPerShard;

    {
        // Setup coll1 (all its chunks are mergeable)
        //  - enableAutoMerge: true
        //  - defragmentatCollection: false
        //  - routing table:
        //       - shard0: 3 chunks (supportedForHistory: false)
        //       - shard1: 3 chunks (supportedForHistory: false)
        const auto collUuid = UUID::gen();
        const auto nss = NamespaceString::createNamespaceString_forTest("test.coll1");
        const auto chunks = generateChunks(collUuid,
                                           false /*allChunksAreSupportedForHistoryOnShard0*/,
                                           false /*allChunksAreSupportedForHistoryOnShard1*/
        );

        setupCollectionWithCustomBalancingFields(
            nss, chunks, false /*defragmentCollection*/, true /*enableAutoMerge*/);

        expectedNamespacesPerShard[_shard0].push_back(nss);
        expectedNamespacesPerShard[_shard1].push_back(nss);
    }
    {
        // Setup coll2 (defragmentation is enabled)
        //  - enableAutoMerge: true
        //  - defragmentatCollection: true
        //  - routing table:
        //       - shard0: 3 chunks (supportedForHistory: false)
        //       - shard1: 3 chunks (supportedForHistory: false)
        const auto collUuid = UUID::gen();
        const auto nss = NamespaceString::createNamespaceString_forTest("test.coll2");
        const auto chunks = generateChunks(collUuid,
                                           false /*allChunksAreSupportedForHistoryOnShard0*/,
                                           false /*allChunksAreSupportedForHistoryOnShard1*/
        );

        setupCollectionWithCustomBalancingFields(
            nss, chunks, true /*defragmentCollection*/, true /*enableAutoMerge*/);

        // No expected namespaces for shard0 and shard1 because defragmentation is enabled.
    }
    {
        // Setup coll3 (automerger is disabled)
        //  - enableAutoMerge: false
        //  - defragmentatCollection: false
        //  - routing table:
        //       - shard0: 3 chunks (supportedForHistory: false)
        //       - shard1: 3 chunks (supportedForHistory: false)
        const auto collUuid = UUID::gen();
        const auto nss = NamespaceString::createNamespaceString_forTest("test.coll3");
        const auto chunks = generateChunks(collUuid,
                                           false /*allChunksAreSupportedForHistoryOnShard0*/,
                                           false /*allChunksAreSupportedForHistoryOnShard1*/);

        setupCollectionWithCustomBalancingFields(
            nss, chunks, false /*defragmentCollection*/, false /*enableAutoMerge*/);

        // No expected namespaces for shard0 and shard1 because automerger is disabled.
    }
    {
        // Setup coll4 (shard0 chunks are supported for history)
        //  - defragmentatCollection: false
        //  - enableAutoMerge: true
        //  - routing table:
        //       - shard0: 3 chunks (supportedForHistory: true)
        //       - shard1: 3 chunks (supportedForHistory: false)
        const auto collUuid = UUID::gen();
        const auto nss = NamespaceString::createNamespaceString_forTest("test.coll5");
        const auto chunks = generateChunks(collUuid,
                                           true /*allChunksAreSupportedForHistoryOnShard0*/,
                                           false /*allChunksAreSupportedForHistoryOnShard1*/);

        setupCollectionWithCustomBalancingFields(
            nss, chunks, false /*defragmentCollection*/, true /*enableAutoMerge*/);

        expectedNamespacesPerShard[_shard1].push_back(nss);
        // No expected namespaces for shard0 because all chunks are supported for history.
    }

    // Run the test
    assertAutomergerConsidersCollectionsWithMergeableChunks(expectedNamespacesPerShard);
};

TEST_F(AutoMergerPolicyTest, MaxConcurrentMergeActionsIsHonored) {

    const auto maxConcurrentActions = getMaxNumberOfConcurrentMergeActions();
    const auto numCollections = maxConcurrentActions + 10;

    for (auto i = 0; i < numCollections; ++i) {

        const auto collUuid = UUID::gen();
        const auto nss =
            NamespaceString::createNamespaceString_forTest("test.coll" + std::to_string(i));
        const auto chunks = generateMergeableChunks(collUuid);

        ConfigServerTestFixture::setupCollection(nss, _keyPattern, chunks);
    }

    _automerger.enable();

    std::deque<BalancerStreamAction> actions;
    auto i = 0;
    for (; i < maxConcurrentActions; ++i) {
        const auto action = _automerger.getNextStreamingAction(operationContext());
        ASSERT(action.has_value());
        actions.emplace_back(action.value());
    }

    for (; i < numCollections; ++i) {
        for (auto j = 0; j < 3; j++) {
            const auto action = _automerger.getNextStreamingAction(operationContext());
            ASSERT(!action.has_value());
        }

        _automerger.applyActionResult(operationContext(),
                                      actions.front(),
                                      BalancerStreamActionResponse(StatusWith<NumMergedChunks>(0)));
        actions.pop_front();

        const auto action = _automerger.getNextStreamingAction(operationContext());
        ASSERT(action.has_value());
        actions.emplace_back(action.value());
    }
}

TEST_F(AutoMergerPolicyTest, MergeActionRescheduledWhenMergeHappened) {
    const auto collUuid = UUID::gen();
    const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const auto chunks = generateMergeableChunks(collUuid);

    ConfigServerTestFixture::setupCollection(nss, _keyPattern, chunks);

    _automerger.enable();

    // ------ FIRST ROUND
    // Auto-merger returns action for <shard0, test.coll> because there are mergeable chunks
    auto optAction = _automerger.getNextStreamingAction(operationContext());
    ASSERT(optAction.has_value());
    auto action = stdx::get<MergeAllChunksOnShardInfo>(*optAction);
    ASSERT_EQ(action.nss, nss);
    ASSERT_EQ(action.shardId, _shard0);
    _automerger.applyActionResult(
        operationContext(), action, BalancerStreamActionResponse(StatusWith<NumMergedChunks>(10)));

    // Auto-merger returns action for <shard1, test.coll> because there are mergeable chunks
    optAction = _automerger.getNextStreamingAction(operationContext());
    ASSERT(optAction.has_value());
    action = stdx::get<MergeAllChunksOnShardInfo>(*optAction);
    ASSERT_EQ(action.nss, nss);
    ASSERT_EQ(action.shardId, _shard1);
    _automerger.applyActionResult(
        operationContext(), action, BalancerStreamActionResponse(StatusWith<NumMergedChunks>(10)));

    // ------ SECOND ROUND
    // Auto-merger returns action for <shard0, test.coll> because some chunks were previously merged
    optAction = _automerger.getNextStreamingAction(operationContext());
    ASSERT(optAction.has_value());
    action = stdx::get<MergeAllChunksOnShardInfo>(*optAction);
    ASSERT_EQ(action.nss, nss);
    ASSERT_EQ(action.shardId, _shard0);
    _automerger.applyActionResult(
        operationContext(), action, BalancerStreamActionResponse(StatusWith<NumMergedChunks>(0)));

    // Auto-merger returns action for <shard1, test.coll> because some chunks were previously merged
    optAction = _automerger.getNextStreamingAction(operationContext());
    ASSERT(optAction.has_value());
    action = stdx::get<MergeAllChunksOnShardInfo>(*optAction);
    ASSERT_EQ(action.nss, nss);
    ASSERT_EQ(action.shardId, _shard1);
    _automerger.applyActionResult(
        operationContext(), action, BalancerStreamActionResponse(StatusWith<NumMergedChunks>(0)));

    // ------ THIRD ROUND
    // Auto-merger not returns any action because no chunks were previously merged
    optAction = _automerger.getNextStreamingAction(operationContext());
    ASSERT(!optAction.has_value());
}

TEST_F(AutoMergerPolicyTest, MergeActionRescheduledUponConflictingOperationInProgress) {
    const auto collUuid = UUID::gen();
    const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const auto chunks = generateMergeableChunks(collUuid);

    ConfigServerTestFixture::setupCollection(nss, _keyPattern, chunks);

    _automerger.enable();

    // Auto-merger returns action for <shard0, test.coll> because there are mergeable chunks
    auto optAction = _automerger.getNextStreamingAction(operationContext());
    ASSERT(optAction.has_value());
    auto action = stdx::get<MergeAllChunksOnShardInfo>(*optAction);
    ASSERT_EQ(action.nss, nss);
    ASSERT_EQ(action.shardId, _shard0);
    _automerger.applyActionResult(operationContext(),
                                  action,
                                  BalancerStreamActionResponse(StatusWith<int>{
                                      ErrorCodes::ConflictingOperationInProgress, "err"}));

    // Discard merge scheduled for shard1
    _automerger.getNextStreamingAction(operationContext());

    // Auto-merger returns action for <shard0, test.coll> upon ConflictingOperationInProgress
    optAction = _automerger.getNextStreamingAction(operationContext());
    action = stdx::get<MergeAllChunksOnShardInfo>(*optAction);
    ASSERT(optAction.has_value());
    ASSERT_EQ(action.nss, nss);
    ASSERT_EQ(action.shardId, _shard0);
}

}  // namespace mongo
