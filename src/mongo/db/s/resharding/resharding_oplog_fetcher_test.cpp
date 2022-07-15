/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include <boost/optional.hpp>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_fetcher.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

/**
 * RAII type for operating at a timestamp. Will remove any timestamping when the object destructs.
 */
class OneOffRead {
public:
    OneOffRead(OperationContext* opCtx, const Timestamp& ts) : _opCtx(opCtx) {
        _opCtx->recoveryUnit()->abandonSnapshot();
        if (ts.isNull()) {
            _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
        } else {
            _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, ts);
        }
    }

    ~OneOffRead() {
        _opCtx->recoveryUnit()->abandonSnapshot();
        _opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
    }

private:
    OperationContext* _opCtx;
};

class ReshardingOplogFetcherTest : public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();
        _opCtx = operationContext();
        _svcCtx = _opCtx->getServiceContext();

        {
            Lock::GlobalWrite lk(_opCtx);
            OldClientContext ctx(_opCtx, NamespaceString::kRsOplogNamespace);
        }

        _metrics = ReshardingMetrics::makeInstance(_reshardingUUID,
                                                   BSON("y" << 1),
                                                   NamespaceString{""},
                                                   ReshardingMetrics::Role::kRecipient,
                                                   getServiceContext()->getFastClockSource()->now(),
                                                   getServiceContext());

        for (const auto& shardId : kTwoShardIdList) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(operationContext(), shardId))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(makeHostAndPort(shardId));
        }

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());

        // onStepUp() relies on the storage interface to create the config.transactions table.
        repl::StorageInterface::set(getServiceContext(),
                                    std::make_unique<repl::StorageInterfaceImpl>());
        MongoDSessionCatalog::onStepUp(operationContext());
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        _fetchTimestamp = queryOplog(BSONObj())["ts"].timestamp();

        _donorShard = kTwoShardIdList[0];
        _destinationShard = kTwoShardIdList[1];
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ShardServerTestFixture::tearDown();
    }

    auto makeFetcherEnv() {
        return std::make_unique<ReshardingOplogFetcher::Env>(_svcCtx, _metrics.get());
    }

    /**
     * Override the CatalogClient to make CatalogClient::getAllShards automatically return the
     * expected shards. We cannot mock the network responses for the ShardRegistry reload, since the
     * ShardRegistry reload is done over DBClient, not the NetworkInterface, and there is no
     * DBClientMock analogous to the NetworkInterfaceMock.
     */
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient(std::vector<ShardId> shardIds) : _shardIds(std::move(shardIds)) {}

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {
                std::vector<ShardType> shardTypes;
                for (const auto& shardId : _shardIds) {
                    const ConnectionString cs = ConnectionString::forReplicaSet(
                        shardId.toString(), {makeHostAndPort(shardId)});
                    ShardType sType;
                    sType.setName(cs.getSetName());
                    sType.setHost(cs.toString());
                    shardTypes.push_back(std::move(sType));
                };
                return repl::OpTimeWith<std::vector<ShardType>>(shardTypes);
            }

        private:
            const std::vector<ShardId> _shardIds;
        };

        return std::make_unique<StaticCatalogClient>(kTwoShardIdList);
    }

    void insertDocument(const CollectionPtr& coll, const InsertStatement& stmt) {
        // Insert some documents.
        OpDebug* const nullOpDebug = nullptr;
        const bool fromMigrate = false;
        ASSERT_OK(coll->insertDocument(_opCtx, stmt, nullOpDebug, fromMigrate));
    }

    BSONObj queryCollection(NamespaceString nss, const BSONObj& query) {
        BSONObj ret;
        ASSERT_TRUE(Helpers::findOne(
            _opCtx, AutoGetCollectionForRead(_opCtx, nss).getCollection(), query, ret))
            << "Query: " << query;
        return ret;
    }

    BSONObj queryOplog(const BSONObj& query) {
        OneOffRead oor(_opCtx, Timestamp::min());
        return queryCollection(NamespaceString::kRsOplogNamespace, query);
    }

    repl::OpTime getLastApplied() {
        return repl::ReplicationCoordinator::get(_opCtx)->getMyLastAppliedOpTime();
    }

    boost::intrusive_ptr<ExpressionContextForTest> createExpressionContext() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(
            new ExpressionContextForTest(_opCtx, NamespaceString::kRsOplogNamespace));
        expCtx->setResolvedNamespace(NamespaceString::kRsOplogNamespace,
                                     {NamespaceString::kRsOplogNamespace, {}});
        return expCtx;
    }

    int itcount(NamespaceString nss) {
        OneOffRead oof(_opCtx, Timestamp::min());
        AutoGetCollectionForRead autoColl(_opCtx, nss);
        auto cursor = autoColl.getCollection()->getCursor(_opCtx);

        int ret = 0;
        while (auto rec = cursor->next()) {
            ++ret;
        }

        return ret;
    }

    void create(NamespaceString nss) {
        writeConflictRetry(_opCtx, "create", nss.ns(), [&] {
            AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(_opCtx->lockState());
            AutoGetDb autoDb(_opCtx, nss.dbName(), LockMode::MODE_X);
            WriteUnitOfWork wunit(_opCtx);
            if (_opCtx->recoveryUnit()->getCommitTimestamp().isNull()) {
                ASSERT_OK(_opCtx->recoveryUnit()->setTimestamp(Timestamp(1, 1)));
            }

            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(_opCtx);
            auto db = autoDb.ensureDbExists(_opCtx);
            ASSERT(db->createCollection(_opCtx, nss)) << nss;
            wunit.commit();
        });
    }

    template <typename T>
    T requestPassthroughHandler(executor::NetworkTestEnv::FutureHandle<T>& future,
                                int maxBatches = -1) {

        int maxNumRequests = 1000;  // No unittests would request more than this?
        if (maxBatches > -1) {
            // The fetcher will send a `killCursors` after the last `getMore`.
            maxNumRequests = maxBatches + 1;
        }

        bool hasMore = true;
        for (int batchNum = 0; hasMore && batchNum < maxNumRequests; ++batchNum) {
            onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
                DBDirectClient client(cc().getOperationContext());
                BSONObj result;
                bool res = client.runCommand(request.dbname, request.cmdObj, result);
                if (res == false || result.hasField("cursorsKilled") ||
                    result["cursor"]["id"].Long() == 0) {
                    hasMore = false;
                }

                return result;
            });
        }

        return future.timed_get(Seconds(5));
    }

    // Writes five documents to `dataCollectionNss` that are replicated with a `destinedRecipient`
    // followed by the final no-op oplog entry that signals the last oplog entry needed to be
    // applied for resharding to move to the next stage.
    void setupBasic(NamespaceString outputCollectionNss,
                    NamespaceString dataCollectionNss,
                    ShardId destinedRecipient) {
        create(outputCollectionNss);
        create(dataCollectionNss);
        _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

        {
            AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);

            // Set a failpoint to tack a `destinedRecipient` onto oplog entries.
            setGlobalFailPoint("addDestinedRecipient",
                               BSON("mode"
                                    << "alwaysOn"
                                    << "data"
                                    << BSON("destinedRecipient" << destinedRecipient.toString())));

            // Insert five documents. Advance the majority point.
            const std::int32_t docsToInsert = 5;
            {
                for (std::int32_t num = 0; num < docsToInsert; ++num) {
                    WriteUnitOfWork wuow(_opCtx);
                    insertDocument(dataColl.getCollection(),
                                   InsertStatement(BSON("_id" << num << "a" << num)));
                    wuow.commit();
                }
            }

            // Write an entry saying that fetching is complete.
            {
                WriteUnitOfWork wuow(_opCtx);
                _opCtx->getServiceContext()->getOpObserver()->onInternalOpMessage(
                    _opCtx,
                    dataColl.getCollection()->ns(),
                    dataColl.getCollection()->uuid(),
                    BSON(
                        "msg" << fmt::format("Writes to {} are temporarily blocked for resharding.",
                                             dataColl.getCollection()->ns().toString())),
                    BSON("type" << resharding::kReshardFinalOpLogType << "reshardingUUID"
                                << _reshardingUUID),
                    boost::none,
                    boost::none,
                    boost::none,
                    boost::none);
                wuow.commit();
            }
        }

        repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);

        // Disable the failpoint.
        setGlobalFailPoint("addDestinedRecipient",
                           BSON("mode"
                                << "off"));
    }

    long long metricsFetchedCount() const {
        auto curOp = _metrics->reportForCurrentOp();
        return curOp["oplogEntriesFetched"_sd].Long();
    }

    CancelableOperationContextFactory makeCancelableOpCtx() {
        auto cancelableOpCtxExecutor = std::make_shared<ThreadPool>([] {
            ThreadPool::Options options;
            options.poolName = "TestReshardOplogFetcherCancelableOpCtxPool";
            options.minThreads = 1;
            options.maxThreads = 1;
            return options;
        }());

        return CancelableOperationContextFactory(operationContext()->getCancellationToken(),
                                                 cancelableOpCtxExecutor);
    }

