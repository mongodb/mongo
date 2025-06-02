/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/commands/query_cmd/aggregation_execution_state.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

/**
 * Test the basic functionality of each subclass of AggCatalogState.
 */
class AggregationExecutionStateTest : public ShardServerTestFixtureWithCatalogCacheMock {
protected:
    void setUp() override;

    DatabaseVersion getDbVersion() {
        return _dbVersion;
    }

    // Install sharded collection metadata for an unsharded collection and fills the cache.
    void installUnshardedCollectionMetadata(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            bool requiresExtendedRangeSupport = false) {
        AutoGetCollection coll(
            opCtx,
            NamespaceStringOrUUID(nss),
            MODE_IX,
            AutoGetCollection::Options{}.viewMode(auto_get_collection::ViewMode::kViewsPermitted));

        if (requiresExtendedRangeSupport) {
            coll->setRequiresTimeseriesExtendedRangeSupport(opCtx);
        }

        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss)
            ->setFilteringMetadata(opCtx, CollectionMetadata::UNTRACKED());
        auto cm = ChunkManager(RoutingTableHistoryValueHandle{OptionalRoutingTableHistory{}},
                               _dbVersion.getTimestamp());
        getCatalogCacheMock()->setCollectionReturnValue(
            nss,
            CollectionRoutingInfo(
                std::move(cm),
                DatabaseTypeValueHandle(DatabaseType{nss.dbName(), kMyShardName, _dbVersion})));
    }

    // Install sharded collection metadata for 1 chunk sharded collection and fills the cache.
    void installShardedCollectionMetadata(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          ShardId shardName,
                                          bool requiresExtendedRangeSupport = false) {
        // Made up a shard version
        const ShardVersion shardVersion = ShardVersionFactory::make(ChunkVersion(
            CollectionGeneration{OID::gen(), Timestamp(5, 0)}, CollectionPlacement(10, 1)));


        const auto uuid = [&] {
            AutoGetCollection autoColl(opCtx, NamespaceStringOrUUID(nss), MODE_IX);
            return autoColl.getCollection()->uuid();
        }();

        const auto chunk = ChunkType(uuid,
                                     ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                                     shardVersion.placementVersion(),
                                     shardName);


        const std::string shardKey("skey");
        const ShardKeyPattern shardKeyPattern{BSON(shardKey << 1)};
        const auto epoch = chunk.getVersion().epoch();
        const auto timestamp = chunk.getVersion().getTimestamp();

        auto rt = RoutingTableHistory::makeNew(nss,
                                               uuid,
                                               shardKeyPattern.getKeyPattern(),
                                               false, /* unsplittable */
                                               nullptr,
                                               false,
                                               epoch,
                                               timestamp,
                                               boost::none /* timeseriesFields */,
                                               boost::none /* resharding Fields */,
                                               true /* allowMigrations */,
                                               {chunk});

        const auto version = rt.getVersion();
        const auto rtHandle = RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));

        auto cm = ChunkManager(rtHandle, boost::none);
        const auto collectionMetadata = CollectionMetadata(cm, shardName);

        AutoGetCollection coll(opCtx, NamespaceStringOrUUID(nss), MODE_IX);

        if (requiresExtendedRangeSupport) {
            coll->setRequiresTimeseriesExtendedRangeSupport(opCtx);
        }

        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss)
            ->setFilteringMetadata(opCtx, collectionMetadata);

        getCatalogCacheMock()->setCollectionReturnValue(
            nss,
            CollectionRoutingInfo(
                std::move(cm),
                DatabaseTypeValueHandle(DatabaseType{nss.dbName(), shardName, _dbVersion})));
    }

    DatabaseType createTestDatabase(const UUID& uuid, const Timestamp& timestamp) {
        return DatabaseType(_dbName, kMyShardName, DatabaseVersion(uuid, timestamp));
    }

    NamespaceString createTestCollection(StringData coll, bool sharded) {
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(_dbName, coll);
        auto opCtx = operationContext();
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            opCtx);
        uassertStatusOK(createCollection(opCtx, _dbName, BSON("create" << coll)));

        if (sharded) {
            installShardedCollectionMetadata(opCtx, nss, kMyShardName);
        } else {
            installUnshardedCollectionMetadata(opCtx, nss);
        }

        return nss;
    }

    NamespaceString createTimeseriesCollection(StringData coll,
                                               bool sharded,
                                               bool requiresExtendedRangeSupport) {
        auto nss = NamespaceString::createNamespaceString_forTest(_dbName, coll);
        auto bucketsNss = nss.makeTimeseriesBucketsNamespace();
        auto opCtx = operationContext();
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            opCtx);

        auto timeseriesOptions = BSON("timeField" << "timestamp");
        uassertStatusOK(createCollection(
            opCtx, _dbName, BSON("create" << coll << "timeseries" << timeseriesOptions)));

        if (sharded) {
            installShardedCollectionMetadata(
                opCtx, bucketsNss, kMyShardName, requiresExtendedRangeSupport);
        } else {
            installUnshardedCollectionMetadata(opCtx, bucketsNss, requiresExtendedRangeSupport);
        }

        return nss;
    }

    std::pair<NamespaceString, std::vector<BSONObj>> createTestView(StringData viewName,
                                                                    StringData collName) {
        NamespaceString viewNss = NamespaceString::createNamespaceString_forTest(_dbName, viewName);

        auto opCtx = operationContext();
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            opCtx);
        auto match = BSON("$match" << BSON("a" << 1));
        uassertStatusOK(createCollection(
            opCtx,
            _dbName,
            BSON("create" << viewName << "viewOn" << collName << "pipeline" << BSON_ARRAY(match))));
        installUnshardedCollectionMetadata(opCtx, viewNss);
        std::vector<BSONObj> expectedResolvedPipeline = {match};
        return std::make_pair(viewNss, expectedResolvedPipeline);
    }

    /**
     * Create an AggExState instance that one might see for a typical query.
     */
    std::unique_ptr<AggExState> createDefaultAggExState(StringData coll) {
        auto opCtx = operationContext();

        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", coll);

        _cmdObj = BSON("aggregate" << coll << "pipeline" << BSONObj{} << "cursor" << BSONObj{});
        std::vector<BSONObj> pipeline;
        _request = std::make_unique<AggregateCommandRequest>(nss, pipeline);
        _lpp = std::make_unique<LiteParsedPipeline>(*_request);

        return std::make_unique<AggExState>(opCtx,
                                            *_request,
                                            *_lpp,
                                            _cmdObj,
                                            _privileges,
                                            _externalSources,
                                            boost::none /* verbosity */);
    }

    /**
     * Create an AggExState instance that one might see for a typical query.
     */
    std::unique_ptr<AggExState> createDefaultAggExStateWithSecondaryCollections(
        StringData main, StringData secondary) {
        auto opCtx = operationContext();

        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", main);
        NamespaceString nss2 = NamespaceString::createNamespaceString_forTest("test", secondary);

        BSONObj lookup = BSON("$lookup" << BSON("from" << nss2.coll()));
        BSONArray pipeline = BSON_ARRAY(lookup);
        _cmdObj = BSON("aggregate" << main << "pipeline" << pipeline << "cursor" << BSONObj{});
        _request =
            std::make_unique<AggregateCommandRequest>(nss, std::vector<mongo::BSONObj>{lookup});
        _lpp = std::make_unique<LiteParsedPipeline>(*_request);

        return std::make_unique<AggExState>(opCtx,
                                            *_request,
                                            *_lpp,
                                            _cmdObj,
                                            _privileges,
                                            _externalSources,
                                            boost::none /* verbosity */);
    }

    /**
     * Create an AggExState instance that one might see for change stream query.
     */
    std::unique_ptr<AggExState> createOplogAggExState(StringData coll) {
        auto opCtx = operationContext();

        // We will wait indefinitely in this unit test for the read concern to be set unless we set
        // it explicitly here.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs{repl::ReadConcernLevel::kLocalReadConcern};

        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", coll);
        BSONObj changeStreamStage = BSON("$changeStream" << BSONObj{});

        _cmdObj = BSON("aggregate" << coll << "pipeline" << BSON_ARRAY(changeStreamStage)
                                   << "cursor" << BSONObj{});
        std::vector<BSONObj> pipeline{changeStreamStage};
        _request = std::make_unique<AggregateCommandRequest>(nss, pipeline);
        _lpp = std::make_unique<LiteParsedPipeline>(*_request);

        auto aggExState = std::make_unique<AggExState>(opCtx,
                                                       *_request,
                                                       *_lpp,
                                                       _cmdObj,
                                                       _privileges,
                                                       _externalSources,
                                                       boost::none /* verbosity */);

        return aggExState;
    }

    /**
     * Create an AggExState instance that one might see for a query that is not on any particular
     * collection.
     */
    std::unique_ptr<AggExState> createCollectionlessAggExState() {
        auto opCtx = operationContext();

        StringData coll = "$cmd.aggregate"_sd;
        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", coll);

        BSONObj documentsStage = BSON("$documents" << BSON_ARRAY(BSON("a" << 1)));
        _cmdObj = BSON("aggregate" << coll << "pipeline" << BSON_ARRAY(documentsStage) << "cursor"
                                   << BSONObj{});
        std::vector<BSONObj> pipeline{documentsStage};
        _request = std::make_unique<AggregateCommandRequest>(nss, pipeline);
        _lpp = std::make_unique<LiteParsedPipeline>(*_request);

        return std::make_unique<AggExState>(opCtx,
                                            *_request,
                                            *_lpp,
                                            _cmdObj,
                                            _privileges,
                                            _externalSources,
                                            boost::none /* verbosity */);
    }

