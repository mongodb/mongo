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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/resharding/resharding_oplog_fetcher.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using namespace unittest;

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
    OperationContext* _opCtx;
    ServiceContext* _svcCtx;
    UUID _reshardingUUID = UUID::gen();
    Timestamp _fetchTimestamp;
    ShardId _donorShard;
    ShardId _destinationShard;

    void setUp() {
        ShardServerTestFixture::setUp();
        _opCtx = operationContext();
        _svcCtx = _opCtx->getServiceContext();

        for (const auto& shardId : kTwoShardIdList) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(operationContext(), shardId))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(makeHostAndPort(shardId));
        }

        WaitForMajorityService::get(getServiceContext()).setUp(getServiceContext());

        // onStepUp() relies on the storage interface to create the config.transactions table.
        repl::StorageInterface::set(getServiceContext(),
                                    std::make_unique<repl::StorageInterfaceImpl>());
        MongoDSessionCatalog::onStepUp(operationContext());
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        _fetchTimestamp = queryOplog(BSONObj())["ts"].timestamp();

        _donorShard = kTwoShardIdList[0];
        _destinationShard = kTwoShardIdList[1];
    }

    void tearDown() {
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ShardServerTestFixture::tearDown();
    }

    /**
     * Override the CatalogClient to make CatalogClient::getAllShards automatically return the
     * expected shards. We cannot mock the network responses for the ShardRegistry reload, since the
     * ShardRegistry reload is done over DBClient, not the NetworkInterface, and there is no
     * DBClientMock analogous to the NetworkInterfaceMock.
     */
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient(std::vector<ShardId> shardIds)
                : ShardingCatalogClientMock(nullptr), _shardIds(std::move(shardIds)) {}

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
        NamespaceString slimNss =
            NamespaceString("local.system.resharding.slimOplogForGraphLookup");

        boost::intrusive_ptr<ExpressionContextForTest> expCtx(
            new ExpressionContextForTest(_opCtx, NamespaceString::kRsOplogNamespace));
        expCtx->setResolvedNamespace(NamespaceString::kRsOplogNamespace,
                                     {NamespaceString::kRsOplogNamespace, {}});
        expCtx->setResolvedNamespace(slimNss,
                                     {slimNss, std::vector<BSONObj>{getSlimOplogPipeline()}});
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
            AutoGetOrCreateDb dbRaii(_opCtx, nss.db(), LockMode::MODE_X);
            WriteUnitOfWork wunit(_opCtx);
            if (_opCtx->recoveryUnit()->getCommitTimestamp().isNull()) {
                ASSERT_OK(_opCtx->recoveryUnit()->setTimestamp(Timestamp(1, 1)));
            }
            invariant(dbRaii.getDb()->createCollection(_opCtx, nss));
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
                BSON("msg" << fmt::format("Writes to {} are temporarily blocked for resharding.",
                                          dataColl.getCollection()->ns().toString())),
                BSON("type"
                     << "reshardFinalOp"
                     << "reshardingUUID" << _reshardingUUID),
                boost::none,
                boost::none,
                boost::none,
                boost::none);
            wuow.commit();
        }

        repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);

        // Disable the failpoint.
        setGlobalFailPoint("addDestinedRecipient",
                           BSON("mode"
                                << "off"));
    }

protected:
    const std::vector<ShardId> kTwoShardIdList{{"s1"}, {"s2"}};

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
        ReshardingOplogFetcher fetcher(_reshardingUUID,
                                       dataColl->uuid(),
                                       {_fetchTimestamp, _fetchTimestamp},
                                       _donorShard,
                                       _destinationShard,
                                       true,
                                       outputCollectionNss);
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);

        fetcher.iterate(&cc());
    });

    requestPassthroughHandler(fetcherJob);

    // Five oplog entries for resharding + the sentinel final oplog entry.
    ASSERT_EQ(6, itcount(outputCollectionNss));
}

