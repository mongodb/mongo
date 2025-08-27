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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/kill_cursors_gen.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_noop_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_oplog_fetcher.h"
#include "mongo/db/s/resharding/resharding_oplog_fetcher_progress_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/s/sharding_task_executor.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <string>
#include <system_error>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

repl::MutableOplogEntry makeOplog(const NamespaceString& nss,
                                  const UUID& uuid,
                                  const repl::OpTypeEnum& opType,
                                  const BSONObj& oField,
                                  const BSONObj& o2Field,
                                  const Date_t wallClockTime,
                                  const ReshardingDonorOplogId& oplogId) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    oplogEntry.setOpType(opType);
    oplogEntry.setObject(oField);

    if (!o2Field.isEmpty()) {
        oplogEntry.setObject2(o2Field);
    }

    oplogEntry.setOpTime({{}, {}});
    oplogEntry.setWallClockTime(wallClockTime);
    oplogEntry.set_id(Value(oplogId.toBSON()));

    return oplogEntry;
}

/**
 * RAII type for operating at a timestamp. Will remove any timestamping when the object destructs.
 */
class OneOffRead {
public:
    OneOffRead(OperationContext* opCtx, const Timestamp& ts, bool waitForOplog = false)
        : _opCtx(opCtx) {
        shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        if (waitForOplog) {
            auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
            LocalOplogInfo* oplogInfo = LocalOplogInfo::get(opCtx);

            // Oplog should be available in this test.
            invariant(oplogInfo);
            storageEngine->waitForAllEarlierOplogWritesToBeVisible(opCtx,
                                                                   oplogInfo->getRecordStore());
        }
        if (ts.isNull()) {
            shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kNoTimestamp);
        } else {
            shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kProvided, ts);
        }
    }

    ~OneOffRead() {
        shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
            RecoveryUnit::ReadSource::kNoTimestamp);
    }

private:
    OperationContext* _opCtx;
};

class ReshardingOplogFetcherTest : public ShardServerTestFixture {
public:
    ReshardingOplogFetcherTest()
        : ShardServerTestFixture(
              Options{}.useMockClock(true).useMockTickSource<Milliseconds>(true)) {}

    void setUp() override {
        ShardServerTestFixture::setUp();
        _opCtx = operationContext();
        _svcCtx = _opCtx->getServiceContext();

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
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(operationContext());
        mongoDSessionCatalog->onStepUp(operationContext());
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());

        resetResharding();
        // In practice, the progress collection is created by ReshardingDataReplication before
        // creating the ReshardingOplogFetchers.
        create(NamespaceString::kReshardingFetcherProgressNamespace);
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ShardServerTestFixture::tearDown();
    }

    void resetResharding() {
        _reshardingUUID = UUID::gen();
        _metrics = ReshardingMetrics::makeInstance_forTest(
            _reshardingUUID,
            kShardKey,
            NamespaceString::kEmpty,
            ReshardingMetrics::Role::kRecipient,
            getServiceContext()->getFastClockSource()->now(),
            getServiceContext());
        _fetchTimestamp = queryOplog(BSONObj())["ts"].timestamp();
        _donorShard = kTwoShardIdList[0];
        _destinationShard = kTwoShardIdList[1];
        _metrics->registerDonors({_donorShard});
    }

    auto makeFetcherEnv() {
        return std::make_unique<ReshardingOplogFetcher::Env>(_svcCtx, _metrics.get());
    }

    auto makeExecutor() {
        ThreadPool::Options threadPoolOpts;
        threadPoolOpts.maxThreads = 100;
        threadPoolOpts.threadNamePrefix = "ReshardingOplogFetcherTest-";
        threadPoolOpts.poolName = "ReshardingOplogFetcherTestThreadPool";
        return executor::ThreadPoolTaskExecutor::create(
            std::make_unique<ThreadPool>(threadPoolOpts),
            std::make_unique<executor::NetworkInterfaceMock>());
    }

    /**
     * Override the CatalogClient to make CatalogClient::getAllShards automatically return the
     * expected shards. We cannot mock the network responses for the ShardRegistry reload, since the
     * ShardRegistry reload is done over DBClient, not the NetworkInterface, and there is no
     * DBClientMock analogous to the NetworkInterfaceMock.
     */
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient(std::vector<ShardId> shardIds) : _shardIds(std::move(shardIds)) {}

            repl::OpTimeWith<std::vector<ShardType>> getAllShards(
                OperationContext* opCtx,
                repl::ReadConcernLevel readConcern,
                BSONObj filter) override {
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
        ASSERT_OK(
            collection_internal::insertDocument(_opCtx, coll, stmt, nullOpDebug, fromMigrate));
    }

    BSONObj queryCollection(NamespaceString nss, const BSONObj& query) {
        BSONObj ret;
        const auto coll = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                         repl::ReadConcernArgs::get(_opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        ASSERT_TRUE(Helpers::findOne(_opCtx, coll, query, ret)) << "Query: " << query;
        return ret;
    }

    BSONObj getLast(NamespaceString nss) {
        BSONObj ret;
        Helpers::getLast(_opCtx, nss, ret);
        return ret;
    }

    BSONObj queryOplog(const BSONObj& query) {
        OneOffRead oor(_opCtx, Timestamp::min(), true);
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

    int itcount(NamespaceString nss, BSONObj filter = BSONObj()) {
        OneOffRead oof(_opCtx, Timestamp::min(), nss.isOplog());

        DBDirectClient client(_opCtx);
        FindCommandRequest findRequest{nss};
        findRequest.setFilter(filter);
        auto cursor = client.find(std::move(findRequest));
        int ret = 0;
        while (cursor->more()) {
            cursor->next();
            ++ret;
        }

        return ret;
    }

    void create(NamespaceString nss) {
        writeConflictRetry(_opCtx, "create", nss, [&] {
            AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(
                shard_role_details::getLocker(_opCtx));
            AutoGetDb autoDb(_opCtx, nss.dbName(), LockMode::MODE_X);
            WriteUnitOfWork wunit(_opCtx);
            if (shard_role_details::getRecoveryUnit(_opCtx)->getCommitTimestamp().isNull()) {
                ASSERT_OK(
                    shard_role_details::getRecoveryUnit(_opCtx)->setTimestamp(Timestamp(1, 1)));
            }

            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(_opCtx, nss);
            auto db = autoDb.ensureDbExists(_opCtx);
            ASSERT(db->createCollection(_opCtx, nss)) << nss.toStringForErrorMsg();
            wunit.commit();
        });
    }

    BSONObj makeMockAggregateResponse(Timestamp postBatchResumeToken,
                                      BSONArray oplogEntries,
                                      CursorId cursorId = 0) {
        return BSON("cursor" << BSON("firstBatch"
                                     << oplogEntries << "postBatchResumeToken"
                                     << BSON("ts" << postBatchResumeToken) << "id" << cursorId
                                     << "ns"
                                     << NamespaceString::kRsOplogNamespace.toString_forTest()));
    };

    BSONObj makeMockGetMoreResponse(Timestamp postBatchResumeToken,
                                    BSONArray oplogEntries,
                                    CursorId cursorId = 0) {
        return BSON("cursor" << BSON("nextBatch"
                                     << oplogEntries << "postBatchResumeToken"
                                     << BSON("ts" << postBatchResumeToken) << "id" << cursorId
                                     << "ns"
                                     << NamespaceString::kRsOplogNamespace.toString_forTest()));
    };

    BSONObj makeFinalNoopOplogEntry(const NamespaceString& nss,
                                    const UUID& collectionUUID,
                                    Timestamp postBatchResumeToken) {
        return makeOplog(nss,
                         collectionUUID,
                         repl::OpTypeEnum::kNoop,
                         BSONObj(),
                         BSON("type" << resharding::kReshardFinalOpLogType << "reshardingUUID"
                                     << _reshardingUUID),
                         now(),
                         ReshardingDonorOplogId(postBatchResumeToken, postBatchResumeToken))
            .toBSON();
    }

    template <typename T>
    T requestPassthroughHandler(executor::NetworkTestEnv::FutureHandle<T>& future,
                                int maxBatches = -1,
                                boost::optional<BSONObj> mockResponse = boost::none) {

        int maxNumRequests = 1000;  // No unittests would request more than this?
        if (maxBatches > -1) {
            // The fetcher will send a `killCursors` after the last `getMore`.
            maxNumRequests = maxBatches + 1;
        }

        bool hasMore = true;
        for (int batchNum = 0; hasMore && batchNum < maxNumRequests; ++batchNum) {
            onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
                if (mockResponse) {
                    hasMore = false;
                    return StatusWith<BSONObj>(mockResponse.get());
                } else {
                    DBDirectClient client(cc().getOperationContext());
                    BSONObj result;
                    bool res = client.runCommand(request.dbname, request.cmdObj, result);
                    if (res == false || result.hasField("cursorsKilled") ||
                        result["cursor"]["id"].Long() == 0) {
                        hasMore = false;
                    }

                    return result;
                }
            });
        }

        return future.timed_get(Seconds(5));
    }

    // Generates the following oplog entries with `destinedRecipient` field attached:
    // - `numInsertOplogEntriesBeforeFinalOplogEntry` insert oplog entries.
    // - one no-op oplog entry indicating that fetching is complete and resharding should move to
    //   the next stage.
    // - `numNoopOplogEntriesAfterFinalOplogEntry` no-op oplog entries. The fetcher should discard
    //   all of these oplog entries.
    void setupBasic(NamespaceString outputCollectionNss,
                    NamespaceString dataCollectionNss,
                    ShardId destinedRecipient,
                    int numInsertOplogEntriesBeforeFinalOplogEntry = 5,
                    int approxInsertOplogEntrySizeBytes = 1,
                    int numNoopOplogEntriesAfterFinalOplogEntry = 0) {
        create(outputCollectionNss);
        create(dataCollectionNss);
        _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

        {
            const auto dataColl =
                acquireCollection(_opCtx,
                                  CollectionAcquisitionRequest{
                                      dataCollectionNss,
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(_opCtx),
                                      AcquisitionPrerequisites::kWrite},
                                  MODE_IX);

            // Set a failpoint to tack a `destinedRecipient` onto oplog entries.
            setGlobalFailPoint("addDestinedRecipient",
                               BSON("mode"
                                    << "alwaysOn"
                                    << "data"
                                    << BSON("destinedRecipient" << destinedRecipient.toString())));
            auto const& nss = dataColl.nss();
            auto const& uuid = dataColl.uuid();
            // Generate insert oplog entries by inserting documents.
            {
                for (std::int32_t num = 0; num < numInsertOplogEntriesBeforeFinalOplogEntry;
                     ++num) {
                    WriteUnitOfWork wuow(_opCtx);
                    insertDocument(
                        dataColl.getCollectionPtr(),
                        InsertStatement(
                            BSON("_id" << num << std::string(approxInsertOplogEntrySizeBytes, 'a')
                                       << num)));
                    wuow.commit();
                }
            }

            // Generate an noop entry indicating that fetching is complete.
            {
                WriteUnitOfWork wuow(_opCtx);
                _opCtx->getServiceContext()->getOpObserver()->onInternalOpMessage(
                    _opCtx,
                    nss,
                    uuid,
                    BSON(
                        "msg" << fmt::format("Writes to {} are temporarily blocked for resharding.",
                                             nss.toString_forTest())),
                    BSON("type" << resharding::kReshardFinalOpLogType << "reshardingUUID"
                                << _reshardingUUID),
                    boost::none,
                    boost::none,
                    boost::none,
                    boost::none);
                wuow.commit();
            }

            // Generate noop oplog entries.
            {
                for (std::int32_t num = 0; num < numNoopOplogEntriesAfterFinalOplogEntry; ++num) {
                    WriteUnitOfWork wuow(_opCtx);
                    _opCtx->getServiceContext()->getOpObserver()->onInternalOpMessage(
                        _opCtx,
                        nss,
                        uuid,
                        BSON("msg" << "other noop"),
                        boost::none /* o2 */,
                        boost::none,
                        boost::none,
                        boost::none,
                        boost::none);
                    wuow.commit();
                }
            }
        }

        repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);

        // Disable the failpoint.
        setGlobalFailPoint("addDestinedRecipient", BSON("mode" << "off"));
    }

    void assertUsedApplyOpsToBatchInsert(NamespaceString nss, int numApplyOpsOplogEntries) {
        ASSERT_EQ(0,
                  itcount(NamespaceString::kRsOplogNamespace,
                          BSON("op" << "i"
                                    << "ns" << nss.ns_forTest())));
        ASSERT_EQ(numApplyOpsOplogEntries,
                  itcount(NamespaceString::kRsOplogNamespace,
                          BSON("o.applyOps.op" << "i"
                                               << "o.applyOps.ns" << nss.ns_forTest())));
    }

    long long currentOpFetchedCount() const {
        auto curOp = _metrics->reportForCurrentOp();
        return curOp["oplogEntriesFetched"_sd].Long();
    }

    long long persistedFetchedCount(OperationContext* opCtx) const {
        DBDirectClient client(opCtx);
        auto sourceId = ReshardingSourceId{_reshardingUUID, _donorShard};
        auto doc = client.findOne(
            NamespaceString::kReshardingFetcherProgressNamespace,
            BSON(ReshardingOplogApplierProgress::kOplogSourceIdFieldName << sourceId.toBSON()));
        if (doc.isEmpty()) {
            return 0;
        }
        return doc[ReshardingOplogFetcherProgress::kNumEntriesFetchedFieldName].Long();
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
    ClockSourceMock* clockSource() {
        return dynamic_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

    TickSourceMock<Milliseconds>* tickSource() {
        return dynamic_cast<TickSourceMock<Milliseconds>*>(getServiceContext()->getTickSource());
    }

    Date_t now() {
        return clockSource()->now();
    }

    void advanceTime(Milliseconds millis) {
        clockSource()->advance(millis);
        tickSource()->advance(millis);
    }

    /**
     * Advances the mock clock forward to the next whole second.
     *
     * If the current time is already at a second boundary, the clock is not advanced. Otherwise,
     * the clock is advanced by the number of milliseconds needed so that the time is rounded up to
     * the next second (i.e. the milliseconds component becomes zero).
     */
    void advanceTimeToNextSecond() {
        long long currentMillis = now().toMillisSinceEpoch();
        long long millisToNextSecond =
            (1000 - (currentMillis % 1000)) % 1000;  // 0 if already at second boundary

        if (millisToNextSecond > 0) {
            advanceTime(Milliseconds(millisToNextSecond));
            tickSource()->advance(Milliseconds(millisToNextSecond));
        }
    }

    /**
     * Makes a cluster timestamp at the given timestamp. Guarantees the "i" field, which is an
     * increasing increment for differentiating operations within the same second, is strictly
     * increasing.
     */
    Timestamp makeClusterTimestampAt(Date_t date) {
        return Timestamp(date.toMillisSinceEpoch() / 1000, _clusterTimestampInc++);
    }

    /**
     * Makes a cluster timestamp at the current timestamp.
     */
    Timestamp makeClusterTimestampAtNow() {
        return makeClusterTimestampAt(now());
    }

    void testFetcherBasic(const NamespaceString& outputCollectionNss,
                          const NamespaceString& dataCollectionNss,
                          bool storeProgress,
                          boost::optional<int> initialAggregateBatchSize,
                          int expectedNumFetchedOplogEntries,
                          int expectedNumApplyOpsOplogEntries) {
        const auto dataColl = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest{dataCollectionNss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(_opCtx),
                                         AcquisitionPrerequisites::kWrite},
            MODE_IX);
        auto fetcherJob = launchAsync([&, this] {
            ThreadClient tc("RefetchRunner", _svcCtx->getService(), Client::noSession());
            ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                           _reshardingUUID,
                                           dataColl.uuid(),
                                           {_fetchTimestamp, _fetchTimestamp},
                                           _donorShard,
                                           _destinationShard,
                                           outputCollectionNss,
                                           storeProgress);
            fetcher.useReadConcernForTest(false);
            if (initialAggregateBatchSize) {
                fetcher.setInitialBatchSizeForTest(*initialAggregateBatchSize);
            }

            auto factory = makeCancelableOpCtx();
            fetcher.iterate(&cc(), factory);
        });

        requestPassthroughHandler(fetcherJob);

        ASSERT_EQ(expectedNumFetchedOplogEntries, itcount(outputCollectionNss));
        ASSERT_EQ(expectedNumFetchedOplogEntries, currentOpFetchedCount())
            << " Verify currentOp metrics";
        ASSERT_EQ(storeProgress ? expectedNumFetchedOplogEntries : 0, persistedFetchedCount(_opCtx))
            << " Verify persisted metrics";
        assertUsedApplyOpsToBatchInsert(outputCollectionNss, expectedNumApplyOpsOplogEntries);
    }

    void assertAggregateReadPreference(const executor::RemoteCommandRequest& request,
                                       const ReadPreferenceSetting& expectedReadPref) {
        auto parsedRequest = AggregateCommandRequest::parse(
            request.cmdObj.addFields(BSON("$db" << request.dbname.toString_forTest())),
            IDLParserContext("ReshardingOplogFetcherTest"));
        ASSERT_BSONOBJ_EQ(*parsedRequest.getUnwrappedReadPref(),
                          BSON("$readPreference" << expectedReadPref.toInnerBSON()));
    }

    void assertGetMoreCursorId(const executor::RemoteCommandRequest& request,
                               CursorId expectedCursorId) {
        auto parsedRequest = GetMoreCommandRequest::parse(
            request.cmdObj.addFields(BSON("$db" << request.dbname.toString_forTest())),
            IDLParserContext("ReshardingOplogFetcherTest"));
        ASSERT_EQ(parsedRequest.getCommandParameter(), expectedCursorId);
    }

    const std::vector<ShardId> kTwoShardIdList{{"s1"}, {"s2"}};
    const BSONObj kShardKey = BSON("skey" << 1);

    OperationContext* _opCtx;
    ServiceContext* _svcCtx;

    // To be reset per test case.
    UUID _reshardingUUID = UUID::gen();
    std::unique_ptr<ReshardingMetrics> _metrics;

    Timestamp _fetchTimestamp;
    int32_t _clusterTimestampInc = 0;

    ShardId _donorShard;
    ShardId _destinationShard;