private:
    std::unique_ptr<AggregateCommandRequest> _request;
    std::unique_ptr<LiteParsedPipeline> _lpp;
    PrivilegeVector _privileges;
    BSONObj _cmdObj;
    std::vector<std::pair<NamespaceString, std::vector<ExternalDataSourceInfo>>> _externalSources;
    DatabaseVersion _dbVersion = {UUID::gen(), Timestamp(1, 0)};
    const DatabaseName _dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
};

void AggregationExecutionStateTest::setUp() {
    ShardServerTestFixtureWithCatalogCacheMock::setUp();
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::RouterServer};

    auto dbType = createTestDatabase(UUID::gen(), Timestamp(1, 0));

    Grid::get(operationContext())->setShardingInitialized();
}

TEST_F(AggregationExecutionStateTest, CreateDefaultAggCatalogState) {
    StringData coll{"coll"};
    auto nss = createTestCollection(coll, false /*sharded*/);
    std::unique_ptr<AggExState> aggExState = createDefaultAggExState(coll);
    std::unique_ptr<AggCatalogState> aggCatalogState = aggExState->createAggCatalogState();

    ASSERT_DOES_NOT_THROW(aggCatalogState->validate());

    ASSERT_TRUE(aggCatalogState->lockAcquired());

    boost::optional<AutoStatsTracker> tracker;
    aggCatalogState->getStatsTrackerIfNeeded(tracker);
    ASSERT_FALSE(tracker.has_value());

    auto [collator, matchesDefault] = aggCatalogState->resolveCollator();
    ASSERT_TRUE(CollatorInterface::isSimpleCollator(collator.get()));
    ASSERT_EQ(matchesDefault, ExpressionContextCollationMatchesDefault::kYes);

    ASSERT_TRUE(aggCatalogState->getCollections().hasMainCollection());

    ASSERT_TRUE(aggCatalogState->getUUID().has_value());

    ASSERT_DOES_NOT_THROW(aggCatalogState->relinquishResources());
}