TEST_F(ReshardingOplogFetcherTest, TestTrackLastSeen) {
    const NamespaceString outputCollectionNss("dbtests.outputCollection");
    const NamespaceString dataCollectionNss("dbtests.runFetchIteration");

    setupBasic(outputCollectionNss, dataCollectionNss, _destinationShard);

    AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);

    const int maxBatches = 1;
    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RefetcherRunner", _svcCtx, nullptr);

        ReshardingOplogFetcher fetcher(_reshardingUUID,
                                       dataColl->uuid(),
                                       {_fetchTimestamp, _fetchTimestamp},
                                       _donorShard,
                                       _destinationShard,
                                       true,
                                       outputCollectionNss);
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);
        fetcher.setMaxBatchesForTest(maxBatches);

        fetcher.iterate(&cc());
        return fetcher.getLastSeenTimestamp();
    });

    ReshardingDonorOplogId lastSeen = requestPassthroughHandler(fetcherJob, maxBatches);

    // Two oplog entries due to the batch size.
    ASSERT_EQ(2, itcount(outputCollectionNss));
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
        ReshardingOplogFetcher fetcher(_reshardingUUID,
                                       dataColl->uuid(),
                                       {doesNotExist, doesNotExist},
                                       _donorShard,
                                       _destinationShard,
                                       true,
                                       outputCollectionNss);
        fetcher.useReadConcernForTest(false);

        // Status has a private default constructor so we wrap it in a boost::optional to placate
        // the Windows compiler.
        try {
            fetcher.iterate(&cc());
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
    ReshardingOplogFetcher fetcher(_reshardingUUID,
                                   collectionUUID,
                                   startAt,
                                   _donorShard,
                                   _destinationShard,
                                   true,
                                   outputCollectionNss);

    // The ReshardingOplogFetcher hasn't inserted a record for
    // {_id: {clusterTime: _fetchTimestamp, ts: _fetchTimestamp}} so awaitInsert(startAt) won't be
    // immediately ready.
    auto hasSeenStartAtFuture = fetcher.awaitInsert(startAt);
    ASSERT_FALSE(hasSeenStartAtFuture.isReady());

    // iterate() won't lead to any documents being inserted into the output collection (because no
    // writes have happened to the data collection) so `hasSeenStartAtFuture` still won't be ready.
    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx, nullptr);
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);
        return fetcher.iterate(&cc());
    });
    ASSERT_TRUE(requestPassthroughHandler(fetcherJob));
    ASSERT_FALSE(hasSeenStartAtFuture.isReady());

    // Insert a document into the data collection and have it generate an oplog entry with a
    // "destinedRecipient" field. Only after iterate() is called again and inserts a record into the
    // output collection will `hasSeenStartAtFuture` have become ready.
    auto dataWriteTimestamp = [&] {
        FailPointEnableBlock fp("addDestinedRecipient",
                                BSON("destinedRecipient" << _destinationShard.toString()));

        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        WriteUnitOfWork wuow(_opCtx);
        insertDocument(dataColl.getCollection(), InsertStatement(BSON("_id" << 1 << "a" << 1)));
        wuow.commit();

        repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);
        return repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);
    }();
    ASSERT_FALSE(hasSeenStartAtFuture.isReady());

    fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx, nullptr);
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);
        return fetcher.iterate(&cc());
    });
    ASSERT_TRUE(requestPassthroughHandler(fetcherJob));
    ASSERT_TRUE(hasSeenStartAtFuture.isReady());

    // Asking for `startAt` again would return an immediately ready future.
    ASSERT_TRUE(fetcher.awaitInsert(startAt).isReady());

    // However, asking for `dataWriteTimestamp` wouldn't become ready until the next record is
    // insert into the output collection.
    ASSERT_FALSE(fetcher.awaitInsert({dataWriteTimestamp, dataWriteTimestamp}).isReady());
}

}  // namespace
}  // namespace mongo