private:
    // Set the sleep to 0 to speed up the tests.
    RAIIServerParameterControllerForTest _sleepMillisBeforeCriticalSection{
        "reshardingOplogFetcherSleepMillisBeforeCriticalSection", 0};
    RAIIServerParameterControllerForTest _sleepMillisDuringCriticalSection{
        "reshardingOplogFetcherSleepMillisDuringCriticalSection", 0};

    static HostAndPort makeHostAndPort(const ShardId& shardId) {
        return HostAndPort(str::stream() << shardId << ":123");
    }
};

TEST_F(ReshardingOplogFetcherTest, TestBasicSingleApplyOps) {
    for (bool storeProgress : {false, true}) {
        LOGV2(9480001, "Running case", "storeProgress"_attr = storeProgress);

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(storeProgress));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(storeProgress));

        auto numInsertOplogEntries = 5;
        auto initialAggregateBatchSize = 2;
        // Add 1 to account for the sentinel final noop oplog entry.
        auto numFetchedOplogEntries = numInsertOplogEntries + 1;
        // The oplog entries come in 2 separate aggregate batches, each requires an applyOps oplog
        // entry.
        auto numApplyOpsOplogEntries = 2;

        setupBasic(
            outputCollectionNss, dataCollectionNss, _destinationShard, numInsertOplogEntries);
        testFetcherBasic(outputCollectionNss,
                         dataCollectionNss,
                         storeProgress,
                         initialAggregateBatchSize,
                         numFetchedOplogEntries,
                         numApplyOpsOplogEntries);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherTest, TestBasicMultipleApplyOps_BatchLimitOperations) {
    for (bool storeProgress : {false, true}) {
        LOGV2(9480002, "Running case", "storeProgress"_attr = storeProgress);

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(storeProgress));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(storeProgress));

        auto batchLimitOperations = 5;
        RAIIServerParameterControllerForTest featureFlagController(
            "reshardingOplogFetcherInsertBatchLimitOperations", batchLimitOperations);
        auto numInsertOplogEntries = 8;
        auto initialAggregateBatchSize = boost::none;
        // Add 1 to account for the sentinel final noop oplog entry.
        auto numFetchedOplogEntries = numInsertOplogEntries + 1;
        // The oplog entries come in one aggregate batch. However, each applyOps oplog entry can
        // only have 'batchLimitOperations' oplog entries.
        auto numApplyOpsOplogEntries =
            std::ceil((double)numFetchedOplogEntries / batchLimitOperations);

        setupBasic(
            outputCollectionNss, dataCollectionNss, _destinationShard, numInsertOplogEntries);
        testFetcherBasic(outputCollectionNss,
                         dataCollectionNss,
                         storeProgress,
                         initialAggregateBatchSize,
                         numFetchedOplogEntries,
                         numApplyOpsOplogEntries);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherTest, TestBasicMultipleApplyOps_BatchLimitBytes) {
    for (bool storeProgress : {false, true}) {
        LOGV2(9480003, "Running case", "storeProgress"_attr = storeProgress);

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(storeProgress));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(storeProgress));

        auto batchLimitBytes = 10 * 1024;
        RAIIServerParameterControllerForTest featureFlagController(
            "reshardingOplogFetcherInsertBatchLimitBytes", batchLimitBytes);
        auto numInsertOplogEntries = 8;
        auto approxInsertOplogEntrySizeBytes = 3 * 1024;
        auto initialAggregateBatchSize = boost::none;
        // Add 1 to account for the sentinel final noop oplog entry.
        auto numFetchedOplogEntries = numInsertOplogEntries + 1;
        // The oplog entries come in one aggregate batch. However, each applyOps oplog entry can
        // only have 'batchLimitBytes'.
        auto numApplyOpsOplogEntries = std::ceil((double)approxInsertOplogEntrySizeBytes *
                                                 numInsertOplogEntries / batchLimitBytes);

        setupBasic(outputCollectionNss,
                   dataCollectionNss,
                   _destinationShard,
                   numInsertOplogEntries,
                   approxInsertOplogEntrySizeBytes);
        testFetcherBasic(outputCollectionNss,
                         dataCollectionNss,
                         storeProgress,
                         initialAggregateBatchSize,
                         numFetchedOplogEntries,
                         numApplyOpsOplogEntries);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherTest,
       TestBasicMultipleApplyOps_SingleOplogEntrySizeExceedsBatchLimitBytes) {
    for (bool storeProgress : {false, true}) {
        LOGV2(9480004, "Running case", "storeProgress"_attr = storeProgress);

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(storeProgress));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(storeProgress));

        auto batchLimitBytes = 1 * 1024;
        RAIIServerParameterControllerForTest featureFlagController(
            "reshardingOplogFetcherInsertBatchLimitBytes", batchLimitBytes);
        auto numInsertOplogEntries = 2;
        auto approxInsertOplogEntrySizeBytes = 3 * 1024;
        auto initialAggregateBatchSize = boost::none;
        // Add 1 to account for the sentinel final noop oplog entry.
        auto numFetchedOplogEntries = numInsertOplogEntries + 1;
        // The oplog entries come in one aggregate batch. However, the size of each insert oplog
        // entry exceeds the 'batchLimitBytes'. They should still get inserted successfully but each
        // should require a separate applyOps oplog entry.
        auto numApplyOpsOplogEntries = numFetchedOplogEntries;

        setupBasic(outputCollectionNss,
                   dataCollectionNss,
                   _destinationShard,
                   numInsertOplogEntries,
                   approxInsertOplogEntrySizeBytes);
        testFetcherBasic(outputCollectionNss,
                         dataCollectionNss,
                         storeProgress,
                         initialAggregateBatchSize,
                         numFetchedOplogEntries,
                         numApplyOpsOplogEntries);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherTest, TestBasicMultipleApplyOps_FinalOplogEntry) {
    for (bool storeProgress : {false, true}) {
        LOGV2(9678001, "Running case", "storeProgress"_attr = storeProgress);

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(storeProgress));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(storeProgress));

        auto batchLimitOperations = 5;
        RAIIServerParameterControllerForTest featureFlagController(
            "reshardingOplogFetcherInsertBatchLimitOperations", batchLimitOperations);
        auto numInsertOplogEntriesBeforeFinal = 8;
        auto approxInsertOplogEntrySizeBytes = 1;
        auto numNoopOplogEntriesAfterFinal = 3;
        auto initialAggregateBatchSize = boost::none;
        // Add 1 to account for the sentinel final noop oplog entry. The oplog entries after the
        // final oplog entries should be discarded.
        auto numFetchedOplogEntries = numInsertOplogEntriesBeforeFinal + 1;
        // The oplog entries come in one aggregate batch. However, each applyOps oplog entry can
        // only have 'batchLimitOperations' oplog entries.
        auto numApplyOpsOplogEntries =
            std::ceil((double)numFetchedOplogEntries / batchLimitOperations);

        setupBasic(outputCollectionNss,
                   dataCollectionNss,
                   _destinationShard,
                   numInsertOplogEntriesBeforeFinal,
                   approxInsertOplogEntrySizeBytes,
                   numNoopOplogEntriesAfterFinal);
        testFetcherBasic(outputCollectionNss,
                         dataCollectionNss,
                         storeProgress,
                         initialAggregateBatchSize,
                         numFetchedOplogEntries,
                         numApplyOpsOplogEntries);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherTest, TestTrackLastSeen) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    setupBasic(outputCollectionNss, dataCollectionNss, _destinationShard);

    const auto dataColl = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest{dataCollectionNss,
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(_opCtx),
                                     AcquisitionPrerequisites::kWrite},
        MODE_IX);

    const int maxBatches = 1;
    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RefetcherRunner", _svcCtx->getService(), Client::noSession());

        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       dataColl.uuid(),
                                       {_fetchTimestamp, _fetchTimestamp},
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       true /* storeProgress */);
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);
        fetcher.setMaxBatchesForTest(maxBatches);

        auto factory = makeCancelableOpCtx();
        fetcher.iterate(&cc(), factory);
        return fetcher.getLastSeenTimestamp();
    });

    ReshardingDonorOplogId lastSeen = requestPassthroughHandler(fetcherJob, maxBatches);

    ASSERT_EQ(2, itcount(outputCollectionNss));
    ASSERT_EQ(2, currentOpFetchedCount()) << " Verify reported metrics";
    ASSERT_EQ(2, persistedFetchedCount(_opCtx)) << " Verify persisted metrics";
    // Assert the lastSeen value has been bumped from the original `_fetchTimestamp`.
    ASSERT_GT(lastSeen.getTs(), _fetchTimestamp);
    assertUsedApplyOpsToBatchInsert(outputCollectionNss, 1 /* numApplyOpsOplogEntries */);
}