protected:
    const std::vector<ShardId> kTwoShardIdList{{"s1"}, {"s2"}};

    OperationContext* _opCtx;
    ServiceContext* _svcCtx;
    UUID _reshardingUUID = UUID::gen();
    Timestamp _fetchTimestamp;
    ShardId _donorShard;
    ShardId _destinationShard;
    std::unique_ptr<ReshardingMetrics> _metrics;

private:
    static HostAndPort makeHostAndPort(const ShardId& shardId) {
        return HostAndPort(str::stream() << shardId << ":123");
    }
};

TEST_F(ReshardingOplogFetcherTest, TestBasic) {
    const NamespaceString outputCollectionNss("dbtests.outputCollection");
    const NamespaceString dataCollectionNss("dbtests.runFetchIteration");

    setupBasic(outputCollectionNss, dataCollectionNss, _destinationShard);

    AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RefetchRunner", _svcCtx, nullptr);
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       dataColl->uuid(),
                                       {_fetchTimestamp, _fetchTimestamp},
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss);
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);

        auto factory = makeCancelableOpCtx();
        fetcher.iterate(&cc(), factory);
    });

    requestPassthroughHandler(fetcherJob);

    // Five oplog entries for resharding + the sentinel final oplog entry.
    ASSERT_EQ(6, itcount(outputCollectionNss));
    ASSERT_EQ(6, metricsFetchedCount()) << " Verify reported metrics";
}