TEST_F(AggregationExecutionStateTest, CreateDefaultAggCatalogStateWithSecondaryCollection) {
    StringData main{"main"};
    StringData secondaryColl{"secondaryColl"};

    auto mainNss = createTestCollection(main, false /*sharded*/);
    auto secondaryNssColl = createTestCollection(secondaryColl, false /*sharded*/);

    std::unique_ptr<AggExState> aggExState =
        createDefaultAggExStateWithSecondaryCollections(main, secondaryColl);

    std::unique_ptr<AggCatalogState> aggCatalogState = aggExState->createAggCatalogState();

    ASSERT_DOES_NOT_THROW(aggCatalogState->validate());

    ASSERT_TRUE(aggCatalogState->lockAcquired());

    // Verify main collection
    ASSERT_TRUE(aggCatalogState->getCollections().hasMainCollection());

    // Verify secondary collections
    {
        auto secondaryColls = aggCatalogState->getCollections().getSecondaryCollections();
        ASSERT_EQ(1, secondaryColls.size());
        ASSERT_TRUE(secondaryColls.contains(secondaryNssColl));
    }

    // Verify MultipleCollectionAccessor
    ASSERT_FALSE(aggCatalogState->getCollections().isAnySecondaryNamespaceAViewOrNotFullyLocal());

    ASSERT_TRUE(aggCatalogState->getUUID().has_value());

    ASSERT_DOES_NOT_THROW(aggCatalogState->relinquishResources());
}