TEST_F(ReshardingOplogFetcherTest, TestFallingOffOplog) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    setupBasic(outputCollectionNss, dataCollectionNss, _destinationShard);

    const auto dataColl = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest{dataCollectionNss,
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(_opCtx),
                                     AcquisitionPrerequisites::kWrite},
        MODE_IX);

    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RefetcherRunner", _svcCtx->getService(), Client::noSession());

        const Timestamp doesNotExist(1, 1);
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       dataColl.uuid(),
                                       {doesNotExist, doesNotExist},
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       true /* storeProgress*/);
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

    ASSERT_EQ(0, itcount(outputCollectionNss));
    ASSERT_EQ(ErrorCodes::OplogQueryMinTsMissing, fetcherStatus->code());
    ASSERT_EQ(0, currentOpFetchedCount()) << " Verify currentOp metrics";
    assertUsedApplyOpsToBatchInsert(outputCollectionNss, 0 /* numApplyOpsOplogEntries */);
}

TEST_F(ReshardingOplogFetcherTest, TestAwaitInsert) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        const auto dataColl = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest{dataCollectionNss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(_opCtx),
                                         AcquisitionPrerequisites::kWrite},
            MODE_IX);
        return dataColl.uuid();
    }();

    ReshardingDonorOplogId startAt{_fetchTimestamp, _fetchTimestamp};
    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   startAt,
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);

    // The ReshardingOplogFetcher hasn't inserted a record yet so awaitInsert(startAt) won't be
    // immediately ready.
    auto hasSeenStartAtFuture = fetcher.awaitInsert(startAt);
    ASSERT_FALSE(hasSeenStartAtFuture.isReady());

    // Because no writes have happened to the data collection, the `hasSeenStartAtFuture` will still
    // not be ready.
    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);
        auto factory = makeCancelableOpCtx();
        return fetcher.iterate(&cc(), factory);
    });

    ASSERT_TRUE(requestPassthroughHandler(fetcherJob));
    ASSERT_FALSE(hasSeenStartAtFuture.isReady());

    // Insert a document into the data collection and have it generate an oplog entry with a
    // "destinedRecipient" field.
    auto dataWriteTimestamp = [&] {
        FailPointEnableBlock fp("addDestinedRecipient",
                                BSON("destinedRecipient" << _destinationShard.toString()));

        {
            const auto dataColl =
                acquireCollection(_opCtx,
                                  CollectionAcquisitionRequest{
                                      dataCollectionNss,
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(_opCtx),
                                      AcquisitionPrerequisites::kWrite},
                                  MODE_IX);
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(dataColl.getCollectionPtr(),
                           InsertStatement(BSON("_id" << 1 << "a" << 1)));
            wuow.commit();
        }

        repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);
        return repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);
    }();

    fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());
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
    for (bool storeProgress : {false, true}) {
        LOGV2(9480006, "Running case", "storeProgress"_attr = storeProgress);

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(storeProgress));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(storeProgress));
        const NamespaceString otherCollection = NamespaceString::createNamespaceString_forTest(
            "dbtests.collectionNotBeingResharded" + std::to_string(storeProgress));

        create(outputCollectionNss);
        create(dataCollectionNss);
        create(otherCollection);
        _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

        const auto& collectionUUID = [&] {
            const auto dataColl =
                acquireCollection(_opCtx,
                                  CollectionAcquisitionRequest{
                                      dataCollectionNss,
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(_opCtx),
                                      AcquisitionPrerequisites::kWrite},
                                  MODE_IX);
            return dataColl.uuid();
        }();

        ReshardingDonorOplogId startAt{_fetchTimestamp, _fetchTimestamp};
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       collectionUUID,
                                       startAt,
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       storeProgress);

        // Insert a document into the data collection and have it generate an oplog entry with a
        // "destinedRecipient" field.
        auto writeToDataCollectionTs = [&] {
            FailPointEnableBlock fp("addDestinedRecipient",
                                    BSON("destinedRecipient" << _destinationShard.toString()));

            {
                const auto dataColl =
                    acquireCollection(_opCtx,
                                      CollectionAcquisitionRequest{
                                          dataCollectionNss,
                                          PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                          repl::ReadConcernArgs::get(_opCtx),
                                          AcquisitionPrerequisites::kWrite},
                                      MODE_IX);
                WriteUnitOfWork wuow(_opCtx);
                insertDocument(dataColl.getCollectionPtr(),
                               InsertStatement(BSON("_id" << 1 << "a" << 1)));
                wuow.commit();
            }

            repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);
            return repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);
        }();

        auto fetcherJob = launchAsync([&, this] {
            ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());
            fetcher.useReadConcernForTest(false);
            fetcher.setInitialBatchSizeForTest(2);
            auto factory = makeCancelableOpCtx();
            return fetcher.iterate(&cc(), factory);
        });
        ASSERT_TRUE(requestPassthroughHandler(fetcherJob));

        // The fetcher's lastSeenTimestamp should be equal to `writeToDataCollectionTs`.
        ASSERT_TRUE(fetcher.getLastSeenTimestamp().getClusterTime() == writeToDataCollectionTs);
        ASSERT_TRUE(fetcher.getLastSeenTimestamp().getTs() == writeToDataCollectionTs);
        ASSERT_EQ(1, currentOpFetchedCount()) << " Verify currentOp metrics";
        ASSERT_EQ(storeProgress ? 1 : 0, persistedFetchedCount(_opCtx))
            << " Verify persisted metrics";
        assertUsedApplyOpsToBatchInsert(outputCollectionNss, 1 /* numApplyOpsOplogEntries */);

        // Now, insert a document into a different collection that is not involved in resharding.
        auto writeToOtherCollectionTs = [&] {
            {
                const auto dataColl =
                    acquireCollection(_opCtx,
                                      CollectionAcquisitionRequest{
                                          otherCollection,
                                          PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                          repl::ReadConcernArgs::get(_opCtx),
                                          AcquisitionPrerequisites::kWrite},
                                      MODE_IX);
                WriteUnitOfWork wuow(_opCtx);
                insertDocument(dataColl.getCollectionPtr(),
                               InsertStatement(BSON("_id" << 1 << "a" << 1)));
                wuow.commit();
            }

            repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);
            return repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);
        }();

        fetcherJob = launchAsync([&, this] {
            ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());
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
        ASSERT_EQ(2, currentOpFetchedCount()) << " Verify currentOp metrics";
        ASSERT_EQ(storeProgress ? 2 : 0, persistedFetchedCount(_opCtx))
            << " Verify persisted metrics";
        assertUsedApplyOpsToBatchInsert(outputCollectionNss, 2 /* numApplyOpsOplogEntries */);

        // The last document returned by ReshardingDonorOplogIterator::getNextBatch() would be
        // `writeToDataCollectionTs`, but ReshardingOplogFetcher would have inserted a doc with
        // `writeToOtherCollectionTs` after this so `awaitInsert` should be immediately ready when
        // passed `writeToDataCollectionTs`.
        ASSERT_TRUE(
            fetcher.awaitInsert({writeToDataCollectionTs, writeToDataCollectionTs}).isReady());

        // `awaitInsert` should not be ready if passed `writeToOtherCollectionTs`.
        ASSERT_FALSE(
            fetcher.awaitInsert({writeToOtherCollectionTs, writeToOtherCollectionTs}).isReady());

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherTest, RetriesOnRemoteInterruptionError) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        const auto dataColl = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest{dataCollectionNss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(_opCtx),
                                         AcquisitionPrerequisites::kWrite},
            MODE_IX);
        return dataColl.uuid();
    }();

    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());

        ReshardingDonorOplogId startAt{_fetchTimestamp, _fetchTimestamp};
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       collectionUUID,
                                       startAt,
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       true /* storeProgress */);
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

TEST_F(ReshardingOplogFetcherTest, RetriesOnNetworkTimeoutError) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        const auto dataColl = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest{dataCollectionNss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(_opCtx),
                                         AcquisitionPrerequisites::kWrite},
            MODE_IX);
        return dataColl.uuid();
    }();

    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());

        ReshardingDonorOplogId startAt{_fetchTimestamp, _fetchTimestamp};
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       collectionUUID,
                                       startAt,
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       true /* storeProgress */);

        auto factory = makeCancelableOpCtx();
        return fetcher.iterate(&cc(), factory);
    });

    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // Inject network timeout error.
        return {ErrorCodes::NetworkInterfaceExceededTimeLimit, "exceeded network time limit"};
    });

    auto moreToCome = fetcherJob.timed_get(Seconds(5));
    ASSERT_TRUE(moreToCome);
}

TEST_F(ReshardingOplogFetcherTest, ImmediatelyDoneWhenFinalOpHasAlreadyBeenFetched) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        const auto dataColl = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest{dataCollectionNss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(_opCtx),
                                         AcquisitionPrerequisites::kWrite},
            MODE_IX);
        return dataColl.uuid();
    }();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   ReshardingOplogFetcher::kFinalOpAlreadyFetched,
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);

    auto future = fetcher.schedule(nullptr, CancellationToken::uncancelable());

    ASSERT_TRUE(future.isReady());
    ASSERT_OK(future.getNoThrow());
}