TEST_F(ReshardingOplogFetcherTest, TestTrackLastSeen) {
    const NamespaceString outputCollectionNss("dbtests.outputCollection");
    const NamespaceString dataCollectionNss("dbtests.runFetchIteration");

    setupBasic(outputCollectionNss, dataCollectionNss, _destinationShard);

    AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);

    const int maxBatches = 1;
    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RefetcherRunner", _svcCtx, nullptr);

        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       dataColl->uuid(),
                                       {_fetchTimestamp, _fetchTimestamp},
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss);
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);
        fetcher.setMaxBatchesForTest(maxBatches);

        auto factory = makeCancelableOpCtx();
        fetcher.iterate(&cc(), factory);
        return fetcher.getLastSeenTimestamp();
    });

    ReshardingDonorOplogId lastSeen = requestPassthroughHandler(fetcherJob, maxBatches);

    // Two oplog entries due to the batch size.
    ASSERT_EQ(2, itcount(outputCollectionNss));
    ASSERT_EQ(2, metricsFetchedCount()) << " Verify reported metrics";
    // Assert the lastSeen value has been bumped from the original `_fetchTimestamp`.
    ASSERT_GT(lastSeen.getTs(), _fetchTimestamp);
}

TEST_F(ReshardingOplogFetcherTest, TestFallingOffOplog) {
    const NamespaceString outputCollectionNss("dbtests.outputCollection");
    const NamespaceString dataCollectionNss("dbtests.runFetchIteration");

    setupBasic(outputCollectionNss, dataCollectionNss, _destinationShard);

    AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);

    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RefetcherRunner", _svcCtx, nullptr);

        const Timestamp doesNotExist(1, 1);
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       dataColl->uuid(),
                                       {doesNotExist, doesNotExist},
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss);
        fetcher.useReadConcernForTest(false);

        // Status has a private default constructor so we wrap it in a boost::optional to placate
        // the Windows compiler.
        try {
            auto factory = makeCancelableOpCtx();
            fetcher.iterate(&cc(), factory);
            // Test failure case.
            return boost::optional<Status>(Status::OK());
        } catch (...) {
            return boost::optional<Status>(exceptionToStatus());
        }
    });

    auto fetcherStatus = requestPassthroughHandler(fetcherJob);

    // Two oplog entries due to the batch size.
    ASSERT_EQ(0, itcount(outputCollectionNss));
    ASSERT_EQ(ErrorCodes::OplogQueryMinTsMissing, fetcherStatus->code());
}