TEST_F(AggregationExecutionStateTest, CreateDefaultAggCatalogStateWithSecondaryShardedCollection) {
    StringData main{"main"};
    StringData secondaryColl{"secondaryColl"};

    auto mainNss = createTestCollection(main, false /*sharded*/);
    auto secondaryNssColl = createTestCollection(secondaryColl, true /*sharded*/);

    // Add at least 1 shard version to the opCtx to simulate a router request. This is necessary
    // to correctly set the isAnySecondaryNamespaceAViewOrNotFullyLocal.
    ScopedSetShardRole setShardRole(
        operationContext(), mainNss, ShardVersion::UNSHARDED(), getDbVersion());
    std::unique_ptr<AggExState> aggExState =
        createDefaultAggExStateWithSecondaryCollections(main, secondaryColl);

    std::unique_ptr<AggCatalogState> aggCatalogState = aggExState->createAggCatalogState();

    ASSERT_DOES_NOT_THROW(aggCatalogState->validate());

    ASSERT_TRUE(aggCatalogState->lockAcquired());

    // Verify main collection.
    ASSERT_TRUE(aggCatalogState->getCollections().hasMainCollection());

    // Verify secondary collections
    {
        auto secondaryColls = aggCatalogState->getCollections().getSecondaryCollections();
        ASSERT_EQ(1, secondaryColls.size());
        ASSERT_TRUE(secondaryColls.contains(secondaryNssColl));
    }

    // Verify MultipleCollectionAccessor
    ASSERT_TRUE(aggCatalogState->getCollections().isAnySecondaryNamespaceAViewOrNotFullyLocal());

    ASSERT_TRUE(aggCatalogState->getUUID().has_value());

    ASSERT_DOES_NOT_THROW(aggCatalogState->relinquishResources());
}

TEST_F(AggregationExecutionStateTest, CreateDefaultAggCatalogStateWithSecondaryView) {
    StringData main{"main"};
    StringData secondaryColl{"secondaryColl"};
    StringData secondaryView{"secondaryView"};

    auto mainNss = createTestCollection(main, false /*sharded*/);
    auto secondaryNssColl = createTestCollection(secondaryColl, false /*sharded*/);
    auto [secondaryNssView, _] = createTestView(secondaryView, secondaryColl);

    std::unique_ptr<AggExState> aggExState =
        createDefaultAggExStateWithSecondaryCollections(main, secondaryView);

    std::unique_ptr<AggCatalogState> aggCatalogState = aggExState->createAggCatalogState();

    ASSERT_DOES_NOT_THROW(aggCatalogState->validate());

    ASSERT_TRUE(aggCatalogState->lockAcquired());

    // Verify main collection
    ASSERT_TRUE(aggCatalogState->getCollections().hasMainCollection());
    ASSERT_FALSE(aggCatalogState->getMainCollectionOrView().isView());

    // Verify secondary collections
    {
        auto secondaryColls = aggCatalogState->getCollections().getSecondaryCollections();
        ASSERT_EQ(1, secondaryColls.size());
        ASSERT_TRUE(secondaryColls.contains(secondaryNssView));
    }

    // Verify MultipleCollectionAccessor
    ASSERT_TRUE(aggCatalogState->getCollections().isAnySecondaryNamespaceAViewOrNotFullyLocal());

    ASSERT_TRUE(aggCatalogState->getUUID().has_value());

    ASSERT_DOES_NOT_THROW(aggCatalogState->relinquishResources());
}

TEST_F(AggregationExecutionStateTest, CreateDefaultAggCatalogStateView) {
    StringData coll{"coll"};
    StringData view{"view"};
    auto viewOn = createTestCollection(coll, false /*sharded*/);
    auto [viewNss, expectedPipeline] = createTestView(view, coll);
    std::unique_ptr<AggExState> aggExState = createDefaultAggExState(view);
    std::unique_ptr<AggCatalogState> aggCatalogState = aggExState->createAggCatalogState();

    ASSERT_DOES_NOT_THROW(aggCatalogState->validate());

    ASSERT_TRUE(aggCatalogState->getMainCollectionOrView().isView());
    ASSERT_TRUE(aggCatalogState->lockAcquired());

    boost::optional<AutoStatsTracker> tracker;
    aggCatalogState->getStatsTrackerIfNeeded(tracker);
    ASSERT_FALSE(tracker.has_value());

    auto [collator, matchesDefault] = aggCatalogState->resolveCollator();
    ASSERT_TRUE(CollatorInterface::isSimpleCollator(collator.get()));
    ASSERT_EQ(matchesDefault, ExpressionContextCollationMatchesDefault::kYes);

    // Check the resolved view correspond to the expected one
    auto resolvedView = aggCatalogState->resolveView(operationContext(), viewNss, boost::none);
    ASSERT_TRUE(resolvedView.isOK());
    ASSERT_EQ(resolvedView.getValue().getNamespace(), viewOn);
    std::vector<BSONObj> result = resolvedView.getValue().getPipeline();
    ASSERT_EQ(expectedPipeline.size(), result.size());
    for (uint32_t i = 0; i < expectedPipeline.size(); i++) {
        ASSERT(SimpleBSONObjComparator::kInstance.evaluate(expectedPipeline[i] == result[i]));
    }

    // It's a view so apparently there is no main collection, per se.
    ASSERT_FALSE(aggCatalogState->getCollections().hasMainCollection());

    ASSERT_FALSE(aggCatalogState->getUUID().has_value());

    ASSERT_DOES_NOT_THROW(aggCatalogState->relinquishResources());
}