DEATH_TEST_REGEX_F(ReshardingOplogFetcherTest,
                   CannotFetchMoreWhenFinalOpHasAlreadyBeenFetched,
                   "Invariant failure.*_startAt != kFinalOpAlreadyFetched") {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        const auto dataColl = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest{dataCollectionNss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(_opCtx),
                                         AcquisitionPrerequisites::kWrite},
            MODE_IX);
        return dataColl.uuid();
    }();

    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());

        // We intentionally do not call fetcher.useReadConcernForTest(false) for this test case.
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       collectionUUID,
                                       ReshardingOplogFetcher::kFinalOpAlreadyFetched,
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       true /* storeProgress */);
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

TEST_F(ReshardingOplogFetcherTest, ReadPreferenceBeforeAfterCriticalSection_TargetPrimary) {
    // Not set the reshardingOplogFetcherTargetPrimaryDuringCriticalSection to test that the
    // default is true.
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   {_fetchTimestamp, _fetchTimestamp},
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);
    auto executor = makeExecutor();
    executor->startup();
    auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

    // Make the cursor for the the aggregate command below have a non-zero id to test that
    // prepareForCriticalSection() interrupts the in-progress aggregation. So the fetcher should not
    // schedule a getMore command after this.
    auto cursorIdBeforePrepare = 123;
    auto aggBeforePrepareFuture = launchAsync([&, this] {
        onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
            auto expectedReadPref = ReadPreferenceSetting{
                ReadPreference::Nearest, ReadPreferenceSetting::kMinimalMaxStalenessValue};
            assertAggregateReadPreference(request, expectedReadPref);

            fetcher.prepareForCriticalSection();

            auto postBatchResumeToken = _fetchTimestamp + 1;
            return makeMockAggregateResponse(
                postBatchResumeToken, {} /* oplogEntries */, cursorIdBeforePrepare);
        });
    });

    aggBeforePrepareFuture.default_timed_get();

    // Depending on when the interrupt occurs, the fetcher may still try to kill the cursor after
    // the cancellation. In that case, schedule a response for the killCursor command.
    auto makeKillCursorResponse = [&](const executor::RemoteCommandRequest& request) {
        auto parsedRequest = KillCursorsCommandRequest::parse(
            request.cmdObj.addFields(BSON("$db" << request.dbname.toString_forTest())),
            IDLParserContext(unittest::getTestName()));

        ASSERT_EQ(parsedRequest.getNamespace().ns_forTest(),
                  NamespaceString::kRsOplogNamespace.toString_forTest());
        ASSERT_EQ(parsedRequest.getCursorIds().size(), 1U);
        ASSERT_EQ(parsedRequest.getCursorIds()[0], cursorIdBeforePrepare);
        return BSONObj{};
    };

    // Make the cursor for the the aggregate command below have a non-zero id to test that the
    // fetcher does not schedule a getMore command after seeing the final oplog entry.
    auto cursorIdAfterPrepare = 456;
    auto makeAggResponse = [&](const executor::RemoteCommandRequest& request) {
        auto expectedReadPref = ReadPreferenceSetting{ReadPreference::PrimaryOnly};
        assertAggregateReadPreference(request, expectedReadPref);

        auto postBatchResumeToken = _fetchTimestamp + 2;
        auto oplogEntries = BSON_ARRAY(
            makeFinalNoopOplogEntry(dataCollectionNss, collectionUUID, postBatchResumeToken));
        return makeMockAggregateResponse(postBatchResumeToken, oplogEntries, cursorIdAfterPrepare);
    };

    bool scheduledAggResponse = false;
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto cmdName = request.cmdObj.firstElementFieldName();
        if (cmdName == "killCursors"_sd) {
            return makeKillCursorResponse(request);
        } else if (cmdName == "aggregate"_sd) {
            scheduledAggResponse = true;
            return makeAggResponse(request);
        }
        return {ErrorCodes::InternalError,
                str::stream() << "Unexpected command request " << request.toString()};
    });
    if (!scheduledAggResponse) {
        onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
            return makeAggResponse(request);
        });
    }

    ASSERT_OK(fetcherFuture.getNoThrow());
    executor->shutdown();
    executor->join();
}

TEST_F(ReshardingOplogFetcherTest, ReadPreferenceBeforeAfterCriticalSection_NotTargetPrimary) {
    RAIIServerParameterControllerForTest targetPrimaryDuringCriticalSection{
        "reshardingOplogFetcherTargetPrimaryDuringCriticalSection", false};

    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   {_fetchTimestamp, _fetchTimestamp},
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);
    auto executor = makeExecutor();
    executor->startup();
    auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

    // Make the cursor for the the aggregate command below have a non-zero id to test that
    // prepareForCriticalSection() does not interrupt the in-progress aggregation. The fetcher
    // should schedule a getMore command after this.
    auto cursorIdBeforePrepare = 123;
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto expectedReadPref = ReadPreferenceSetting{
            ReadPreference::Nearest, ReadPreferenceSetting::kMinimalMaxStalenessValue};
        assertAggregateReadPreference(request, expectedReadPref);

        fetcher.prepareForCriticalSection();

        auto postBatchResumeToken = _fetchTimestamp + 1;
        return makeMockAggregateResponse(
            postBatchResumeToken, {} /* oplogEntries */, cursorIdBeforePrepare);
    });

    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        assertGetMoreCursorId(request, cursorIdBeforePrepare);
        auto postBatchResumeToken = _fetchTimestamp + 2;
        return makeMockAggregateResponse(
            postBatchResumeToken, {} /* oplogEntries */, cursorIdBeforePrepare);
    });

    // The fetcher should kill the cursor after exhausting it.
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto parsedRequest = KillCursorsCommandRequest::parse(
            request.cmdObj.addFields(BSON("$db" << request.dbname.toString_forTest())),
            IDLParserContext(unittest::getTestName()));

        ASSERT_EQ(parsedRequest.getNamespace().ns_forTest(),
                  NamespaceString::kRsOplogNamespace.toString_forTest());
        ASSERT_EQ(parsedRequest.getCursorIds().size(), 1U);
        ASSERT_EQ(parsedRequest.getCursorIds()[0], cursorIdBeforePrepare);
        return BSONObj{};
    });

    // Make the cursor for the the aggregate command below have a non-zero id to test that the
    // fetcher does not schedule a getMore command after seeing the final oplog entry.
    auto cursorIdAfterPrepare = 456;
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto expectedReadPref = ReadPreferenceSetting{
            ReadPreference::Nearest, ReadPreferenceSetting::kMinimalMaxStalenessValue};
        assertAggregateReadPreference(request, expectedReadPref);

        auto postBatchResumeToken = _fetchTimestamp + 2;
        auto oplogEntries = BSON_ARRAY(
            makeFinalNoopOplogEntry(dataCollectionNss, collectionUUID, postBatchResumeToken));
        return makeMockAggregateResponse(postBatchResumeToken, oplogEntries, cursorIdAfterPrepare);
    });

    ASSERT_OK(fetcherFuture.getNoThrow());
    executor->shutdown();
    executor->join();
}

TEST_F(ReshardingOplogFetcherTest, PrepareForCriticalSectionBeforeScheduling) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   {_fetchTimestamp, _fetchTimestamp},
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);
    fetcher.prepareForCriticalSection();

    auto executor = makeExecutor();
    executor->startup();
    auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

    // Make the cursor for the the aggregate command below have a non-zero id to test that the
    // fetcher does not schedule a getMore command after seeing the final oplog entry.
    auto cursorIdAfterPrepare = 123;
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto expectedReadPref = ReadPreferenceSetting{ReadPreference::PrimaryOnly};
        assertAggregateReadPreference(request, expectedReadPref);

        auto postBatchResumeToken = _fetchTimestamp + 1;
        auto oplogEntries = BSON_ARRAY(
            makeFinalNoopOplogEntry(dataCollectionNss, collectionUUID, postBatchResumeToken));
        return makeMockAggregateResponse(postBatchResumeToken, oplogEntries, cursorIdAfterPrepare);
    });

    ASSERT_OK(fetcherFuture.getNoThrow());
    executor->shutdown();
    executor->join();
}

TEST_F(ReshardingOplogFetcherTest, PrepareForCriticalSectionMoreThanOnce) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   {_fetchTimestamp, _fetchTimestamp},
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);
    auto executor = makeExecutor();
    executor->startup();
    auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

    // Make the cursor for the the aggregate command below have id 0 to make the fetcher not
    // schedule a getMore command so that the test does not need to also schedule a getMore
    // response.
    auto cursorIdBeforePrepare = 0;
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto expectedReadPref = ReadPreferenceSetting{
            ReadPreference::Nearest, ReadPreferenceSetting::kMinimalMaxStalenessValue};
        assertAggregateReadPreference(request, expectedReadPref);

        // This should interrupt the in-progress aggregation.
        fetcher.prepareForCriticalSection();
        // This should not interrupt the in-progress aggregation since it has already been
        // interrupted.
        fetcher.prepareForCriticalSection();

        auto postBatchResumeToken = _fetchTimestamp + 1;
        return makeMockAggregateResponse(
            postBatchResumeToken, {} /* oplogEntries */, cursorIdBeforePrepare);
    });

    // Make the cursor for the the aggregate command below have a non-zero id to test that the
    // fetcher does not schedule a getMore command after seeing the final oplog entry.
    auto cursorIdAfterPrepare = 123;
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto expectedReadPref = ReadPreferenceSetting{ReadPreference::PrimaryOnly};
        assertAggregateReadPreference(request, expectedReadPref);

        // This should not interrupt the in-progress aggregation.
        fetcher.prepareForCriticalSection();

        auto postBatchResumeToken = _fetchTimestamp + 2;
        auto oplogEntries = BSON_ARRAY(
            makeFinalNoopOplogEntry(dataCollectionNss, collectionUUID, postBatchResumeToken));
        return makeMockAggregateResponse(postBatchResumeToken, oplogEntries, cursorIdAfterPrepare);
    });

    ASSERT_OK(fetcherFuture.getNoThrow());
    fetcher.prepareForCriticalSection();
    executor->shutdown();
    executor->join();
}

TEST_F(ReshardingOplogFetcherTest, PrepareForCriticalSectionAfterFetchingFinalOplogEntry) {
    for (bool targetPrimary : {false, true}) {
        LOGV2(10355403, "Running case", "targetPrimary"_attr = targetPrimary);

        RAIIServerParameterControllerForTest targetPrimaryDuringCriticalSection{
            "reshardingOplogFetcherTargetPrimaryDuringCriticalSection", targetPrimary};

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(targetPrimary));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(targetPrimary));

        create(outputCollectionNss);
        create(dataCollectionNss);

        const auto& collectionUUID = [&] {
            AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
            return dataColl->uuid();
        }();

        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       collectionUUID,
                                       {_fetchTimestamp, _fetchTimestamp},
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       true /* storeProgress */);
        auto executor = makeExecutor();
        executor->startup();

        // Invoke onPrepareCriticalSection() after the fetcher has consumed the final oplog entry.
        auto fp = globalFailPointRegistry().find("pauseReshardingOplogFetcherAfterConsuming");
        auto timesEnteredBefore = fp->setMode(FailPoint::alwaysOn);

        auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

        auto cursorId = 123;
        auto aggBeforePrepareFuture = launchAsync([&, this] {
            onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
                auto expectedReadPref = ReadPreferenceSetting{
                    ReadPreference::Nearest, ReadPreferenceSetting::kMinimalMaxStalenessValue};
                assertAggregateReadPreference(request, expectedReadPref);

                auto postBatchResumeToken = _fetchTimestamp + 1;
                auto oplogEntries = BSON_ARRAY(makeFinalNoopOplogEntry(
                    dataCollectionNss, collectionUUID, postBatchResumeToken));
                return makeMockAggregateResponse(postBatchResumeToken, oplogEntries, cursorId);
            });
        });

        fp->waitForTimesEntered(timesEnteredBefore + 1);

        fetcher.prepareForCriticalSection();
        auto timesEnteredAfter = fp->setMode(FailPoint::off);
        ASSERT_EQ(timesEnteredAfter, timesEnteredBefore + 1);

        // The thread below would block without the time advancing.
        advanceTime(Seconds{1});
        aggBeforePrepareFuture.default_timed_get();

        // Schedule a response for the killCursor command to prevent its request from interfering
        // with the next test case.
        onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
            auto parsedRequest = KillCursorsCommandRequest::parse(
                request.cmdObj.addFields(BSON("$db" << request.dbname.toString_forTest())),
                IDLParserContext(unittest::getTestName()));

            ASSERT_EQ(parsedRequest.getNamespace().ns_forTest(),
                      NamespaceString::kRsOplogNamespace.toString_forTest());
            ASSERT_EQ(parsedRequest.getCursorIds().size(), 1U);
            ASSERT_EQ(parsedRequest.getCursorIds()[0], cursorId);
            return BSONObj{};
        });

        // The fetcher should not schedule another aggregate command. If it does, it would get stuck
        // waiting for the aggregate response which the test does not schedule.
        ASSERT_OK(fetcherFuture.getNoThrow());
        executor->shutdown();
        executor->join();

        resetResharding();
    }
}