TEST_F(ReshardingOplogFetcherTest, TestAwaitInsert) {
    const NamespaceString outputCollectionNss("dbtests.outputCollection");
    const NamespaceString dataCollectionNss("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    ReshardingDonorOplogId startAt{_fetchTimestamp, _fetchTimestamp};
    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   startAt,
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss);

    // The ReshardingOplogFetcher hasn't inserted a record for
    // {_id: {clusterTime: _fetchTimestamp, ts: _fetchTimestamp}} so awaitInsert(startAt) won't be
    // immediately ready.
    auto hasSeenStartAtFuture = fetcher.awaitInsert(startAt);
    ASSERT_FALSE(hasSeenStartAtFuture.isReady());

    // Because no writes have happened to the data collection, the fetcher will insert a no-op entry
    // with the latestOplogTimestamp, so `hasSeenStartAtFuture` will be ready.
    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx, nullptr);
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);
        auto factory = makeCancelableOpCtx();
        return fetcher.iterate(&cc(), factory);
    });

    ASSERT_TRUE(requestPassthroughHandler(fetcherJob));
    ASSERT_TRUE(hasSeenStartAtFuture.isReady());

    // Insert a document into the data collection and have it generate an oplog entry with a
    // "destinedRecipient" field.
    auto dataWriteTimestamp = [&] {
        FailPointEnableBlock fp("addDestinedRecipient",
                                BSON("destinedRecipient" << _destinationShard.toString()));

        {
            AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(dataColl.getCollection(), InsertStatement(BSON("_id" << 1 << "a" << 1)));
            wuow.commit();
        }

        repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);
        return repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);
    }();

    fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx, nullptr);
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);
        auto factory = makeCancelableOpCtx();
        return fetcher.iterate(&cc(), factory);
    });
    ASSERT_TRUE(requestPassthroughHandler(fetcherJob));
    ASSERT_TRUE(hasSeenStartAtFuture.isReady());

    // Asking for `startAt` again would return an immediately ready future.
    ASSERT_TRUE(fetcher.awaitInsert(startAt).isReady());

    // However, asking for `dataWriteTimestamp` wouldn't become ready until the next record is
    // inserted into the output collection.
    ASSERT_FALSE(fetcher.awaitInsert({dataWriteTimestamp, dataWriteTimestamp}).isReady());
}