TEST_F(AggregationExecutionStateTest, UnshardedSecondaryNssRequiresExtendedRangeSupport) {
    StringData main{"coll"};
    StringData timeseriesColl{"timeseries"};

    createTestCollection(main, false /*sharded*/);
    createTimeseriesCollection(
        timeseriesColl, false /*sharded*/, true /*requiresExtendedRangeSupport*/);

    auto aggExState = createDefaultAggExStateWithSecondaryCollections(main, timeseriesColl);
    auto aggCatalogState = aggExState->createAggCatalogState();
    auto expCtx = aggCatalogState->createExpressionContext();

    ASSERT_TRUE(expCtx->getRequiresTimeseriesExtendedRangeSupport());
}

TEST_F(AggregationExecutionStateTest, ShardedSecondaryNssRequiresExtendedRangeSupport) {
    StringData main{"coll"};
    StringData timeseriesColl{"timeseries"};

    createTestCollection(main, true /*sharded*/);
    createTimeseriesCollection(
        timeseriesColl, true /*sharded*/, true /*requiresExtendedRangeSupport*/);

    auto aggExState = createDefaultAggExStateWithSecondaryCollections(main, timeseriesColl);
    auto aggCatalogState = aggExState->createAggCatalogState();
    auto expCtx = aggCatalogState->createExpressionContext();

    ASSERT_TRUE(expCtx->getRequiresTimeseriesExtendedRangeSupport());
}

TEST_F(AggregationExecutionStateTest, SecondaryNssNoExtendedRangeSupport) {
    StringData main{"coll"};
    StringData timeseriesColl{"timeseries"};

    createTestCollection(main, false /*sharded*/);
    createTimeseriesCollection(
        timeseriesColl, false /*sharded*/, false /*requiresExtendedRangeSupport*/);

    auto aggExState = createDefaultAggExStateWithSecondaryCollections(main, timeseriesColl);
    auto aggCatalogState = aggExState->createAggCatalogState();
    auto expCtx = aggCatalogState->createExpressionContext();

    ASSERT_FALSE(expCtx->getRequiresTimeseriesExtendedRangeSupport());
}

TEST_F(AggregationExecutionStateTest, CreateOplogAggCatalogState) {
    StringData coll{"coll"};
    createTestCollection(coll, false /*sharded*/);
    std::unique_ptr<AggExState> aggExState = createOplogAggExState(coll);
    std::unique_ptr<AggCatalogState> aggCatalogState = aggExState->createAggCatalogState();

    ASSERT_DOES_NOT_THROW(aggCatalogState->validate());

    ASSERT_TRUE(aggCatalogState->lockAcquired());

    boost::optional<AutoStatsTracker> tracker;
    aggCatalogState->getStatsTrackerIfNeeded(tracker);
    ASSERT_FALSE(tracker.has_value());

    auto [collator, matchesDefault] = aggCatalogState->resolveCollator();
    ASSERT_TRUE(CollatorInterface::isSimpleCollator(collator.get()));
    ASSERT_EQ(matchesDefault, ExpressionContextCollationMatchesDefault::kYes);

    ASSERT_TRUE(aggCatalogState->getCollections().hasMainCollection());

    // UUIDs are not used for change stream queries.
    ASSERT_FALSE(aggCatalogState->getUUID().has_value());

    ASSERT_DOES_NOT_THROW(aggCatalogState->relinquishResources());
}

TEST_F(AggregationExecutionStateTest, CreateOplogAggCatalogStateFailsOnView) {
    StringData coll{"coll"};
    StringData view{"view"};
    createTestCollection(coll, false /*sharded*/);
    createTestView(view, coll);

    std::unique_ptr<AggExState> aggExState = createOplogAggExState(view);

    // This will call the validate() method which will fail because you cannot open a change
    // stream on a view.
    ASSERT_THROWS_CODE(
        aggExState->createAggCatalogState(), DBException, ErrorCodes::CommandNotSupportedOnView);
}

}  // namespace
}  // namespace mongo