// TODO (SERVER-106341): Uncomment the assertions in all UpdateAverageTime* unit tests.

TEST_F(ReshardingOplogFetcherTest, UpdateAverageTimeToFetchCursorAdvancedBasic) {
    auto smoothingFactor = 0.5;
    const RAIIServerParameterControllerForTest smoothingFactorServerParameter{
        "reshardingExponentialMovingAverageTimeToFetchAndApplySmoothingFactor", smoothingFactor};

    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    // To make the time difference calculation in this test work as expected, advance the clock to
    // the next second since a clusterTime timestamp only has second granularity, as the "t" field
    // is the number of seconds since epoch and the "i" field is just an increasing increment for
    // differentiating operations within the same second.
    advanceTimeToNextSecond();

    advanceTime(Seconds{60});
    _fetchTimestamp = makeClusterTimestampAtNow();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   {_fetchTimestamp, _fetchTimestamp},
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);
    auto executor = makeExecutor();
    executor->startup();
    auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

    // Verify that the average started out uninitialized.
    ASSERT_FALSE(_metrics->getAverageTimeToFetchOplogEntries(_donorShard));

    auto cursorId = 123;
    advanceTime(Seconds{10});
    auto postBatchResumeToken0 = makeClusterTimestampAtNow();

    // Advance the clock before mocking a response with the resume token above.
    auto timeToFetch0 = Milliseconds(5000);
    advanceTime(timeToFetch0);
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return makeMockAggregateResponse(postBatchResumeToken0, {} /* oplogEntries */, cursorId);
    });

    // Verify that the average got initialized based on the difference between the current timestamp
    // and the latest resume timestamp.
    // auto avgTimeToFetch0 = timeToFetch0;
    // ASSERT_EQ(_metrics->getAverageTimeToFetchOplogEntries(_donorShard), avgTimeToFetch0);

    advanceTime(Seconds{5});
    auto postBatchResumeToken1 = makeClusterTimestampAtNow();

    // Advance the clock before mocking a response with the resume token above.
    auto timeToFetch1 = Milliseconds(2000);
    advanceTime(timeToFetch1);
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return makeMockGetMoreResponse(postBatchResumeToken1, {} /* oplogEntries */, cursorId);
    });

    // Verify that the average got updated based on the difference between the current timestamp
    // and the latest resume timestamp.
    // auto avgTimeToFetch1 = Milliseconds((int)resharding::calculateExponentialMovingAverage(
    //    avgTimeToFetch0.count(), timeToFetch1.count(), smoothingFactor));
    // ASSERT_EQ(_metrics->getAverageTimeToFetchOplogEntries(_donorShard),
    //           Milliseconds(avgTimeToFetch1));

    advanceTime(Seconds{1});
    auto postBatchResumeToken2 = makeClusterTimestampAtNow();

    // Advance the clock before mocking a response with the resume token above.
    auto timeToFetch2 = Milliseconds(1000);
    advanceTime(timeToFetch2);
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto oplogEntries = BSON_ARRAY(
            makeFinalNoopOplogEntry(dataCollectionNss, collectionUUID, postBatchResumeToken2));
        return makeMockGetMoreResponse(postBatchResumeToken2, oplogEntries, cursorId);
    });

    ASSERT_OK(fetcherFuture.getNoThrow());
    executor->shutdown();
    executor->join();

    // Verify that the average did not get updated when the fetcher joined.
    // ASSERT_EQ(_metrics->getAverageTimeToFetchOplogEntries(_donorShard),
    //          Milliseconds(avgTimeToFetch1));
}

TEST_F(ReshardingOplogFetcherTest, UpdateAverageTimeToFetchAdvancedDelayLessThanOneSecond) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    // To make the time difference calculation in this test work as expected, advance the clock to
    // the next second since a clusterTime timestamp only has second granularity, as the "t" field
    // is the number of seconds since epoch and the "i" field is just an increasing increment for
    // differentiating operations within the same second.
    advanceTimeToNextSecond();

    advanceTime(Seconds{60});
    _fetchTimestamp = makeClusterTimestampAtNow();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   {_fetchTimestamp, _fetchTimestamp},
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);
    auto executor = makeExecutor();
    executor->startup();
    auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

    // Verify that the average started out uninitialized.
    ASSERT_FALSE(_metrics->getAverageTimeToFetchOplogEntries(_donorShard));

    auto cursorId = 123;
    advanceTime(Seconds{10});
    auto postBatchResumeToken0 = makeClusterTimestampAtNow();

    // Advance the clock by less than one second before mocking a response with the resume token
    // above.
    auto timeToFetch0 = Milliseconds(321);
    advanceTime(timeToFetch0);
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return makeMockAggregateResponse(postBatchResumeToken0, {} /* oplogEntries */, cursorId);
    });

    // Verify that the average got initialized based on the difference between the current timestamp
    // and the latest resume timestamp.
    // auto avgTimeToFetch0 = timeToFetch0;
    // ASSERT_EQ(_metrics->getAverageTimeToFetchOplogEntries(_donorShard), avgTimeToFetch0);

    // Mock a response with the final oplog entry so the fetcher can join.
    advanceTime(Seconds{1});
    auto postBatchResumeToken1 = makeClusterTimestampAtNow();
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto oplogEntries = BSON_ARRAY(
            makeFinalNoopOplogEntry(dataCollectionNss, collectionUUID, postBatchResumeToken1));
        return makeMockGetMoreResponse(postBatchResumeToken1, oplogEntries, cursorId);
    });

    ASSERT_OK(fetcherFuture.getNoThrow());
    executor->shutdown();
    executor->join();
}

TEST_F(ReshardingOplogFetcherTest, UpdateAverageTimeToFetchAdvancedDelayZeroSecond) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    // To make the time difference calculation in this test work as expected, advance the clock to
    // the next second since a clusterTime timestamp only has second granularity, as the "t" field
    // is the number of seconds since epoch and the "i" field is just an increasing increment for
    // differentiating operations within the same second.
    advanceTimeToNextSecond();

    advanceTime(Seconds{60});
    _fetchTimestamp = makeClusterTimestampAtNow();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   {_fetchTimestamp, _fetchTimestamp},
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);
    auto executor = makeExecutor();
    executor->startup();
    auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

    // Verify that the average started out uninitialized.
    ASSERT_FALSE(_metrics->getAverageTimeToFetchOplogEntries(_donorShard));

    auto cursorId = 123;
    advanceTime(Seconds{10});
    auto postBatchResumeToken0 = makeClusterTimestampAtNow();

    // Do not advance the clock before mocking a response with the resume token above.
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return makeMockAggregateResponse(postBatchResumeToken0, {} /* oplogEntries */, cursorId);
    });

    // Verify that the average got initialized based on the difference between the current timestamp
    // and the latest resume timestamp which is 0.
    // auto avgTimeToFetch0 = Milliseconds(0);
    // ASSERT_EQ(_metrics->getAverageTimeToFetchOplogEntries(_donorShard), avgTimeToFetch0);

    // Mock a response with the final oplog entry so the fetcher can join.
    advanceTime(Seconds{1});
    auto postBatchResumeToken1 = makeClusterTimestampAtNow();
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto oplogEntries = BSON_ARRAY(
            makeFinalNoopOplogEntry(dataCollectionNss, collectionUUID, postBatchResumeToken1));
        return makeMockGetMoreResponse(postBatchResumeToken1, oplogEntries, cursorId);
    });

    ASSERT_OK(fetcherFuture.getNoThrow());
    executor->shutdown();
    executor->join();
}

TEST_F(ReshardingOplogFetcherTest, UpdateAverageTimeToFetchAdvancedDelayNegativeClockSkew) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    // To make the time difference calculation in this test work as expected, advance the clock to
    // the next second since a clusterTime timestamp only has second granularity, as the "t" field
    // is the number of seconds since epoch and the "i" field is just an increasing increment for
    // differentiating operations within the same second.
    advanceTimeToNextSecond();

    advanceTime(Seconds{60});
    _fetchTimestamp = makeClusterTimestampAtNow();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   {_fetchTimestamp, _fetchTimestamp},
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);
    auto executor = makeExecutor();
    executor->startup();
    auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

    // Verify that the average started out uninitialized.
    ASSERT_FALSE(_metrics->getAverageTimeToFetchOplogEntries(_donorShard));

    auto cursorId = 123;
    advanceTime(Seconds{10});
    // Make the resume timestamp greater than the current time on the recipient.
    auto postBatchResumeToken0 = makeClusterTimestampAt(now() + Seconds(5));

    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return makeMockAggregateResponse(postBatchResumeToken0, {} /* oplogEntries */, cursorId);
    });

    // Verify that the average got initialized based on the difference between the current timestamp
    // and the latest resume timestamp. The difference was negative but got capped at 0.
    // auto avgTimeToFetch0 = Milliseconds(0);
    // ASSERT_EQ(_metrics->getAverageTimeToFetchOplogEntries(_donorShard), avgTimeToFetch0);

    // Mock a response with the final oplog entry so the fetcher can join.
    advanceTime(Seconds{1});
    auto postBatchResumeToken1 = makeClusterTimestampAtNow();
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto oplogEntries = BSON_ARRAY(
            makeFinalNoopOplogEntry(dataCollectionNss, collectionUUID, postBatchResumeToken1));
        return makeMockGetMoreResponse(postBatchResumeToken1, oplogEntries, cursorId);
    });

    ASSERT_OK(fetcherFuture.getNoThrow());
    executor->shutdown();
    executor->join();
}