TEST_F(ReshardingOplogFetcherTest, TestStartAtUpdatedWithProgressMarkOplogTs) {
    const NamespaceString outputCollectionNss("dbtests.outputCollection");
    const NamespaceString dataCollectionNss("dbtests.runFetchIteration");
    const NamespaceString otherCollection("dbtests.collectionNotBeingResharded");

    create(outputCollectionNss);
    create(dataCollectionNss);
    create(otherCollection);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    ReshardingDonorOplogId startAt{_fetchTimestamp, _fetchTimestamp};
    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   startAt,
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss);

    // Insert a document into the data collection and have it generate an oplog entry with a
    // "destinedRecipient" field.
    auto writeToDataCollectionTs = [&] {
        FailPointEnableBlock fp("addDestinedRecipient",
                                BSON("destinedRecipient" << _destinationShard.toString()));

        {
            AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(dataColl.getCollection(), InsertStatement(BSON("_id" << 1 << "a" << 1)));
            wuow.commit();
        }

        repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);
        return repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);
    }();

    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx, nullptr);
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);
        auto factory = makeCancelableOpCtx();
        return fetcher.iterate(&cc(), factory);
    });
    ASSERT_TRUE(requestPassthroughHandler(fetcherJob));

    // The fetcher's lastSeenTimestamp should be equal to `writeToDataCollectionTs`.
    ASSERT_TRUE(fetcher.getLastSeenTimestamp().getClusterTime() == writeToDataCollectionTs);
    ASSERT_TRUE(fetcher.getLastSeenTimestamp().getTs() == writeToDataCollectionTs);
    ASSERT_EQ(1, metricsFetchedCount()) << " Verify reported metrics";

    // Now, insert a document into a different collection that is not involved in resharding.
    auto writeToOtherCollectionTs = [&] {
        {
            AutoGetCollection dataColl(_opCtx, otherCollection, LockMode::MODE_IX);
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(dataColl.getCollection(), InsertStatement(BSON("_id" << 1 << "a" << 1)));
            wuow.commit();
        }

        repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);
        return repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);
    }();

    fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx, nullptr);
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);
        auto factory = makeCancelableOpCtx();
        return fetcher.iterate(&cc(), factory);
    });
    ASSERT_TRUE(requestPassthroughHandler(fetcherJob));

    // The fetcher's lastSeenTimestamp should now be equal to `writeToOtherCollectionTs`
    // because the lastSeenTimestamp will be updated with the latest oplog timestamp from the
    // donor's cursor response.
    ASSERT_TRUE(fetcher.getLastSeenTimestamp().getClusterTime() == writeToOtherCollectionTs);
    ASSERT_TRUE(fetcher.getLastSeenTimestamp().getTs() == writeToOtherCollectionTs);
    ASSERT_EQ(2, metricsFetchedCount()) << " Verify reported metrics";

    // The last document returned by ReshardingDonorOplogIterator::getNextBatch() would be
    // `writeToDataCollectionTs`, but ReshardingOplogFetcher would have inserted a doc with
    // `writeToOtherCollectionTs` after this so `awaitInsert` should be immediately ready when
    // passed `writeToDataCollectionTs`.
    ASSERT_TRUE(fetcher.awaitInsert({writeToDataCollectionTs, writeToDataCollectionTs}).isReady());

    // `awaitInsert` should not be ready if passed `writeToOtherCollectionTs`.
    ASSERT_FALSE(
        fetcher.awaitInsert({writeToOtherCollectionTs, writeToOtherCollectionTs}).isReady());
}

TEST_F(ReshardingOplogFetcherTest, RetriesOnRemoteInterruptionError) {
    const NamespaceString outputCollectionNss("dbtests.outputCollection");
    const NamespaceString dataCollectionNss("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx, nullptr);

        ReshardingDonorOplogId startAt{_fetchTimestamp, _fetchTimestamp};
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       collectionUUID,
                                       startAt,
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss);
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);

        auto factory = makeCancelableOpCtx();
        return fetcher.iterate(&cc(), factory);
    });

    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // Simulate the remote donor shard stepping down or transitioning into rollback.
        return {ErrorCodes::InterruptedDueToReplStateChange, "operation was interrupted"};
    });

    auto moreToCome = fetcherJob.timed_get(Seconds(5));
    ASSERT_TRUE(moreToCome);
}

TEST_F(ReshardingOplogFetcherTest, ImmediatelyDoneWhenFinalOpHasAlreadyBeenFetched) {
    const NamespaceString outputCollectionNss("dbtests.outputCollection");
    const NamespaceString dataCollectionNss("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   ReshardingOplogFetcher::kFinalOpAlreadyFetched,
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss);

    auto factory = makeCancelableOpCtx();
    auto future = fetcher.schedule(nullptr, CancellationToken::uncancelable(), factory);

    ASSERT_TRUE(future.isReady());
    ASSERT_OK(future.getNoThrow());
}

DEATH_TEST_REGEX_F(ReshardingOplogFetcherTest,
                   CannotFetchMoreWhenFinalOpHasAlreadyBeenFetched,
                   "Invariant failure.*_startAt != kFinalOpAlreadyFetched") {
    const NamespaceString outputCollectionNss("dbtests.outputCollection");
    const NamespaceString dataCollectionNss("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx, nullptr);

        // We intentionally do not call fetcher.useReadConcernForTest(false) for this test case.
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       collectionUUID,
                                       ReshardingOplogFetcher::kFinalOpAlreadyFetched,
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss);
        fetcher.setInitialBatchSizeForTest(2);

        auto factory = makeCancelableOpCtx();
        return fetcher.iterate(&cc(), factory);
    });

    // Calling onCommand() leads to a more helpful "Expected death, found life" error when the
    // invariant failure isn't triggered.
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return {ErrorCodes::InternalError, "this error should never be observed"};
    });

    (void)fetcherJob.timed_get(Seconds(5));
}

}  // namespace
}  // namespace mongo