TEST_F(ReshardingOplogFetcherTest, UpdateAverageTimeToFetchCursorNotAdvanced) {
    auto smoothingFactor = 0.6;
    const RAIIServerParameterControllerForTest smoothingFactorServerParameter{
        "reshardingExponentialMovingAverageTimeToFetchAndApplySmoothingFactor", smoothingFactor};

    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    // To make the time difference calculation in this test work as expected, advance the clock to
    // the next second since a clusterTime timestamp only has second granularity, as the "t" field
    // is the number of seconds since epoch and the "i" field is just an increasing increment for
    // differentiating operations within the same second.
    advanceTimeToNextSecond();

    advanceTime(Seconds{60});
    _fetchTimestamp = makeClusterTimestampAtNow();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   {_fetchTimestamp, _fetchTimestamp},
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);
    auto executor = makeExecutor();
    executor->startup();
    auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

    // Verify that the average started out uninitialized.
    ASSERT_FALSE(_metrics->getAverageTimeToFetchOplogEntries(_donorShard));

    auto cursorId = 123;
    advanceTime(Seconds{10});
    auto postBatchResumeToken0 = makeClusterTimestampAtNow();

    // Advance the clock before mocking a response with the resume token above.
    auto timeToFetch0 = Milliseconds(5000);
    advanceTime(timeToFetch0);
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return makeMockAggregateResponse(postBatchResumeToken0, {} /* oplogEntries */, cursorId);
    });

    // Verify that the average got initialized based on the difference between the current timestamp
    // and the latest resume timestamp.
    // auto avgTimeToFetch0 = timeToFetch0;
    // ASSERT_EQ(_metrics->getAverageTimeToFetchOplogEntries(_donorShard), avgTimeToFetch0);

    // Make the cursor not advance.
    auto postBatchResumeToken1 = postBatchResumeToken0;
    auto getMoreDuration1 = Milliseconds(5);
    // auto timeToFetch1 = getMoreDuration1;
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // Advance the clock before mocking a response with the resume token above.
        advanceTime(getMoreDuration1);
        return makeMockGetMoreResponse(postBatchResumeToken1, {} /* oplogEntries */, cursorId);
    });

    // Verify that the average got updated based on the time taken for the getMore command to
    // return.
    // auto avgTimeToFetch1 = Milliseconds((int)resharding::calculateExponentialMovingAverage(
    //     avgTimeToFetch0.count(), timeToFetch1.count(), smoothingFactor));
    // ASSERT_EQ(_metrics->getAverageTimeToFetchOplogEntries(_donorShard),
    //           Milliseconds(avgTimeToFetch1));

    // Make the cursor not advance again.
    auto postBatchResumeToken2 = postBatchResumeToken0;
    auto getMoreDuration2 = Milliseconds(1);
    // auto timeToFetch2 = getMoreDuration2;
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // Advance the clock before mocking a response with the resume token above.
        advanceTime(getMoreDuration2);
        return makeMockGetMoreResponse(postBatchResumeToken2, {} /* oplogEntries */, cursorId);
    });

    // Verify that the average got updated based on the time taken for the getMore command to
    // return.
    // auto avgTimeToFetch2 = Milliseconds((int)resharding::calculateExponentialMovingAverage(
    //     avgTimeToFetch1.count(), timeToFetch2.count(), smoothingFactor));
    // ASSERT_EQ(_metrics->getAverageTimeToFetchOplogEntries(_donorShard),
    //           Milliseconds(avgTimeToFetch2));

    // Mock a response with the final oplog entry so the fetcher can join.
    advanceTime(Seconds{1});
    auto postBatchResumeToken3 = makeClusterTimestampAtNow();
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto oplogEntries = BSON_ARRAY(
            makeFinalNoopOplogEntry(dataCollectionNss, collectionUUID, postBatchResumeToken3));
        return makeMockGetMoreResponse(postBatchResumeToken3, oplogEntries, cursorId);
    });

    ASSERT_OK(fetcherFuture.getNoThrow());
    executor->shutdown();
    executor->join();
}

TEST_F(ReshardingOplogFetcherTest, UpdateAverageTimeToFetchMultipleCursors) {
    auto smoothingFactor = 0.7;
    const RAIIServerParameterControllerForTest smoothingFactorServerParameter{
        "reshardingExponentialMovingAverageTimeToFetchAndApplySmoothingFactor", smoothingFactor};

    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    // To make the time difference calculation in this test work as expected, advance the clock to
    // the next second since a clusterTime timestamp only has second granularity, as the "t" field
    // is the number of seconds since epoch and the "i" field is just an increasing increment for
    // differentiating operations within the same second.
    advanceTimeToNextSecond();

    advanceTime(Seconds{60});
    _fetchTimestamp = makeClusterTimestampAtNow();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   {_fetchTimestamp, _fetchTimestamp},
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);
    auto executor = makeExecutor();
    executor->startup();
    auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

    // Verify that the average started out uninitialized.
    ASSERT_FALSE(_metrics->getAverageTimeToFetchOplogEntries(_donorShard));

    auto cursorId0 = 0;
    advanceTime(Seconds{10});
    auto postBatchResumeToken0 = makeClusterTimestampAtNow();

    // Advance the clock before mocking a response with the resume token above.
    auto timeToFetch0 = Milliseconds(3000);
    advanceTime(timeToFetch0);
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return makeMockAggregateResponse(postBatchResumeToken0, {} /* oplogEntries */, cursorId0);
    });

    // Verify that the average got initialized based on the difference between the current timestamp
    // and the latest resume timestamp.
    // auto avgTimeToFetch0 = timeToFetch0;
    // ASSERT_EQ(_metrics->getAverageTimeToFetchOplogEntries(_donorShard), avgTimeToFetch0);

    auto cursorId1 = 123;
    advanceTime(Seconds{5});
    auto postBatchResumeToken1 = makeClusterTimestampAtNow();

    // Advance the clock before mocking a response with the resume token above.
    auto timeToFetch1 = Milliseconds(2000);
    advanceTime(timeToFetch1);
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return makeMockAggregateResponse(postBatchResumeToken1, {} /* oplogEntries */, cursorId1);
    });

    // Verify that the average got updated based on the difference between the current timestamp
    // and the latest resume timestamp.
    // auto avgTimeToFetch1 = Milliseconds((int)resharding::calculateExponentialMovingAverage(
    //     avgTimeToFetch0.count(), timeToFetch1.count(), smoothingFactor));
    // ASSERT_EQ(_metrics->getAverageTimeToFetchOplogEntries(_donorShard),
    //           Milliseconds(avgTimeToFetch1));

    // Mock a response with the final oplog entry so the fetcher can join.
    advanceTime(Seconds{1});
    auto postBatchResumeToken2 = makeClusterTimestampAtNow();
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto oplogEntries = BSON_ARRAY(
            makeFinalNoopOplogEntry(dataCollectionNss, collectionUUID, postBatchResumeToken2));
        return makeMockGetMoreResponse(postBatchResumeToken2, oplogEntries, cursorId1);
    });

    ASSERT_OK(fetcherFuture.getNoThrow());
    executor->shutdown();
    executor->join();
}

class ReshardingOplogFetcherProgressMarkOplogTest : public ReshardingOplogFetcherTest {
protected:
    struct OplogFetcherState {
        bool oplogApplicationStarted;
        bool storedProgress;
        int lastFetchedCount;
        ReshardingDonorOplogId lastFetchedOplogId;
        Date_t lastFetchedOplogWallClockTime;
        boost::optional<repl::MutableOplogEntry> lastFetchedOplogEntry;
    };

    struct TestOptions {
        bool storeProgress;
        bool movingAvgFeatureFlag;
        bool movingAvgServerParameter;

        BSONObj toBSON() const {
            BSONObjBuilder bob;
            bob.append("storeProgress", storeProgress);
            bob.append("movingAvgFeatureFlag", movingAvgFeatureFlag);
            bob.append("movingAvgServerParameter", movingAvgServerParameter);
            return bob.obj();
        }
    };

    struct TestContext {
        TestContext(std::unique_ptr<ReshardingOplogFetcher> fetcher_,
                    ReshardingDonorOplogId startAt_,
                    NamespaceString outputCollectionNss_,
                    NamespaceString dataCollectionNss_,
                    UUID dataCollectionUUID_,
                    bool movingAvgFeatureFlag_,
                    bool movingAvgServerParameter_)
            : fetcher(std::move(fetcher_)),
              startAt(startAt_),
              outputCollectionNss(outputCollectionNss_),
              dataCollectionNss(dataCollectionNss_),
              dataCollectionUUID(dataCollectionUUID_),
              movingAvgFeatureFlag("featureFlagReshardingRemainingTimeEstimateBasedOnMovingAverage",
                                   movingAvgFeatureFlag_),
              movingAvgServerParameter("reshardingRemainingTimeEstimateBasedOnMovingAverage",
                                       movingAvgServerParameter_) {}

        std::unique_ptr<ReshardingOplogFetcher> fetcher;
        ReshardingDonorOplogId startAt;
        NamespaceString outputCollectionNss;
        NamespaceString dataCollectionNss;
        UUID dataCollectionUUID;
        RAIIServerParameterControllerForTest movingAvgFeatureFlag;
        RAIIServerParameterControllerForTest movingAvgServerParameter;
    };

    std::vector<TestOptions> makeAllTestOptions() {
        std::vector<TestOptions> testOptions;
        for (bool storeProgress : {false, true}) {
            for (bool movingAvgFeatureFlag : {false, true}) {
                for (bool movingAvgServerParameter : {false, true}) {
                    testOptions.push_back(
                        {storeProgress, movingAvgFeatureFlag, movingAvgServerParameter});
                }
            }
        }
        return testOptions;
    }

    TestContext makeTestContext(int testNum, TestOptions testOptions) {
        auto outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(testNum));
        auto dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(testNum));
        create(outputCollectionNss);
        create(dataCollectionNss);

        auto dataCollectionUUID = [&] {
            AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
            return dataColl->uuid();
        }();

        auto startAt = ReshardingDonorOplogId{_fetchTimestamp, _fetchTimestamp};
        auto fetcher = std::make_unique<ReshardingOplogFetcher>(makeFetcherEnv(),
                                                                _reshardingUUID,
                                                                dataCollectionUUID,
                                                                startAt,
                                                                _donorShard,
                                                                _destinationShard,
                                                                outputCollectionNss,
                                                                testOptions.storeProgress);

        return {std::move(fetcher),
                startAt,
                outputCollectionNss,
                dataCollectionNss,
                dataCollectionUUID,
                testOptions.movingAvgFeatureFlag,
                testOptions.movingAvgServerParameter};
    }

    void runFetcher(ReshardingOplogFetcher* fetcher, const BSONObj& mockCursorResponse) {
        auto fetcherJob = launchAsync([&, this] {
            ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());
            auto factory = makeCancelableOpCtx();
            return fetcher->iterate(&cc(), factory);
        });
        ASSERT_TRUE(requestPassthroughHandler(fetcherJob, -1, mockCursorResponse));
    }

    void assertOplogEntriesCount(const NamespaceString& outputNss,
                                 const OplogFetcherState& expected) {
        ASSERT_EQ(currentOpFetchedCount(), expected.lastFetchedCount);
        ASSERT_EQ(persistedFetchedCount(_opCtx),
                  expected.storedProgress ? expected.lastFetchedCount : 0);
        assertUsedApplyOpsToBatchInsert(outputNss, expected.lastFetchedCount);
    }

    void assertNoOplogEntries(const NamespaceString& outputNss) {
        assertOplogEntriesCount(outputNss, OplogFetcherState{.lastFetchedCount = 0});
        ASSERT_TRUE(getLast(outputNss).isEmpty());
    }

    void assertLastOplogEntryProgressMark(const NamespaceString& outputNss,
                                          const OplogFetcherState& expected) {
        auto lastOplogEntryBson = getLast(outputNss);
        auto lastOplogEntry = uassertStatusOK(repl::OplogEntry::parse(lastOplogEntryBson));

        ASSERT_EQ(lastOplogEntry.getOpType(), repl::OpTypeEnum::kNoop);
        ASSERT_BSONOBJ_EQ(lastOplogEntry.get_id()->getDocument().toBson(),
                          expected.lastFetchedOplogId.toBSON());

        ASSERT_EQ(lastOplogEntry.getWallClockTime(), expected.lastFetchedOplogWallClockTime);
        ReshardProgressMarkO2Field o2Field;
        o2Field.setType(resharding::kReshardProgressMarkOpLogType);
        if (expected.oplogApplicationStarted) {
            o2Field.setCreatedAfterOplogApplicationStarted(true);
        }
        ASSERT_BSONOBJ_EQ(*lastOplogEntry.getObject2(), o2Field.toBSON());
    }

    void assertLastOplogNotProgressMark(const NamespaceString& outputNss,
                                        const OplogFetcherState& expected) {
        auto lastOplogEntryBson = getLast(outputNss);
        auto lastOplogEntry = uassertStatusOK(repl::OplogEntry::parse(lastOplogEntryBson));

        ASSERT_EQ(lastOplogEntry.getWallClockTime(), expected.lastFetchedOplogWallClockTime);

        // Exclude the 'wallClockTime' field from the BSON comparison, as it is expected to be set
        // to the current time by the fetcher.
        auto actualLastOplogEntryBson =
            lastOplogEntryBson.removeField(repl::OplogEntry::kWallClockTimeFieldName);
        auto expectedLastOplogEntryBson = expected.lastFetchedOplogEntry->toBSON().removeField(
            repl::OplogEntry::kWallClockTimeFieldName);
        ASSERT_BSONOBJ_EQ(actualLastOplogEntryBson, expectedLastOplogEntryBson);
    }
};

TEST_F(
    ReshardingOplogFetcherProgressMarkOplogTest,
    BeforeOplogApplication_NotInsertProgressMarkOplog_EmptyBatchAndResumeTimestampEqualToStartAt) {
    auto testNum = 0;
    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(10635001,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        testNum++;
        auto testCtx = makeTestContext(testNum, testOptions);

        auto postBatchResumeToken = testCtx.startAt.getTs();
        auto oplogEntries = BSONArrayBuilder().arr();
        auto mockCursorResponse = makeMockAggregateResponse(postBatchResumeToken, oplogEntries);
        runFetcher(testCtx.fetcher.get(), mockCursorResponse);

        assertNoOplogEntries(testCtx.outputCollectionNss);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherProgressMarkOplogTest,
       BeforeOplogApplication_InsertProgressMark_BatchEmptyAndResumeTimestampGreaterThanStartAt) {
    auto testNum = 0;
    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(10635002,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        testNum++;
        auto testCtx = makeTestContext(testNum, testOptions);

        auto postBatchResumeToken = testCtx.startAt.getTs() + 1;
        auto oplogEntries = BSONArrayBuilder().arr();
        auto mockCursorResponse = makeMockAggregateResponse(postBatchResumeToken, oplogEntries);
        // Advance the clock to verify that the fetcher sets the wall clock time of the
        // 'reshardProgressMark' oplog entry to the current time.
        advanceTime(Milliseconds(1500));
        runFetcher(testCtx.fetcher.get(), mockCursorResponse);

        OplogFetcherState expected;
        expected.oplogApplicationStarted = false;
        expected.storedProgress = testOptions.storeProgress;
        expected.lastFetchedCount = 1;
        expected.lastFetchedOplogId = {postBatchResumeToken, postBatchResumeToken};
        expected.lastFetchedOplogWallClockTime = now();

        assertOplogEntriesCount(testCtx.outputCollectionNss, expected);
        assertLastOplogEntryProgressMark(testCtx.outputCollectionNss, expected);

        resetResharding();
    }
}

TEST_F(
    ReshardingOplogFetcherProgressMarkOplogTest,
    BeforeOplogApplication_NotInsertProgressMarkOplog_BatchNotEmptyAndResumeTimestampEqualToLastOplogTimestamp) {
    auto testNum = 0;
    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(10635003,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        testNum++;
        auto testCtx = makeTestContext(testNum, testOptions);

        auto oplogTs = testCtx.startAt.getTs() + 1;
        auto postBatchResumeToken = oplogTs;
        auto oplogId = ReshardingDonorOplogId(oplogTs, oplogTs);
        auto oplogWallClockTime =
            Date_t::fromMillisSinceEpoch(postBatchResumeToken.getSecs() * 1000);
        auto oplogEntry = makeOplog(testCtx.dataCollectionNss,
                                    testCtx.dataCollectionUUID,
                                    repl::OpTypeEnum::kInsert,
                                    BSONObj() /* oField */,
                                    BSONObj() /* o2Field */,
                                    oplogWallClockTime,
                                    oplogId);
        auto oplogEntries = BSON_ARRAY(oplogEntry.toBSON());
        auto mockCursorResponse = makeMockAggregateResponse(postBatchResumeToken, oplogEntries);
        // Advance the clock to verify that the fetcher sets the wall clock time of every oplog
        // entry it fetches to the current time. It should not insert a 'reshardProgressMark' oplog
        // entry since the batch is not empty.
        advanceTime(Milliseconds(1500));
        runFetcher(testCtx.fetcher.get(), mockCursorResponse);

        OplogFetcherState expected;
        expected.oplogApplicationStarted = false;
        expected.storedProgress = testOptions.storeProgress;
        expected.lastFetchedCount = 1;
        expected.lastFetchedOplogId = oplogId;
        expected.lastFetchedOplogWallClockTime = now();
        expected.lastFetchedOplogEntry = oplogEntry;

        assertOplogEntriesCount(testCtx.outputCollectionNss, expected);
        assertLastOplogNotProgressMark(testCtx.outputCollectionNss, expected);

        resetResharding();
    }
}

TEST_F(
    ReshardingOplogFetcherProgressMarkOplogTest,
    BeforeOplogApplication_NotInsertProgressMarkOplog_BatchNotEmptyAndResumeTimestampGreaterThanLastOplogTimestamp) {
    auto testNum = 0;
    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(10634900,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        testNum++;
        auto testCtx = makeTestContext(testNum, testOptions);

        auto oplogTs = testCtx.startAt.getTs() + 1;
        auto postBatchResumeToken = oplogTs + 1;
        auto oplogId = ReshardingDonorOplogId(oplogTs, oplogTs);
        auto oplogWallClockTime =
            Date_t::fromMillisSinceEpoch(postBatchResumeToken.getSecs() * 1000);
        auto oplogEntry = makeOplog(testCtx.dataCollectionNss,
                                    testCtx.dataCollectionUUID,
                                    repl::OpTypeEnum::kInsert,
                                    BSONObj() /* oField */,
                                    BSONObj() /* o2Field */,
                                    oplogWallClockTime,
                                    oplogId);
        auto oplogEntries = BSON_ARRAY(oplogEntry.toBSON());
        auto mockCursorResponse = makeMockAggregateResponse(postBatchResumeToken, oplogEntries);
        // Advance the clock to verify that the fetcher sets the wall clock time of every oplog
        // entry it fetches to the current time. It should not insert a 'reshardProgressMark' oplog
        // entry since the batch is not empty.
        advanceTime(Milliseconds(1500));
        runFetcher(testCtx.fetcher.get(), mockCursorResponse);

        OplogFetcherState expected;
        expected.oplogApplicationStarted = false;
        expected.storedProgress = testOptions.storeProgress;
        expected.lastFetchedCount = 1;
        expected.lastFetchedOplogId = oplogId;
        expected.lastFetchedOplogWallClockTime = now();
        expected.lastFetchedOplogEntry = oplogEntry;

        assertOplogEntriesCount(testCtx.outputCollectionNss, expected);
        assertLastOplogNotProgressMark(testCtx.outputCollectionNss, expected);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherProgressMarkOplogTest,
       BeforeOplogApplication_NotInsertProgressMarkOplog_BatchEmptyAndTimestampEqualToLastSeen) {
    auto testNum = 0;
    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(10635004,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        testNum++;
        auto testCtx = makeTestContext(testNum, testOptions);

        // Mock a non-empty batch so that the last seen timestamp advances.
        auto postBatchResumeToken0 = testCtx.startAt.getTs() + 1;
        auto oplogId0 = ReshardingDonorOplogId(postBatchResumeToken0, postBatchResumeToken0);
        auto oplogWallClockTime0 =
            Date_t::fromMillisSinceEpoch(postBatchResumeToken0.getSecs() * 1000);
        auto oplogEntry0 = makeOplog(testCtx.dataCollectionNss,
                                     testCtx.dataCollectionUUID,
                                     repl::OpTypeEnum::kInsert,
                                     BSONObj() /* oField */,
                                     BSONObj() /* o2Field */,
                                     oplogWallClockTime0,
                                     oplogId0);
        auto oplogEntries0 = BSON_ARRAY(oplogEntry0.toBSON());
        auto mockCursorResponse0 = makeMockAggregateResponse(postBatchResumeToken0, oplogEntries0);
        // Advance the clock to verify that the fetcher sets the wall clock time of every oplog
        // entry it fetches to the current time. It should not insert a 'reshardProgressMark' oplog
        // entry since the batch is not empty.
        advanceTime(Milliseconds(1500));
        runFetcher(testCtx.fetcher.get(), mockCursorResponse0);

        OplogFetcherState expected;
        expected.oplogApplicationStarted = false;
        expected.storedProgress = testOptions.storeProgress;
        expected.lastFetchedCount = 1;
        expected.lastFetchedOplogId = oplogId0;
        expected.lastFetchedOplogWallClockTime = now();
        expected.lastFetchedOplogEntry = oplogEntry0;

        assertOplogEntriesCount(testCtx.outputCollectionNss, expected);
        assertLastOplogNotProgressMark(testCtx.outputCollectionNss, expected);

        // Mock an empty batch and verify that the fetcher does not insert a 'reshardProgressMark'
        // oplog entry.
        auto oplogEntries1 = BSONArrayBuilder().arr();
        auto mockCursorResponse1 = makeMockAggregateResponse(postBatchResumeToken0, oplogEntries1);
        runFetcher(testCtx.fetcher.get(), mockCursorResponse1);

        assertOplogEntriesCount(testCtx.outputCollectionNss, expected);
        assertLastOplogNotProgressMark(testCtx.outputCollectionNss, expected);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherProgressMarkOplogTest,
       BeforeOplogApplication_InsertProgressMarkOplog_BatchEmptyAndTimestampGreaterThanLastSeen) {
    auto testNum = 0;
    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(10635005,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        testNum++;
        auto testCtx = makeTestContext(testNum, testOptions);

        // Mock a non-empty batch so that the last seen timestamp advances.
        auto postBatchResumeToken0 = testCtx.startAt.getTs() + 1;
        auto oplogId0 = ReshardingDonorOplogId(postBatchResumeToken0, postBatchResumeToken0);
        auto oplogWallClockTime0 =
            Date_t::fromMillisSinceEpoch(postBatchResumeToken0.getSecs() * 1000);
        auto oplogEntry0 = makeOplog(testCtx.dataCollectionNss,
                                     testCtx.dataCollectionUUID,
                                     repl::OpTypeEnum::kInsert,
                                     BSONObj() /* oField */,
                                     BSONObj() /* o2Field */,
                                     oplogWallClockTime0,
                                     oplogId0);
        auto oplogEntries0 = BSON_ARRAY(oplogEntry0.toBSON());
        auto mockCursorResponse0 = makeMockAggregateResponse(postBatchResumeToken0, oplogEntries0);
        // Advance the clock to verify that the fetcher sets the wall clock time of every oplog
        // entry it fetches to the current time. It should not insert a 'reshardProgressMark' oplog
        // entry since the batch is not empty.
        advanceTime(Milliseconds(1500));
        runFetcher(testCtx.fetcher.get(), mockCursorResponse0);

        OplogFetcherState expected;
        expected.oplogApplicationStarted = false;
        expected.storedProgress = testOptions.storeProgress;
        expected.lastFetchedCount = 1;
        expected.lastFetchedOplogId = oplogId0;
        expected.lastFetchedOplogWallClockTime = now();
        expected.lastFetchedOplogEntry = oplogEntry0;

        assertOplogEntriesCount(testCtx.outputCollectionNss, expected);
        assertLastOplogNotProgressMark(testCtx.outputCollectionNss, expected);

        // Mock an empty batch but with a resume timestamp greater than the last seen timestamp and
        // verify that the fetcher inserts a 'reshardProgressMark' oplog entry.
        auto postBatchResumeToken1 = postBatchResumeToken0 + 1;
        auto oplogEntries1 = BSONArrayBuilder().arr();
        auto mockCursorResponse1 = makeMockAggregateResponse(postBatchResumeToken1, oplogEntries1);
        runFetcher(testCtx.fetcher.get(), mockCursorResponse1);

        expected.lastFetchedCount = 2;
        expected.lastFetchedOplogId = {postBatchResumeToken1, postBatchResumeToken1};
        expected.lastFetchedOplogWallClockTime = now();

        assertOplogEntriesCount(testCtx.outputCollectionNss, expected);
        assertLastOplogEntryProgressMark(testCtx.outputCollectionNss, expected);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherProgressMarkOplogTest,
       DuringOplogApplication_EmptyBatchAndResumeTimestampEqualToStartAt) {
    auto testNum = 0;
    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(10634901,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        testNum++;
        auto testCtx = makeTestContext(testNum, testOptions);
        testCtx.fetcher->onStartingOplogApplication();

        auto postBatchResumeToken = testCtx.startAt.getTs();
        auto oplogEntries = BSONArrayBuilder().arr();
        auto mockCursorResponse = makeMockAggregateResponse(postBatchResumeToken, oplogEntries);
        // Advance the clock to verify that the fetcher it sets the wall clock time of the
        // 'reshardProgressMark' to the current time.
        advanceTime(Milliseconds(1500));
        runFetcher(testCtx.fetcher.get(), mockCursorResponse);

        if (testOptions.movingAvgFeatureFlag && testOptions.movingAvgServerParameter) {
            // If the recipient has been configured to estimate the remaining time based on moving
            // average, then the fetcher needs to insert a 'reshardProgressMark' oplog entry at
            // least once within the configured interval. So it is expected to insert a
            // 'reshardProgressMark' oplog entry after getting the first batch.
            OplogFetcherState expected;
            expected.oplogApplicationStarted = true;
            expected.storedProgress = testOptions.storeProgress;
            expected.lastFetchedCount = 1;
            expected.lastFetchedOplogId = {postBatchResumeToken, postBatchResumeToken};
            expected.lastFetchedOplogId.setProgressMarkId(0);
            expected.lastFetchedOplogWallClockTime = now();

            assertLastOplogEntryProgressMark(testCtx.outputCollectionNss, expected);
            assertOplogEntriesCount(testCtx.outputCollectionNss, expected);
        } else {
            assertNoOplogEntries(testCtx.outputCollectionNss);
        }

        resetResharding();
    }
}

TEST_F(
    ReshardingOplogFetcherProgressMarkOplogTest,
    DuringOplogApplication_InsertProgressMarkOplog_BatchEmptyAndResumeTimestampGreaterThanStartAt) {
    auto testNum = 0;
    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(10634902,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        testNum++;
        auto testCtx = makeTestContext(testNum, testOptions);
        testCtx.fetcher->onStartingOplogApplication();

        auto postBatchResumeToken = testCtx.startAt.getTs() + 1;
        auto oplogEntries = BSONArrayBuilder().arr();
        auto mockCursorResponse = makeMockAggregateResponse(postBatchResumeToken, oplogEntries);
        // Advance the clock to verify that the fetcher sets the wall clock time of the
        // 'reshardProgressMark' to the current time.
        advanceTime(Milliseconds(1500));
        runFetcher(testCtx.fetcher.get(), mockCursorResponse);

        OplogFetcherState expected;
        expected.oplogApplicationStarted = true;
        expected.storedProgress = testOptions.storeProgress;
        expected.lastFetchedCount = 1;
        expected.lastFetchedOplogId = {postBatchResumeToken, postBatchResumeToken};
        if (testOptions.movingAvgFeatureFlag && testOptions.movingAvgServerParameter) {
            expected.lastFetchedOplogId.setProgressMarkId(0);
        }
        expected.lastFetchedOplogWallClockTime = now();

        assertLastOplogEntryProgressMark(testCtx.outputCollectionNss, expected);
        assertOplogEntriesCount(testCtx.outputCollectionNss, expected);

        resetResharding();
    }
}

TEST_F(
    ReshardingOplogFetcherProgressMarkOplogTest,
    DuringOplogApplication_NotInsertProgressMarkOplog_BatchNotEmptyAndResumeTimestampEqualToLastOplogTimestamp) {
    auto testNum = 0;
    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(10634903,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        testNum++;
        auto testCtx = makeTestContext(testNum, testOptions);
        testCtx.fetcher->onStartingOplogApplication();

        auto oplogTs = testCtx.startAt.getTs() + 1;
        auto postBatchResumeToken = oplogTs;
        auto oplogId = ReshardingDonorOplogId(oplogTs, oplogTs);
        auto oplogWallClockTime =
            Date_t::fromMillisSinceEpoch(postBatchResumeToken.getSecs() * 1000);
        auto oplogEntry = makeOplog(testCtx.dataCollectionNss,
                                    testCtx.dataCollectionUUID,
                                    repl::OpTypeEnum::kInsert,
                                    BSONObj() /* oField */,
                                    BSONObj() /* o2Field */,
                                    oplogWallClockTime,
                                    oplogId);
        auto oplogEntries = BSON_ARRAY(oplogEntry.toBSON());
        auto mockCursorResponse = makeMockAggregateResponse(postBatchResumeToken, oplogEntries);
        // Advance the clock to verify that the fetcher sets the wall clock time of every oplog
        // entry it fetches to the current time. It should not insert a 'reshardProgressMark' oplog
        // entry since the batch is not empty.
        advanceTime(Milliseconds(1500));
        runFetcher(testCtx.fetcher.get(), mockCursorResponse);

        OplogFetcherState expected;
        expected.oplogApplicationStarted = true;
        expected.storedProgress = testOptions.storeProgress;

        expected.lastFetchedCount = 1;
        expected.lastFetchedOplogId = oplogId;
        expected.lastFetchedOplogWallClockTime = now();
        expected.lastFetchedOplogEntry = oplogEntry;

        assertLastOplogNotProgressMark(testCtx.outputCollectionNss, expected);
        assertOplogEntriesCount(testCtx.outputCollectionNss, expected);

        resetResharding();
    }
}

TEST_F(
    ReshardingOplogFetcherProgressMarkOplogTest,
    DuringOplogApplication_NotInsertProgressMarkOplog_BatchNotEmptyAndResumeTimestampGreaterThanLastOplogTimestamp) {
    auto testNum = 0;
    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(10634904,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        testNum++;
        auto testCtx = makeTestContext(testNum, testOptions);
        testCtx.fetcher->onStartingOplogApplication();

        auto oplogTs = testCtx.startAt.getTs() + 1;
        auto postBatchResumeToken = oplogTs + 1;
        auto oplogId = ReshardingDonorOplogId(oplogTs, oplogTs);
        auto oplogWallClockTime =
            Date_t::fromMillisSinceEpoch(postBatchResumeToken.getSecs() * 1000);
        auto oplogEntry = makeOplog(testCtx.dataCollectionNss,
                                    testCtx.dataCollectionUUID,
                                    repl::OpTypeEnum::kInsert,
                                    BSONObj() /* oField */,
                                    BSONObj() /* o2Field */,
                                    oplogWallClockTime,
                                    oplogId);
        auto oplogEntries = BSON_ARRAY(oplogEntry.toBSON());
        auto mockCursorResponse = makeMockAggregateResponse(postBatchResumeToken, oplogEntries);
        // Advance the clock to verify that the fetcher sets the wall clock time of every oplog
        // entry it fetches to the current time. It should not insert a 'reshardProgressMark' oplog
        // entry since the batch is not empty.
        advanceTime(Milliseconds(1500));
        runFetcher(testCtx.fetcher.get(), mockCursorResponse);

        OplogFetcherState expected;
        expected.oplogApplicationStarted = true;
        expected.storedProgress = testOptions.storeProgress;
        expected.lastFetchedCount = 1;
        expected.lastFetchedOplogId = oplogId;
        expected.lastFetchedOplogWallClockTime = now();
        expected.lastFetchedOplogEntry = oplogEntry;

        assertLastOplogNotProgressMark(testCtx.outputCollectionNss, expected);
        assertOplogEntriesCount(testCtx.outputCollectionNss, expected);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherProgressMarkOplogTest,
       DuringOplogApplication_BatchEmptyAndTimestampEqualToLastSeen) {
    auto movingAvgInterval = Milliseconds(50);
    RAIIServerParameterControllerForTest intervalMillisServerParameter{
        "reshardingExponentialMovingAverageTimeToFetchAndApplyIntervalMillis",
        movingAvgInterval.count()};

    auto testNum = 0;
    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(10634905,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        testNum++;
        auto testCtx = makeTestContext(testNum, testOptions);
        testCtx.fetcher->onStartingOplogApplication();

        // Mock a non-empty batch so that the last seen timestamp advances.
        auto postBatchResumeToken0 = testCtx.startAt.getTs() + 1;
        auto oplogId0 = ReshardingDonorOplogId(postBatchResumeToken0, postBatchResumeToken0);
        auto oplogWallClockTime0 =
            Date_t::fromMillisSinceEpoch(postBatchResumeToken0.getSecs() * 1000);
        auto oplogEntry0 = makeOplog(testCtx.dataCollectionNss,
                                     testCtx.dataCollectionUUID,
                                     repl::OpTypeEnum::kInsert,
                                     BSONObj() /* oField */,
                                     BSONObj() /* o2Field */,
                                     oplogWallClockTime0,
                                     oplogId0);
        auto oplogEntries0 = BSON_ARRAY(oplogEntry0.toBSON());
        auto mockCursorResponse0 = makeMockAggregateResponse(postBatchResumeToken0, oplogEntries0);
        // Advance the clock to verify that the fetcher sets the wall clock time of every oplog
        // entry it fetches to the current time. It should not insert a 'reshardProgressMark' oplog
        // entry since the batch is not empty.
        advanceTime(Milliseconds(1500));
        runFetcher(testCtx.fetcher.get(), mockCursorResponse0);

        OplogFetcherState expected;
        expected.oplogApplicationStarted = true;
        expected.storedProgress = testOptions.storeProgress;
        expected.lastFetchedOplogId = oplogId0;
        expected.lastFetchedCount = 1;
        expected.lastFetchedOplogWallClockTime = now();
        expected.lastFetchedOplogEntry = oplogEntry0;

        assertLastOplogNotProgressMark(testCtx.outputCollectionNss, expected);
        assertOplogEntriesCount(testCtx.outputCollectionNss, expected);

        // Mock an empty batch immediately and verify that the fetcher only inserts a
        // 'reshardProgressMark' oplog entry if the recipient has been configured to estimate
        // the remaining time based on moving average.
        auto oplogEntries1 = BSONArrayBuilder().arr();
        auto mockCursorResponse1 = makeMockAggregateResponse(postBatchResumeToken0, oplogEntries1);
        runFetcher(testCtx.fetcher.get(), mockCursorResponse1);

        if (testOptions.movingAvgFeatureFlag && testOptions.movingAvgServerParameter) {
            // If the recipient has been configured to estimate the remaining time based on moving
            // average, then the fetcher needs to insert a 'reshardProgressMark' oplog entry at
            // least once within the configured interval. So it is expected to insert a
            // 'reshardProgressMark' oplog entry after getting the first batch.
            expected.lastFetchedCount = 2;
            expected.lastFetchedOplogId.setProgressMarkId(0);
            expected.lastFetchedOplogWallClockTime = now();

            assertLastOplogEntryProgressMark(testCtx.outputCollectionNss, expected);
        } else {
            assertLastOplogNotProgressMark(testCtx.outputCollectionNss, expected);
        }
        assertOplogEntriesCount(testCtx.outputCollectionNss, expected);

        // Mock another empty batch immediately and verify that the fetcher does not insert a
        // 'reshardProgressMark' oplog entry, even if the recipient has been configured to estimate
        // the remaining time based on moving average.
        auto oplogEntries2 = BSONArrayBuilder().arr();
        auto mockCursorResponse2 = makeMockAggregateResponse(postBatchResumeToken0, oplogEntries2);
        runFetcher(testCtx.fetcher.get(), mockCursorResponse1);

        if (testOptions.movingAvgFeatureFlag && testOptions.movingAvgServerParameter) {
            assertLastOplogEntryProgressMark(testCtx.outputCollectionNss, expected);
        } else {
            assertLastOplogNotProgressMark(testCtx.outputCollectionNss, expected);
        }
        assertOplogEntriesCount(testCtx.outputCollectionNss, expected);

        // Mock another empty batch after advancing the clock by the interval for updating the
        // exponential moving average time to fetch and apply oplog entries, and then verify that
        // the fetcher only inserts a 'reshardProgressMark' oplog entry if the recipient has been
        // configured to estimate the remaining time based on moving average.
        advanceTime(movingAvgInterval);

        auto oplogEntries3 = BSONArrayBuilder().arr();
        auto mockCursorResponse3 = makeMockAggregateResponse(postBatchResumeToken0, oplogEntries3);
        runFetcher(testCtx.fetcher.get(), mockCursorResponse3);

        if (testOptions.movingAvgFeatureFlag && testOptions.movingAvgServerParameter) {
            expected.lastFetchedCount = 3;
            // The new 'reshardProgressMark' oplog entry should have progress mark id equal to 1
            // instead of 0, and the wall clock time equal to the current time.
            expected.lastFetchedOplogId.setProgressMarkId(1);
            expected.lastFetchedOplogWallClockTime = now();
            assertLastOplogEntryProgressMark(testCtx.outputCollectionNss, expected);
        } else {
            assertLastOplogNotProgressMark(testCtx.outputCollectionNss, expected);
        }
        assertOplogEntriesCount(testCtx.outputCollectionNss, expected);

        resetResharding();
    }
}

}  // namespace
}  // namespace mongo
