/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/s/resharding/resharding_recipient_service.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/index_builds/index_builds_coordinator_mock.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/lock_manager/locker.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/db/s/resharding/resharding_change_event_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_data_replication.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_progress_gen.h"
#include "mongo/db/s/resharding/resharding_oplog_fetcher_progress_gen.h"
#include "mongo/db/s/resharding/resharding_recipient_service_external_state.h"
#include "mongo/db/s/resharding/resharding_service_test_helpers.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/sharding_test_fixture_common.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/uuid.h"

#include <initializer_list>
#include <memory>
#include <ostream>
#include <string>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(failToCreateReshardingDataReplicationForTest);

using RecipientStateTransitionController =
    resharding_service_test_helpers::StateTransitionController<RecipientStateEnum>;
using PauseDuringStateTransitions =
    resharding_service_test_helpers::PauseDuringStateTransitions<RecipientStateEnum>;
using OpObserverForTest = resharding_service_test_helpers::
    StateTransitionControllerOpObserver<RecipientStateEnum, ReshardingRecipientDocument>;
const ShardId recipientShardId{"myShardId"};
const long approxBytesToCopy = 10000;
const long approxDocumentsToCopy = 100;

const BSONObj sourceCollectionOptions = BSONObj();
BSONObj tempReshardingCollectionOptions = BSONObj();

template <typename DocumentType>
DocumentType getPersistedStateDocument(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       boost::optional<UUID> reshardingUUID) {
    DBDirectClient client(opCtx);
    auto query = reshardingUUID ? BSON(DocumentType::kReshardingUUIDFieldName << *reshardingUUID)
                                : BSONObj{};
    auto doc = client.findOne(nss, query);
    return DocumentType::parse(
        doc, IDLParserContext("reshardingRecipientServiceTest::getPersistedStateDocument"));
}

ReshardingRecipientDocument getPersistedRecipientDocument(
    OperationContext* opCtx, boost::optional<UUID> reshardingUUID = boost::none) {
    return getPersistedStateDocument<ReshardingRecipientDocument>(
        opCtx, NamespaceString::kRecipientReshardingOperationsNamespace, reshardingUUID);
}

ReshardingCoordinatorDocument getPersistedCoordinatorDocument(
    OperationContext* opCtx, boost::optional<UUID> reshardingUUID = boost::none) {
    return getPersistedStateDocument<ReshardingCoordinatorDocument>(
        opCtx, NamespaceString::kConfigReshardingOperationsNamespace, reshardingUUID);
}

RecipientStateEnum getRecipientPhaseOnDisk(OperationContext* opCtx) {
    return getPersistedRecipientDocument(opCtx).getMutableState().getState();
}

class DataReplicationForTest : public ReshardingDataReplicationInterface {
public:
    DataReplicationForTest() {
        if (failToCreateReshardingDataReplicationForTest.shouldFail()) {
            uasserted(ErrorCodes::InternalError, "Failed to create DataReplicationForTest");
        }
    }
    SemiFuture<void> runUntilStrictlyConsistent(
        std::shared_ptr<executor::TaskExecutor> executor,
        std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
        CancellationToken cancelToken,
        CancelableOperationContextFactory opCtxFactory,
        const mongo::Date_t& startConfigTxnCloneTime) override {
        return makeReadyFutureWith([] {}).semi();
    };

    void startOplogApplication() override {};

    void prepareForCriticalSection() override {};

    SharedSemiFuture<void> awaitCloningDone() override {
        return makeReadyFutureWith([] {}).share();
    };

    SharedSemiFuture<void> awaitStrictlyConsistent() override {
        return makeReadyFutureWith([] {}).share();
    };

    void shutdown() override {}

    void join() override {}
};

class ExternalStateForTestImpl {
public:
    enum class ExternalFunction {
        kRefreshCatalogCache,
        kGetTrackedCollectionRoutingInfo,
        kGetCollectionOptions,
        kGetCollectionIndexes,
        kEnsureReshardingStashCollectionsEmpty,
        kMakeDataReplication,
    };

    ShardId myShardId(ServiceContext* serviceContext) const {
        return recipientShardId;
    }

    void refreshCatalogCache(OperationContext* opCtx, const NamespaceString& nss) {
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kRefreshCatalogCache);
    }

    CollectionRoutingInfo getTrackedCollectionRoutingInfo(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kGetTrackedCollectionRoutingInfo);
        invariant(nss == _sourceNss);

        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks = {ChunkType{
            _sourceUUID,
            ChunkRange{BSON(_currentShardKey << MINKEY), BSON(_currentShardKey << MAXKEY)},
            ChunkVersion({epoch, Timestamp(1, 1)}, {100, 0}),
            _someDonorId}};

        auto rt = RoutingTableHistory::makeNew(_sourceNss,
                                               _sourceUUID,
                                               BSON(_currentShardKey << 1),
                                               false, /* unsplittable */
                                               nullptr /* defaultCollator */,
                                               false /* unique */,
                                               epoch,
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               true /* allowMigrations */,
                                               chunks);
        return CollectionRoutingInfo{
            ChunkManager(
                ShardingTestFixtureCommon::makeStandaloneRoutingTableHistory(std::move(rt)),
                boost::none /* clusterTime */),
            DatabaseTypeValueHandle(DatabaseType{
                nss.dbName(), _someDonorId, DatabaseVersion(UUID::gen(), Timestamp(1, 1))})};
    }

    MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        boost::optional<Timestamp> afterClusterTime,
        StringData reason) {
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kGetCollectionOptions);
        if (nss == _sourceNss) {
            return {sourceCollectionOptions, uuid};
        } else {
            return {tempReshardingCollectionOptions, uuid};
        }
    }

    MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        boost::optional<Timestamp> afterClusterTime,
        StringData reason,
        const ShardId& fromShardId) {
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kGetCollectionOptions);
        return getCollectionOptions(opCtx, nss, uuid, afterClusterTime, reason);
    }

    MigrationDestinationManager::IndexesAndIdIndex getCollectionIndexes(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        Timestamp afterClusterTime,
        StringData reason,
        bool expandSimpleCollation) {
        invariant(nss == _sourceNss);
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kGetCollectionIndexes);
        return {std::vector<BSONObj>{}, BSONObj()};
    }

    void route(
        OperationContext* opCtx,
        const NamespaceString& nss,
        StringData reason,
        unique_function<void(OperationContext* opCtx, const CollectionRoutingInfo& cri)> callback) {
        callback(opCtx, getTrackedCollectionRoutingInfo(opCtx, nss));
    }

    void updateCoordinatorDocument(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update) {
        auto coll = acquireCollection(opCtx,
                                      CollectionAcquisitionRequest::fromOpCtx(
                                          opCtx,
                                          NamespaceString::kConfigReshardingOperationsNamespace,
                                          AcquisitionPrerequisites::kWrite),
                                      MODE_IX);
        Helpers::update(opCtx, coll, query, update);
    }

    void clearFilteringMetadataOnTempReshardingCollection(
        OperationContext* opCtx, const NamespaceString& tempReshardingNss) {}

    void ensureReshardingStashCollectionsEmpty(
        OperationContext* opCtx,
        const UUID& sourceUUID,
        const std::vector<DonorShardFetchTimestamp>& donorShards) {
        _maybeThrowErrorForFunction(opCtx,
                                    ExternalFunction::kEnsureReshardingStashCollectionsEmpty);

        for (const auto& donor : donorShards) {
            auto stashNss =
                resharding::getLocalConflictStashNamespace(sourceUUID, donor.getShardId());
            const auto stashColl =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest::fromOpCtx(
                                      opCtx, stashNss, AcquisitionPrerequisites::kRead),
                                  MODE_IS);
            uassert(10494621,
                    "Resharding completed with non-empty stash collections",
                    !stashColl.exists() || stashColl.getCollectionPtr()->isEmpty(opCtx));
        }
    }

    std::unique_ptr<ReshardingDataReplicationInterface> makeDataReplication(
        OperationContext* opCtx,
        ReshardingMetrics* metrics,
        ReshardingApplierMetricsMap* applierMetrics,
        const CommonReshardingMetadata& metadata,
        const std::vector<DonorShardFetchTimestamp>& donorShards,
        std::size_t oplogBatchTaskCount,
        Timestamp cloneTimestamp,
        bool cloningDone,
        bool storeOplogFetcherProgress,
        bool relaxed) {
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kMakeDataReplication);
        setDataReplicationStarted();
        return std::make_unique<DataReplicationForTest>();
    }

    void throwUnrecoverableErrorIn(RecipientStateEnum phase, ExternalFunction func) {
        _errorFunction = std::make_tuple(phase, func);
    }

    void notifyNewInstanceStarted() {
        stdx::lock_guard guard(_instanceStateMutex);
        _instanceSpecificState = std::make_unique<InstanceSpecificState>();
    }

    bool setDataReplicationStarted() {
        stdx::lock_guard guard(_instanceStateMutex);
        return _instanceSpecificState->dataReplicationRunning = true;
    }

    bool isDataReplicationRunning() {
        stdx::lock_guard guard(_instanceStateMutex);
        return _instanceSpecificState->dataReplicationRunning;
    }

private:
    struct InstanceSpecificState {
        bool dataReplicationRunning = false;
    };

    const StringData _currentShardKey = "oldKey";

    const NamespaceString _sourceNss =
        NamespaceString::createNamespaceString_forTest("sourcedb", "sourcecollection");
    const UUID _sourceUUID = UUID::gen();

    const ShardId _someDonorId{"myDonorId"};

    boost::optional<std::tuple<RecipientStateEnum, ExternalFunction>> _errorFunction = boost::none;

    stdx::mutex _instanceStateMutex;
    std::unique_ptr<InstanceSpecificState> _instanceSpecificState =
        std::make_unique<InstanceSpecificState>();

    void _maybeThrowErrorForFunction(OperationContext* opCtx, ExternalFunction func) {
        if (_errorFunction) {
            auto [expectedPhase, expectedFunction] = *_errorFunction;

            if (getRecipientPhaseOnDisk(opCtx) == expectedPhase && func == expectedFunction) {
                uasserted(ErrorCodes::InternalError, "Simulating unrecoverable error for testing");
            }
        }
    }
};

class ExternalStateForTest : public ReshardingRecipientService::RecipientStateMachineExternalState {
public:
    ExternalStateForTest(std::shared_ptr<ExternalStateForTestImpl> impl =
                             std::make_shared<ExternalStateForTestImpl>())
        : _impl(impl) {}

    ShardId myShardId(ServiceContext* serviceContext) const override {
        return _impl->myShardId(serviceContext);
    }

    void refreshCatalogCache(OperationContext* opCtx, const NamespaceString& nss) override {
        _impl->refreshCatalogCache(opCtx, nss);
    }

    CollectionRoutingInfo getTrackedCollectionRoutingInfo(OperationContext* opCtx,
                                                          const NamespaceString& nss) override {
        return _impl->getTrackedCollectionRoutingInfo(opCtx, nss);
    }

    MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        boost::optional<Timestamp> afterClusterTime,
        StringData reason) override {
        return _impl->getCollectionOptions(opCtx, nss, uuid, afterClusterTime, reason);
    }

    MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        boost::optional<Timestamp> afterClusterTime,
        StringData reason,
        const ShardId& fromShardId) override {
        return _impl->getCollectionOptions(opCtx, nss, uuid, afterClusterTime, reason, fromShardId);
    }

    MigrationDestinationManager::IndexesAndIdIndex getCollectionIndexes(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        Timestamp afterClusterTime,
        StringData reason,
        bool expandSimpleCollation) override {
        return _impl->getCollectionIndexes(
            opCtx, nss, uuid, afterClusterTime, reason, expandSimpleCollation);
    }

    void route(OperationContext* opCtx,
               const NamespaceString& nss,
               StringData reason,
               unique_function<void(OperationContext* opCtx, const CollectionRoutingInfo& cri)>
                   callback) override {
        _impl->route(opCtx, nss, reason, std::move(callback));
    }

    void updateCoordinatorDocument(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update) override {
        _impl->updateCoordinatorDocument(opCtx, query, update);
    }

    void clearFilteringMetadataOnTempReshardingCollection(
        OperationContext* opCtx, const NamespaceString& tempReshardingNss) override {}

    void ensureReshardingStashCollectionsEmpty(
        OperationContext* opCtx,
        const UUID& sourceUUID,
        const std::vector<DonorShardFetchTimestamp>& donorShards) override {
        _impl->ensureReshardingStashCollectionsEmpty(opCtx, sourceUUID, donorShards);
    }

    std::unique_ptr<ReshardingDataReplicationInterface> makeDataReplication(
        OperationContext* opCtx,
        ReshardingMetrics* metrics,
        ReshardingApplierMetricsMap* applierMetrics,
        const CommonReshardingMetadata& metadata,
        const std::vector<DonorShardFetchTimestamp>& donorShards,
        std::size_t oplogBatchTaskCount,
        Timestamp cloneTimestamp,
        bool cloningDone,
        bool storeOplogFetcherProgress,
        bool relaxed) override {
        return _impl->makeDataReplication(opCtx,
                                          metrics,
                                          applierMetrics,
                                          metadata,
                                          donorShards,
                                          oplogBatchTaskCount,
                                          cloneTimestamp,
                                          cloningDone,
                                          storeOplogFetcherProgress,
                                          relaxed);
    }

private:
    std::shared_ptr<ExternalStateForTestImpl> _impl;
};

class ReshardingRecipientServiceForTest : public ReshardingRecipientService {
public:
    explicit ReshardingRecipientServiceForTest(
        ServiceContext* serviceContext, std::shared_ptr<ExternalStateForTestImpl> externalStateImpl)
        : ReshardingRecipientService(serviceContext),
          _serviceContext(serviceContext),
          _externalStateImpl(externalStateImpl) {}

    std::shared_ptr<repl::PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) override {
        _externalStateImpl->notifyNewInstanceStarted();
        return std::make_shared<RecipientStateMachine>(
            this,
            ReshardingRecipientDocument::parse(
                initialState, IDLParserContext{"ReshardingRecipientServiceForTest"}),
            std::make_unique<ExternalStateForTest>(_externalStateImpl),
            _serviceContext);
    }

private:
    ServiceContext* _serviceContext;
    std::shared_ptr<ExternalStateForTestImpl> _externalStateImpl;
};

struct TestOptions {
    bool isAlsoDonor;
    bool skipCloningAndApplying;
    bool noChunksToCopy;
    bool storeOplogFetcherProgress = true;
    bool performVerification = true;
    bool driveCloneNoRefresh;

    BSONObj toBSON() const {
        BSONObjBuilder bob;
        bob.append("isAlsoDonor", isAlsoDonor);
        bob.append("skipCloningAndApplying", skipCloningAndApplying);
        bob.append("noChunksToCopy", noChunksToCopy);
        bob.append("storeOplogFetcherProgress", storeOplogFetcherProgress);
        bob.append("performVerification", performVerification);
        bob.append("driveCloneNoRefresh", driveCloneNoRefresh);
        return bob.obj();
    }
};

bool shouldDataReplicationBeRunningIn(const TestOptions& testOptions, RecipientStateEnum phase) {
    if (testOptions.skipCloningAndApplying) {
        return false;
    }
    return phase == RecipientStateEnum::kCloning || phase == RecipientStateEnum::kBuildingIndex ||
        phase == RecipientStateEnum::kApplying;
}

std::vector<TestOptions> makeBasicTestOptions() {
    std::vector<TestOptions> testOptions;
    for (bool isAlsoDonor : {false, true}) {
        for (bool skipCloningAndApplying : {false, true}) {
            for (bool driveCloneNoRefresh : {false, true}) {
                RAIIServerParameterControllerForTest cloneNoRefreshFeatureFlagController(
                    "featureFlagReshardingCloneNoRefresh", driveCloneNoRefresh);

                testOptions.push_back({isAlsoDonor, skipCloningAndApplying, driveCloneNoRefresh});
            }
        }
    }
    return testOptions;
}

std::vector<TestOptions> makeAllTestOptions() {
    std::vector<TestOptions> testOptions;
    for (bool isAlsoDonor : {false, true}) {
        for (bool skipCloningAndApplying : {false, true}) {
            for (bool noChunksToCopy : {false, true}) {
                for (bool storeOplogFetcherProgress : {false, true}) {
                    for (bool performVerification : {false, true}) {
                        for (bool driveCloneNoRefresh : {false, true}) {
                            if (skipCloningAndApplying && !noChunksToCopy) {
                                // This is an invalid combination.
                                continue;
                            }

                            RAIIServerParameterControllerForTest
                                cloneNoRefreshFeatureFlagController(
                                    "featureFlagReshardingCloneNoRefresh", driveCloneNoRefresh);

                            testOptions.push_back({isAlsoDonor,
                                                   skipCloningAndApplying,
                                                   noChunksToCopy,
                                                   storeOplogFetcherProgress,
                                                   performVerification,
                                                   driveCloneNoRefresh});
                        }
                    }
                }
            }
        }
    }
    return testOptions;
}

BSONObj makeTestDocumentForInsert(int i, UUID& id) {
    return BSON("_id" << id.toBSON() << "x" << i << "y" << i);
}

BSONObj makeTestDocumentForInsert(int i) {
    auto id = UUID::gen();
    return makeTestDocumentForInsert(i, id);
}

BSONObj makeTestDocumentUpdateStatement() {
    return BSON("$inc" << BSON("x" << 1));
}

void runRandomizedLocking(OperationContext* opCtx,
                          Locker& locker,
                          const ResourceId& resId,
                          AtomicWord<bool>& keepRunning) {
    PseudoRandom random = PseudoRandom(123456);

    while (keepRunning.load()) {
        try {
            locker.lockGlobal(opCtx, MODE_IX);
            locker.lock(opCtx, resId, MODE_X);

            // The migrationLockAcquisitionMaxWaitMS is 500 by default. Ensure the lock is held long
            // enough to trigger the lockTimeout.
            sleepFor(Milliseconds(500 + random.nextInt32(500)));

            ASSERT(locker.unlock(resId));
            ASSERT(locker.unlockGlobal());

            sleepFor(Milliseconds(random.nextInt32(5)));
        } catch (const DBException& ex) {
            LOGV2(10568801, "Error during locking/unlocking", "error"_attr = ex.toString());
        }
    }
}

struct RecipientMetricsCommon {
    int64_t docsCopied = 0;
    int64_t bytesCopied = 0;
    int64_t oplogFetched = 0;
    int64_t oplogApplied = 0;
};

struct RecipientMetricsForDonor {
    ShardId shardId;
    RecipientMetricsCommon metrics;
};

struct TestRecipientMetrics {
public:
    void add(const RecipientMetricsForDonor& metricsForDonor) {
        _metricsTotal.docsCopied += metricsForDonor.metrics.docsCopied;
        _metricsTotal.bytesCopied += metricsForDonor.metrics.bytesCopied;
        _metricsTotal.oplogFetched += metricsForDonor.metrics.oplogFetched;
        _metricsTotal.oplogApplied += metricsForDonor.metrics.oplogApplied;
        _metricsForDonors.push_back(metricsForDonor);
    }

    const RecipientMetricsCommon& getMetricsTotal() const {
        return _metricsTotal;
    }

    const std::vector<RecipientMetricsForDonor>& getMetricsForDonors() const {
        return _metricsForDonors;
    }

private:
    RecipientMetricsCommon _metricsTotal;
    std::vector<RecipientMetricsForDonor> _metricsForDonors;
};

TestRecipientMetrics makeTestRecipientMetrics(const TestOptions& testOptions,
                                              const std::vector<DonorShardFetchTimestamp>& donors) {
    if (testOptions.noChunksToCopy) {
        return {};
    }

    ASSERT_GT(donors.size(), 1);
    TestRecipientMetrics testRecipientMetrics;

    for (unsigned long i = 0; i < donors.size(); i++) {
        if (i == 0) {
            // Make the recipient not have any documents to copy or oplog entries to fetch/apply
            // from donor0.
            continue;
        }

        RecipientMetricsForDonor metricsForDonor;
        metricsForDonor.shardId = donors[i].getShardId();

        auto bytesPerDoc = makeTestDocumentForInsert(0).objsize();
        metricsForDonor.metrics.docsCopied = i;
        metricsForDonor.metrics.bytesCopied = bytesPerDoc * metricsForDonor.metrics.docsCopied;
        metricsForDonor.metrics.oplogFetched = i * 2;
        metricsForDonor.metrics.oplogApplied = metricsForDonor.metrics.oplogFetched;

        testRecipientMetrics.add(metricsForDonor);
    }
    return testRecipientMetrics;
}

/**
 * Tests the behavior of the ReshardingRecipientService upon recovery from failover.
 */
class ReshardingRecipientServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    using RecipientStateMachine = ReshardingRecipientService::RecipientStateMachine;
    using enum ExternalStateForTestImpl::ExternalFunction;

    ReshardingRecipientServiceTest()
        : repl::PrimaryOnlyServiceMongoDTest(
              Options{}.useIndexBuildsCoordinator(std::make_unique<IndexBuildsCoordinatorMock>())) {
    }

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<ReshardingRecipientServiceForTest>(serviceContext,
                                                                   _externalStateImpl);
    }

    void setUp() override {
        _externalStateImpl = std::make_shared<ExternalStateForTestImpl>();
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        auto serviceContext = getServiceContext();
        auto storageMock = std::make_unique<repl::StorageInterfaceMock>();
        repl::StorageInterface::set(serviceContext, std::move(storageMock));

        _controller = std::make_shared<RecipientStateTransitionController>();
        _opObserverRegistry->addObserver(std::make_unique<OpObserverForTest>(
            _controller,
            NamespaceString::kRecipientReshardingOperationsNamespace,
            [](const ReshardingRecipientDocument& stateDoc) {
                return stateDoc.getMutableState().getState();
            }));
    }

    RecipientStateTransitionController* controller() {
        return _controller.get();
    }

    ExternalStateForTestImpl* externalState() {
        return _externalStateImpl.get();
    }

    BSONObj newShardKeyPattern() {
        return BSON("newKey" << 1);
    }

    ReshardingRecipientDocument makeRecipientDocument(const TestOptions& testOptions) {
        RecipientShardContext recipientCtx;
        recipientCtx.setState(RecipientStateEnum::kUnused);

        ReshardingRecipientDocument doc(
            std::move(recipientCtx),
            {ShardId{"donor1"},
             testOptions.isAlsoDonor ? recipientShardId : ShardId{"donor2"},
             ShardId{"donor3"}},
            durationCount<Milliseconds>(Milliseconds{5}));

        NamespaceString sourceNss =
            NamespaceString::createNamespaceString_forTest("sourcedb", "sourcecollection");
        auto sourceUUID = UUID::gen();
        auto commonMetadata = CommonReshardingMetadata(
            UUID::gen(),
            sourceNss,
            sourceUUID,
            resharding::constructTemporaryReshardingNss(sourceNss, sourceUUID),
            newShardKeyPattern());
        commonMetadata.setStartTime(getServiceContext()->getFastClockSource()->now());

        doc.setCommonReshardingMetadata(std::move(commonMetadata));
        doc.setSkipCloningAndApplying(testOptions.skipCloningAndApplying);
        doc.setStoreOplogFetcherProgress(testOptions.storeOplogFetcherProgress);
        doc.setPerformVerification(testOptions.performVerification);
        return doc;
    }

    ReshardingCoordinatorDocument makeCoordinatorDocument(
        const ReshardingRecipientDocument& recipientDoc) {
        ReshardingCoordinatorDocument coordinatorDoc;
        coordinatorDoc.setState(CoordinatorStateEnum::kUnused);
        coordinatorDoc.setCommonReshardingMetadata(recipientDoc.getCommonReshardingMetadata());

        std::vector<DonorShardEntry> donorShards;
        std::transform(
            recipientDoc.getDonorShards().begin(),
            recipientDoc.getDonorShards().end(),
            std::back_inserter(donorShards),
            [](auto donorShard) { return DonorShardEntry{donorShard.getShardId(), {}}; });
        coordinatorDoc.setDonorShards(donorShards);

        std::vector<RecipientShardEntry> recipientShards;
        recipientShards.push_back({recipientShardId, recipientDoc.getMutableState()});
        coordinatorDoc.setRecipientShards(recipientShards);

        return coordinatorDoc;
    }

    void createSourceCollection(OperationContext* opCtx,
                                const ReshardingRecipientDocument& recipientDoc) {
        CollectionOptions options;
        options.uuid = recipientDoc.getSourceUUID();
        resharding::data_copy::ensureCollectionDropped(opCtx, recipientDoc.getSourceNss());
        resharding::data_copy::ensureCollectionExists(opCtx, recipientDoc.getSourceNss(), options);
    }

    void createTempReshardingCollection(OperationContext* opCtx,
                                        const ReshardingRecipientDocument& recipientDoc) {
        CollectionOptions options;
        options.uuid = recipientDoc.getReshardingUUID();
        resharding::data_copy::ensureCollectionDropped(opCtx, recipientDoc.getTempReshardingNss());
        resharding::data_copy::ensureCollectionExists(
            opCtx, recipientDoc.getTempReshardingNss(), options);
    }

    SemiFuture<void> notifyToStartCloningUsingCmd(const CancellationToken& cancelToken,
                                                  RecipientStateMachine& recipient,
                                                  const ReshardingRecipientDocument& recipientDoc) {
        auto recipientFields = _makeRecipientFields(recipientDoc);
        return recipient.fulfillAllDonorsPreparedToDonate(
            {recipientFields.getCloneTimestamp().get(),
             recipientFields.getApproxDocumentsToCopy().get(),
             recipientFields.getApproxBytesToCopy().get(),
             recipientFields.getDonorShards()},
            cancelToken);
    }

    void notifyToStartCloning(OperationContext* opCtx,
                              RecipientStateMachine& recipient,
                              const ReshardingRecipientDocument& recipientDoc) {
        auto driveCloneNoRefresh =
            resharding::gFeatureFlagReshardingCloneNoRefresh.isEnabledAndIgnoreFCVUnsafe();

        if (driveCloneNoRefresh) {
            CancellationSource cancelSource;
            SemiFuture<void> future =
                notifyToStartCloningUsingCmd(cancelSource.token(), recipient, recipientDoc);
            // There is a race here where the recipient can fulfill the future before cancelSource
            // is canceled. Due to this we need to check for Status::OK() as well as
            // CallbackCanceled.
            cancelSource.cancel();
            auto status = future.getNoThrow();
            ASSERT_TRUE(status == ErrorCodes::CallbackCanceled || status == Status::OK()) << status;
        } else {
            _onReshardingFieldsChanges(
                opCtx, recipient, recipientDoc, CoordinatorStateEnum::kCloning);
        }
    }

    void notifyReshardingCommitting(OperationContext* opCtx,
                                    RecipientStateMachine& recipient,
                                    const ReshardingRecipientDocument& recipientDoc) {
        _onReshardingFieldsChanges(
            opCtx, recipient, recipientDoc, CoordinatorStateEnum::kCommitting);
    }

    void awaitChangeStreamsMonitorStarted(OperationContext* opCtx,
                                          RecipientStateMachine& recipient,
                                          const ReshardingRecipientDocument& recipientDoc) {
        auto status = recipient.awaitChangeStreamsMonitorStartedForTest().getNoThrow(opCtx);
        if (recipientDoc.getPerformVerification() && !recipientDoc.getSkipCloningAndApplying()) {
            ASSERT_OK(status);
        } else {
            ASSERT_EQ(status, ErrorCodes::IllegalOperation);
        }
        if (!_noChunksToCopy) {
            // Only mock writes during the 'applying' state if the recipient has chunks to copy.
            writeToCollection(opCtx, recipientDoc, _numInserts, _numDeletes, _numUpdates);
        }
    }

    void awaitChangeStreamsMonitorCompleted(OperationContext* opCtx,
                                            RecipientStateMachine& recipient,
                                            const ReshardingRecipientDocument& recipientDoc) {
        auto swDocumentsDelta =
            recipient.awaitChangeStreamsMonitorCompletedForTest().getNoThrow(opCtx);
        if (recipientDoc.getPerformVerification() && !recipientDoc.getSkipCloningAndApplying()) {
            ASSERT_OK(swDocumentsDelta.getStatus());

            // Verify the delta.
            int documentsDelta = getExpectedDocumentsDelta();
            ASSERT_EQ(swDocumentsDelta.getValue(), documentsDelta);

            auto persistedDoc =
                getPersistedRecipientDocument(opCtx, recipientDoc.getReshardingUUID());
            auto changeStreamsMonitorCtx = persistedDoc.getChangeStreamsMonitor();
            ASSERT(changeStreamsMonitorCtx);
            ASSERT_EQ(changeStreamsMonitorCtx->getDocumentsDelta(), documentsDelta);
        } else {
            ASSERT_EQ(swDocumentsDelta.getStatus(), ErrorCodes::IllegalOperation);
        }
    }

    void checkRecipientDocumentRemoved(OperationContext* opCtx) {
        auto recipientColl = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(NamespaceString::kRecipientReshardingOperationsNamespace,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        ASSERT_TRUE(recipientColl.exists());
        ASSERT_TRUE(recipientColl.getCollectionPtr()->isEmpty(opCtx));
    }

protected:
    void setNoChunksToCopy(const TestOptions& testOptions) {
        _noChunksToCopy = testOptions.noChunksToCopy;
    }

    void insertDocuments(OperationContext* opCtx,
                         const NamespaceString nss,
                         const std::vector<BSONObj>& docs) {
        resharding::data_copy::ensureCollectionExists(opCtx, nss, CollectionOptions());
        std::vector<InsertStatement> inserts;
        for (const auto& doc : docs) {
            inserts.emplace_back(doc);
        }
        resharding::data_copy::insertBatch(opCtx, nss, inserts);
    }

    void mockCollectionClonerStateForDonor(
        OperationContext* opCtx,
        const TestOptions testOptions,
        const RecipientMetricsForDonor& testRecipientMetricsForDonor,
        const CommonReshardingMetadata& metadata,
        ReshardingMetrics& metrics) {
        auto docsCopied = testRecipientMetricsForDonor.metrics.docsCopied;
        auto bytesCopied = testRecipientMetricsForDonor.metrics.bytesCopied;

        if (docsCopied == 0) {
            ASSERT_EQ(bytesCopied, 0);
            return;
        }
        ASSERT_GT(bytesCopied, 0);

        bool storeCollectionClonerProgress = testOptions.performVerification;
        if (storeCollectionClonerProgress) {
            // Set up the cloner resume data collection. To verify that the metrics are restored
            // from the counts in this collection instead of the fast count of documents the
            // temporary collection, do not insert any documents to the temporary collection.
            ReshardingRecipientResumeDataId resumeDataId(
                {metadata.getReshardingUUID(), testRecipientMetricsForDonor.shardId});
            ReshardingRecipientResumeData resumeDataDoc(resumeDataId);
            resumeDataDoc.setDocumentsCopied(docsCopied);
            resumeDataDoc.setBytesCopied(bytesCopied);
            insertDocuments(opCtx,
                            NamespaceString::kRecipientReshardingResumeDataNamespace,
                            {resumeDataDoc.toBSON()});
        } else {
            // Set up the temporary collection.
            std::vector<BSONObj> docs;
            for (int i = 0; i < docsCopied; i++) {
                docs.push_back(makeTestDocumentForInsert(i));
            }
            insertDocuments(opCtx, metadata.getTempReshardingNss(), docs);
        }
        metrics.onDocumentsProcessed(docsCopied, bytesCopied, Milliseconds(1));
    }

    std::vector<BSONObj> makeFetchedOplogEntryDocuments(long num) {
        std::vector<BSONObj> oplogEntries;

        Timestamp timestamp{1, 1};
        for (auto i = 0; i < num; i++) {
            // Only mock the necessary fields.
            ReshardingDonorOplogId oplogId{timestamp, timestamp};
            oplogEntries.push_back(BSON("_id" << oplogId.toBSON()));
            timestamp = timestamp + 1ULL;
        }
        return oplogEntries;
    }

    void mockOplogFetcherAndApplierStateForDonor(
        OperationContext* opCtx,
        const TestOptions testOptions,
        const RecipientMetricsForDonor& testRecipientMetricsForDonor,
        const CommonReshardingMetadata& metadata,
        boost::optional<ReshardingMetrics&> metrics = boost::none) {
        auto oplogEntriesFetched = testRecipientMetricsForDonor.metrics.oplogFetched;
        auto oplogEntriesApplied = testRecipientMetricsForDonor.metrics.oplogApplied;
        ASSERT_EQ(oplogEntriesFetched, oplogEntriesApplied);

        if (oplogEntriesApplied == 0) {
            return;
        }

        auto fetchedOplogEntries = makeFetchedOplogEntryDocuments(oplogEntriesApplied);

        if (testOptions.storeOplogFetcherProgress) {
            // Set up the fetcher progress collection. To verify that the metrics are restored
            // from the counts in this collection instead of the fast count of documents the oplog
            // buffer collection, do not insert any documents to the buffer collection.
            ReshardingOplogFetcherProgress fetcherProgressDoc(
                {metadata.getReshardingUUID(), testRecipientMetricsForDonor.shardId},
                oplogEntriesApplied);
            insertDocuments(opCtx,
                            NamespaceString::kReshardingFetcherProgressNamespace,
                            {fetcherProgressDoc.toBSON()});
        } else {
            // Set up the oplog buffer collection.
            insertDocuments(opCtx,
                            resharding::getLocalOplogBufferNamespace(
                                metadata.getSourceUUID(), testRecipientMetricsForDonor.shardId),
                            fetchedOplogEntries);
        }

        // Set up the applier progress collection.
        auto donorOplogId =
            ReshardingDonorOplogId::parse(fetchedOplogEntries.back()["_id"].Obj(),
                                          IDLParserContext("ReshardingRecipientServiceTest"));
        ReshardingOplogApplierProgress applierProgressDoc(
            {metadata.getReshardingUUID(), testRecipientMetricsForDonor.shardId},
            donorOplogId,
            oplogEntriesApplied);
        insertDocuments(opCtx,
                        NamespaceString::kReshardingApplierProgressNamespace,
                        {applierProgressDoc.toBSON()});

        if (metrics) {
            metrics->onOplogEntriesFetched(oplogEntriesFetched);
            metrics->onOplogEntriesApplied(oplogEntriesApplied);
        }
    }

    void mockDataReplicationStateIfNeeded(OperationContext* opCtx,
                                          const TestOptions testOptions,
                                          const TestRecipientMetrics& testRecipientMetrics,
                                          const CommonReshardingMetadata& metadata,
                                          RecipientStateEnum prevState,
                                          ReshardingMetrics& metrics) {
        if (testOptions.skipCloningAndApplying) {
            return;
        }

        if (prevState == RecipientStateEnum::kCloning) {
            for (const auto& testMetricsForDonor : testRecipientMetrics.getMetricsForDonors()) {
                mockCollectionClonerStateForDonor(
                    opCtx, testOptions, testMetricsForDonor, metadata, metrics);
            }
        } else if (prevState == RecipientStateEnum::kApplying) {
            for (const auto& testMetricsForDonor : testRecipientMetrics.getMetricsForDonors()) {
                mockOplogFetcherAndApplierStateForDonor(
                    opCtx, testOptions, testMetricsForDonor, metadata, metrics);
            }
        }
    }

    ChangeStreamsMonitorContext createChangeStreamsMonitorContext(OperationContext* opCtx) {
        // Perform an insert to ensure the change streams monitor has a valid startAt timestamp.
        auto testNss = NamespaceString::createNamespaceString_forTest("testDb", "testColl");
        resharding::data_copy::ensureCollectionExists(opCtx, testNss, CollectionOptions());
        insertDocuments(opCtx, testNss, {makeTestDocumentForInsert(0)});

        WriteUnitOfWork wuow(opCtx);
        auto ts = repl::getNextOpTime(opCtx).getTimestamp();
        wuow.commit();

        ChangeStreamsMonitorContext changeStreams;
        changeStreams.setStartAtOperationTime(ts - 1);
        changeStreams.setDocumentsDelta(0);
        return changeStreams;
    }

    int64_t getExpectedDocumentsDelta() {
        if (_noChunksToCopy) {
            return 0;
        }
        return _numInserts - _numDeletes;
    }

    int64_t getExpectedDocumentsDeltaBytes() {
        if (_noChunksToCopy) {
            return 0;
        }
        auto objectSize = makeTestDocumentForInsert(0).objsize();
        return objectSize * _numInserts - objectSize * _numDeletes;
    }

    void checkRecipientToCopyMetrics(const TestOptions& testOptions,
                                     const ReshardingRecipientDocument& recipientDoc,
                                     const boost::optional<BSONObj>& currOp) {
        auto state = recipientDoc.getMutableState().getState();

        if (state <= RecipientStateEnum::kCloning) {
            return;
        }

        auto expectedApproxDocsToCopy = testOptions.noChunksToCopy ? 0 : approxDocumentsToCopy;
        auto expectedApproxBytesToCopy = testOptions.noChunksToCopy ? 0 : approxBytesToCopy;

        auto driveCloneNoRefresh =
            resharding::gFeatureFlagReshardingCloneNoRefresh.isEnabledAndIgnoreFCVUnsafe();
        if (driveCloneNoRefresh) {
            // Unit tests rely on resharding fields to set the copy metrics. With no refresh
            // enabled, the coordinator sends copy metrics according to the chunk manager, which
            // ignores the arbitary noChunksToCopy value set in unit tests.
            expectedApproxDocsToCopy = approxDocumentsToCopy;
            expectedApproxBytesToCopy = approxBytesToCopy;
        }

        ASSERT_EQ(*recipientDoc.getMetrics()->getApproxDocumentsToCopy(), expectedApproxDocsToCopy);
        ASSERT_EQ(*recipientDoc.getMetrics()->getApproxBytesToCopy(), expectedApproxBytesToCopy);

        if (currOp) {
            ASSERT_EQ(currOp->getField("approxDocumentsToCopy").numberLong(),
                      expectedApproxDocsToCopy);
            ASSERT_EQ(currOp->getField("approxBytesToCopy").numberLong(),
                      expectedApproxBytesToCopy);
        }
    }

    void checkRecipientDocumentMetrics(const TestOptions& testOptions,
                                       const TestRecipientMetrics& testRecipientMetrics,
                                       const ReshardingRecipientDocument& recipientDoc,
                                       const boost::optional<BSONObj>& currOp) {
        auto mutableState = recipientDoc.getMutableState();
        auto state = mutableState.getState();

        if (state < RecipientStateEnum::kApplying) {
            return;
        }

        auto expectedDocsCopied = testRecipientMetrics.getMetricsTotal().docsCopied;
        auto expectedBytesCopied = testRecipientMetrics.getMetricsTotal().bytesCopied;

        ASSERT_EQ(*recipientDoc.getMetrics()->getFinalDocumentsCopiedCount(), expectedDocsCopied);
        ASSERT_EQ(*recipientDoc.getMetrics()->getFinalBytesCopiedCount(), expectedBytesCopied);

        // There are separate metrics in the recipient doc that are shared with the coordinator doc
        // (via RecipientShardEntry/ReshardingRecipientContext).

        // The metrics below are populated upon transitioning to the "done" state.
        if (state == RecipientStateEnum::kDone) {
            // There is currently no 'documentsCopied'.
            ASSERT_EQ(*mutableState.getBytesCopied(), expectedBytesCopied);
        } else {
            ASSERT_FALSE(mutableState.getBytesCopied());
        }

        // If verification is enabled, the metrics below are populated upon transitioning to the
        // "applying" state after cloning completes and are updated as oplog entries are applied. If
        // verification is disabled, they are populated upon transitioning to the "done" state.
        if (testOptions.performVerification &&
            (state == RecipientStateEnum::kApplying ||
             state == RecipientStateEnum::kStrictConsistency)) {
            ASSERT_EQ(*mutableState.getTotalNumDocuments(), expectedDocsCopied);
            ASSERT_EQ(*mutableState.getTotalDocumentSize(), expectedBytesCopied);
        } else if (state == RecipientStateEnum::kDone) {
            ASSERT_EQ(*mutableState.getTotalNumDocuments(),
                      expectedDocsCopied + getExpectedDocumentsDelta());
            // Upon transitioning to the "done" state, 'totalDocumentSize' is set to the fast count
            // size. This test deliberately leaves the temporary collection empty when testing
            // verification so this is expected to be equal to the delta.
            ASSERT_EQ(*mutableState.getTotalDocumentSize(),
                      testOptions.performVerification
                          ? getExpectedDocumentsDeltaBytes()
                          : expectedBytesCopied + getExpectedDocumentsDeltaBytes());
        } else {
            ASSERT_FALSE(mutableState.getTotalNumDocuments());
            ASSERT_FALSE(mutableState.getTotalDocumentSize());
        }

        if (currOp) {
            ASSERT_EQ(currOp->getField("documentsCopied").numberLong(), expectedDocsCopied);
            ASSERT_EQ(currOp->getField("bytesCopied").numberLong(), expectedBytesCopied);
        }
    }

    void checkRecipientOplogMetrics(const TestRecipientMetrics& testRecipientMetrics,
                                    const ReshardingRecipientDocument& recipientDoc,
                                    const boost::optional<BSONObj>& currOp) {
        auto mutableState = recipientDoc.getMutableState();
        auto state = mutableState.getState();

        if (state < RecipientStateEnum::kStrictConsistency) {
            return;
        }

        auto expectedOplogFetched = testRecipientMetrics.getMetricsTotal().oplogFetched;
        auto expectedOplogApplied = testRecipientMetrics.getMetricsTotal().oplogApplied;

        // The oplog metrics in the recipient doc (in ReshardingRecipientContext) are populated upon
        // transitioning to the "done" state.
        if (state == RecipientStateEnum::kDone) {
            ASSERT_EQ(*mutableState.getOplogFetched(), expectedOplogFetched);
            ASSERT_EQ(*mutableState.getOplogApplied(), expectedOplogApplied);
        } else {
            ASSERT_FALSE(mutableState.getOplogFetched());
            ASSERT_FALSE(mutableState.getOplogApplied());
        }

        if (currOp) {
            ASSERT_EQ(currOp->getField("oplogEntriesFetched").numberLong(), expectedOplogFetched);
            ASSERT_EQ(currOp->getField("oplogEntriesApplied").numberLong(), expectedOplogApplied);
        }
    }

    void checkRecipientMetrics(const TestOptions& testOptions,
                               const TestRecipientMetrics& testRecipientMetrics,
                               const ReshardingRecipientDocument& recipientDoc,
                               const boost::optional<BSONObj>& currOp) {
        checkRecipientToCopyMetrics(testOptions, recipientDoc, currOp);
        checkRecipientDocumentMetrics(testOptions, testRecipientMetrics, recipientDoc, currOp);
        checkRecipientOplogMetrics(testRecipientMetrics, recipientDoc, currOp);
    }

    void checkCoordinatorDocumentMetrics(const TestOptions& testOptions,
                                         const TestRecipientMetrics& testRecipientMetrics,
                                         const ReshardingCoordinatorDocument& coordinatorDoc,
                                         RecipientStateEnum recipientState) {
        if (recipientState < RecipientStateEnum::kApplying) {
            return;
        }

        bool checked = false;
        for (const auto& recipientShard : coordinatorDoc.getRecipientShards()) {
            if (recipientShard.getId() == recipientShardId) {
                auto mutableState = recipientShard.getMutableState();
                auto state = mutableState.getState();

                auto expectedDocsCopied = testRecipientMetrics.getMetricsTotal().docsCopied;
                auto expectedBytesCopied = testRecipientMetrics.getMetricsTotal().bytesCopied;

                // The metrics below are populated upon transitioning to the "strict-consistency"
                // state.
                if (state >= RecipientStateEnum::kStrictConsistency) {
                    // There is currently no 'documentsCopied'.
                    ASSERT_EQ(*mutableState.getBytesCopied(), expectedBytesCopied);
                } else {
                    ASSERT_FALSE(mutableState.getBytesCopied());
                }

                // If verification is enabled, the metrics below are populated upon transitioning to
                // the "applying" state after cloning completes and are updated upon transitioning
                // to the "strict-consistency" state. If verification is disabled, they are
                // populated upon transitioning to the "strict-consistency" state.
                if (testOptions.performVerification && state == RecipientStateEnum::kApplying) {
                    ASSERT_EQ(*mutableState.getTotalNumDocuments(), expectedDocsCopied);
                    ASSERT_EQ(*mutableState.getTotalDocumentSize(), expectedBytesCopied);
                } else if (state >= RecipientStateEnum::kStrictConsistency) {
                    ASSERT_EQ(*mutableState.getTotalNumDocuments(),
                              expectedDocsCopied + getExpectedDocumentsDelta());
                    // Upon transitioning to the "strict-consistency" state, 'totalDocumentSize' is
                    // set to the fast count size. This test deliberately leaves the temporary
                    // collection empty when testing verification so this is expected to be equal to
                    // the delta.
                    ASSERT_EQ(*mutableState.getTotalDocumentSize(),
                              testOptions.performVerification
                                  ? getExpectedDocumentsDeltaBytes()
                                  : expectedBytesCopied + getExpectedDocumentsDeltaBytes());
                } else {
                    ASSERT_FALSE(mutableState.getTotalNumDocuments());
                    ASSERT_FALSE(mutableState.getTotalDocumentSize());
                }
                checked = true;
                break;
            }
        }
        ASSERT(checked);
    }

    void checkCoordinatorOplogMetrics(const TestRecipientMetrics& testRecipientMetrics,
                                      const ReshardingCoordinatorDocument& coordinatorDoc,
                                      RecipientStateEnum recipientState) {
        if (recipientState < RecipientStateEnum::kStrictConsistency) {
            return;
        }

        auto expectedOplogFetched = testRecipientMetrics.getMetricsTotal().oplogFetched;
        auto expectedOplogApplied = testRecipientMetrics.getMetricsTotal().oplogApplied;

        bool checked = false;
        for (const auto& recipientShard : coordinatorDoc.getRecipientShards()) {
            if (recipientShard.getId() == recipientShardId) {
                auto mutableState = recipientShard.getMutableState();
                ASSERT_EQ(*mutableState.getOplogFetched(), expectedOplogFetched);
                ASSERT_EQ(*mutableState.getOplogApplied(), expectedOplogApplied);

                checked = true;
                break;
            }
        }
        ASSERT(checked);
    }

    void checkCoordinatorMetrics(const TestOptions& testOptions,
                                 const TestRecipientMetrics& testRecipientMetrics,
                                 const ReshardingCoordinatorDocument& coordinatorDoc,
                                 RecipientStateEnum recipientState) {
        checkCoordinatorDocumentMetrics(
            testOptions, testRecipientMetrics, coordinatorDoc, recipientState);
        checkCoordinatorOplogMetrics(testRecipientMetrics, coordinatorDoc, recipientState);
    }

    void writeToCollection(OperationContext* opCtx,
                           const ReshardingRecipientDocument& recipientDoc,
                           int numInserts,
                           int numDeletes,
                           int numUpdates) {
        resharding::data_copy::ensureCollectionExists(
            opCtx, recipientDoc.getTempReshardingNss(), CollectionOptions());
        ASSERT(numInserts >= numUpdates);
        ASSERT(numInserts >= numDeletes);

        DBDirectClient client(opCtx);

        std::vector<UUID> idsInserted;
        idsInserted.reserve(numInserts);

        for (int i = 0; i <= numInserts; i++) {
            auto id = UUID::gen();
            auto doc = makeTestDocumentForInsert(i, id);
            client.insert(recipientDoc.getTempReshardingNss(), doc);
            idsInserted.emplace_back(id);
        }

        for (int i = 0; i <= numUpdates; i++) {
            client.update(recipientDoc.getTempReshardingNss(),
                          BSON("_id" << idsInserted[i].toBSON()),
                          makeTestDocumentUpdateStatement(),
                          false,
                          false);
        }

        for (int i = 0; i <= numDeletes; i++) {
            client.remove(
                recipientDoc.getTempReshardingNss(), BSON("_id" << idsInserted[i].toBSON()), false);
        }
    }

    void runUnrecoverableErrorTest(const TestOptions& testOptions,
                                   RecipientStateEnum state,
                                   ExternalStateForTestImpl::ExternalFunction errorFunction) {
        externalState()->throwUnrecoverableErrorIn(state, errorFunction);

        auto opCtx = makeOperationContext();
        auto doc = makeRecipientDocument(testOptions);
        if (testOptions.isAlsoDonor) {
            createSourceCollection(opCtx.get(), doc);
        }

        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
        notifyToStartCloning(opCtx.get(), *recipient, doc);

        ASSERT_OK(recipient->awaitInStrictConsistencyOrError().getNoThrow());

        auto persistedDoc = getPersistedRecipientDocument(opCtx.get(), doc.getReshardingUUID());
        if ((state == RecipientStateEnum::kCloning || state == RecipientStateEnum::kApplying) &&
            testOptions.skipCloningAndApplying) {
            ASSERT_EQ(persistedDoc.getMutableState().getState(),
                      RecipientStateEnum::kStrictConsistency);
        } else {
            ASSERT_EQ(persistedDoc.getMutableState().getState(), RecipientStateEnum::kError);
            auto abortReason = persistedDoc.getMutableState().getAbortReason();
            ASSERT(abortReason);
            ASSERT_EQ(abortReason->getIntField("code"), ErrorCodes::InternalError);
        }

        recipient->abort(false);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());
    }

private:
    TypeCollectionRecipientFields _makeRecipientFields(
        const ReshardingRecipientDocument& recipientDoc) {
        TypeCollectionRecipientFields recipientFields{
            recipientDoc.getDonorShards(),
            recipientDoc.getSourceUUID(),
            recipientDoc.getSourceNss(),
            recipientDoc.getMinimumOperationDurationMillis()};

        auto donorShards = recipientFields.getDonorShards();
        for (unsigned i = 0; i < donorShards.size(); ++i) {
            auto minFetchTimestamp = Timestamp{10 + i, i};
            donorShards[i].setMinFetchTimestamp(minFetchTimestamp);
            recipientFields.setCloneTimestamp(minFetchTimestamp);
        }
        recipientFields.setDonorShards(std::move(donorShards));

        ReshardingApproxCopySize approxCopySize;
        approxCopySize.setApproxBytesToCopy(approxBytesToCopy);
        approxCopySize.setApproxDocumentsToCopy(approxDocumentsToCopy);
        recipientFields.setReshardingApproxCopySizeStruct(std::move(approxCopySize));

        return recipientFields;
    }

    void _onReshardingFieldsChanges(OperationContext* opCtx,
                                    RecipientStateMachine& recipient,
                                    const ReshardingRecipientDocument& recipientDoc,
                                    CoordinatorStateEnum coordinatorState) {
        auto reshardingFields = TypeCollectionReshardingFields{recipientDoc.getReshardingUUID()};
        reshardingFields.setRecipientFields(_makeRecipientFields(recipientDoc));
        reshardingFields.setState(coordinatorState);
        bool noChunksToCopy = recipientDoc.getSkipCloningAndApplying().value_or(false) ||
            _noChunksToCopy.value_or(false);
        recipient.onReshardingFieldsChanges(opCtx, reshardingFields, noChunksToCopy);
    }

    std::shared_ptr<RecipientStateTransitionController> _controller;
    std::shared_ptr<ExternalStateForTestImpl> _externalStateImpl;

    boost::optional<bool> _noChunksToCopy;

    // The number of default writes.
    const int64_t _numInserts = 5;
    const int64_t _numUpdates = 1;
    const int64_t _numDeletes = 2;

    // Set the batch size 1 to test multi-batch processing in unit tests with multiple events.
    RAIIServerParameterControllerForTest _batchSize{
        "reshardingVerificationChangeStreamsEventsBatchSizeLimit", 1};
};

TEST_F(ReshardingRecipientServiceTest, CanTransitionThroughEachStateToCompletion) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(5551105,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        auto removeRecipientDocFailpoint =
            globalFailPointRegistry().find("removeRecipientDocFailpoint");
        auto timesEnteredFailPoint = removeRecipientDocFailpoint->setMode(FailPoint::alwaysOn);
        auto doc = makeRecipientDocument(testOptions);
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        PauseDuringStateTransitions stateTransitionsGuard{controller(),
                                                          RecipientStateEnum::kStrictConsistency};

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        stateTransitionsGuard.wait(RecipientStateEnum::kStrictConsistency);
        awaitChangeStreamsMonitorStarted(opCtx.get(), *recipient, doc);
        stateTransitionsGuard.unset(RecipientStateEnum::kStrictConsistency);

        awaitChangeStreamsMonitorCompleted(opCtx.get(), *recipient, doc);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);

        removeRecipientDocFailpoint->waitForTimesEntered(timesEnteredFailPoint + 1);

        // Search metrics in the state document and verify they are valid and the same as the
        // ones in memory.
        auto persistedDoc = getPersistedRecipientDocument(opCtx.get(), doc.getReshardingUUID());

        Date_t copyBegin = recipient->getMetrics()
                               .getStartFor(ReshardingMetrics::TimedPhase::kCloning)
                               .value_or(Date_t::min());
        Date_t copyEnd = recipient->getMetrics()
                             .getEndFor(ReshardingMetrics::TimedPhase::kCloning)
                             .value_or(Date_t::min());
        Date_t buildIndexBegin = recipient->getMetrics()
                                     .getStartFor(ReshardingMetrics::TimedPhase::kBuildingIndex)
                                     .value_or(Date_t::min());
        Date_t buildIndexEnd = recipient->getMetrics()
                                   .getEndFor(ReshardingMetrics::TimedPhase::kBuildingIndex)
                                   .value_or(Date_t::min());
        Date_t applyBegin = recipient->getMetrics()
                                .getStartFor(ReshardingMetrics::TimedPhase::kApplying)
                                .value_or(Date_t::min());
        Date_t applyEnd = recipient->getMetrics()
                              .getEndFor(ReshardingMetrics::TimedPhase::kApplying)
                              .value_or(Date_t::min());

        auto copyBeginDoc = persistedDoc.getMetrics()->getDocumentCopy()->getStart();
        auto copyEndDoc = persistedDoc.getMetrics()->getDocumentCopy()->getStop();
        auto buildIndexBeginDoc = persistedDoc.getMetrics()->getIndexBuildTime()->getStart();
        auto buildIndexEndDoc = persistedDoc.getMetrics()->getIndexBuildTime()->getStop();
        auto applyBeginDoc = persistedDoc.getMetrics()->getOplogApplication()->getStart();
        auto applyEndDoc = persistedDoc.getMetrics()->getOplogApplication()->getStop();

        ASSERT_NE(copyBegin, Date_t::min());
        ASSERT_NE(copyEnd, Date_t::min());
        ASSERT_NE(buildIndexBegin, Date_t::min());
        ASSERT_NE(buildIndexEnd, Date_t::min());
        ASSERT_NE(applyBegin, Date_t::min());
        ASSERT_NE(applyEnd, Date_t::min());
        ASSERT_LTE(copyBegin, copyEnd);
        ASSERT_LTE(buildIndexBegin, buildIndexEnd);
        ASSERT_LTE(applyBegin, applyEnd);

        ASSERT_TRUE(copyBeginDoc.has_value());
        ASSERT_EQ(copyBegin, copyBeginDoc.get());

        ASSERT_TRUE(copyEndDoc.has_value());
        ASSERT_EQ(copyEnd, copyEndDoc.get());

        ASSERT_TRUE(buildIndexBeginDoc.has_value());
        ASSERT_EQ(buildIndexBegin, buildIndexBeginDoc.get());

        ASSERT_TRUE(buildIndexEndDoc.has_value());
        ASSERT_EQ(buildIndexEnd, buildIndexEndDoc.get());

        ASSERT_TRUE(applyBeginDoc.has_value());
        ASSERT_EQ(applyBegin, applyBeginDoc.get());

        ASSERT_TRUE(applyEndDoc.has_value());
        ASSERT_EQ(applyEnd, applyEndDoc.get());

        removeRecipientDocFailpoint->setMode(FailPoint::off);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, StepDownStepUpEachTransition) {
    const std::vector<RecipientStateEnum> recipientStates{RecipientStateEnum::kCreatingCollection,
                                                          RecipientStateEnum::kCloning,
                                                          RecipientStateEnum::kBuildingIndex,
                                                          RecipientStateEnum::kApplying,
                                                          RecipientStateEnum::kStrictConsistency,
                                                          RecipientStateEnum::kDone};
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(5551106,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        PauseDuringStateTransitions stateTransitionsGuard{controller(), recipientStates};
        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
        auto opCtx = makeOperationContext();
        auto prevState = RecipientStateEnum::kUnused;

        for (const auto state : recipientStates) {

            auto recipient = [&] {
                if (prevState == RecipientStateEnum::kUnused) {
                    if (testOptions.isAlsoDonor) {
                        createSourceCollection(opCtx.get(), doc);
                    }

                    RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
                    return RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
                } else {
                    auto [maybeRecipient, isPausedOrShutdown] =
                        RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
                    ASSERT_TRUE(maybeRecipient);
                    ASSERT_FALSE(isPausedOrShutdown);

                    // Allow the transition to prevState to succeed on this primary-only service
                    // instance.
                    stateTransitionsGuard.unset(prevState);
                    return *maybeRecipient;
                }
            }();

            if (prevState != RecipientStateEnum::kUnused) {
                // Allow the transition to prevState to succeed on this primary-only service
                // instance.
                stateTransitionsGuard.unset(prevState);
            }

            // Signal the coordinator's earliest state that allows the recipient's transition
            // into 'state' to be valid. This mimics the real system where, upon step up, the
            // new RecipientStateMachine instance gets refreshed with the coordinator's most
            // recent state.
            switch (state) {
                case RecipientStateEnum::kCreatingCollection:
                case RecipientStateEnum::kCloning: {
                    notifyToStartCloning(opCtx.get(), *recipient, doc);
                    break;
                }
                case RecipientStateEnum::kStrictConsistency: {
                    awaitChangeStreamsMonitorStarted(opCtx.get(), *recipient, doc);
                    break;
                }
                case RecipientStateEnum::kDone: {
                    awaitChangeStreamsMonitorCompleted(opCtx.get(), *recipient, doc);
                    notifyReshardingCommitting(opCtx.get(), *recipient, doc);
                    break;
                }
                default:
                    break;
            }

            // Step down before the transition to state can complete.
            stateTransitionsGuard.wait(state);
            stepDown();

            ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
                      ErrorCodes::InterruptedDueToReplStateChange);

            recipient.reset();
            stepUp(opCtx.get());

            if (shouldDataReplicationBeRunningIn(testOptions, prevState)) {
                stateTransitionsGuard.wait(state);
                ASSERT_TRUE(externalState()->isDataReplicationRunning());
            }

            prevState = state;
        }

        // Finally complete the operation and ensure its success.
        auto [maybeRecipient, isPausedOrShutdown] =
            RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeRecipient);
        ASSERT_FALSE(isPausedOrShutdown);

        auto recipient = *maybeRecipient;

        stateTransitionsGuard.unset(RecipientStateEnum::kDone);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, ReportForCurrentOpAfterCompletion) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9297801,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        const auto recipientState = RecipientStateEnum::kCreatingCollection;

        PauseDuringStateTransitions stateTransitionsGuard{controller(), recipientState};
        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
        auto opCtx = makeOperationContext();

        auto recipient = [&] {
            RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
            return RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
        }();

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        // Step down before the transition to state can complete.
        stateTransitionsGuard.wait(recipientState);
        stepDown();
        stateTransitionsGuard.unset(recipientState);

        // At this point, the resharding metrics will have been unregistered from the cumulative
        // metrics
        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
                  ErrorCodes::InterruptedDueToReplStateChange);

        // Now call step up. The old recipient object has not yet been destroyed because we
        // still hold a shared pointer to it ('recipient') - this can happen in production after
        // a failover if a state machine is slow to clean up.
        stepUp(opCtx.get());

        // Assert that the old recipient object will return a currentOp report, because the
        // resharding metrics still exist on the coordinator object itelf.
        ASSERT(recipient->reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
            MongoProcessInterface::CurrentOpSessionsMode::kIncludeIdle));

        // Ensure the new recipient started up successfully (and thus, registered new resharding
        // metrics), despite the "zombie" state machine still existing.
        auto [maybeRecipient, isPausedOrShutdown] =
            RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeRecipient);
        ASSERT_FALSE(isPausedOrShutdown);
        auto newRecipient = *maybeRecipient;
        ASSERT_NE(recipient, newRecipient);

        // No need to finish the resharding op, so we just cancel the op.
        newRecipient->abort(false);
        ASSERT_OK(newRecipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest, OpCtxKilledWhileRestoringMetrics) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(5992701,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        // Initialize recipient.
        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
        auto opCtx = makeOperationContext();
        if (testOptions.isAlsoDonor) {
            createSourceCollection(opCtx.get(), doc);
        }
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        // In order to restore metrics, metrics need to exist in the first place, so put the
        // recipient in the cloning state, then step down.
        PauseDuringStateTransitions stateTransitionsGuard{
            controller(), {RecipientStateEnum::kCloning, RecipientStateEnum::kStrictConsistency}};

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        stateTransitionsGuard.wait(RecipientStateEnum::kCloning);
        stepDown();
        stateTransitionsGuard.unset(RecipientStateEnum::kCloning);
        recipient.reset();

        // Enable failpoint and step up.
        auto fp = globalFailPointRegistry().find("reshardingOpCtxKilledWhileRestoringMetrics");
        fp->setMode(FailPoint::nTimes, 1);
        stepUp(opCtx.get());

        // After the failpoint is disabled, the operation should succeed.
        auto [maybeRecipient, isPausedOrShutdown] =
            RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeRecipient);
        ASSERT_FALSE(isPausedOrShutdown);
        recipient = *maybeRecipient;

        stateTransitionsGuard.wait(RecipientStateEnum::kStrictConsistency);
        awaitChangeStreamsMonitorStarted(opCtx.get(), *recipient, doc);
        stateTransitionsGuard.unset(RecipientStateEnum::kStrictConsistency);

        awaitChangeStreamsMonitorCompleted(opCtx.get(), *recipient, doc);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());
    }
}

DEATH_TEST_REGEX_F(ReshardingRecipientServiceTest, CommitFn, "4457001.*tripwire") {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9297802,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);
        auto doc = makeRecipientDocument(testOptions);
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        ASSERT_THROWS_CODE(
            recipient->commit(), DBException, ErrorCodes::ReshardCollectionInProgress);

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        recipient->awaitInStrictConsistencyOrError().get();
        recipient->commit();

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest, DropsTemporaryReshardingCollectionOnAbort) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(5551107,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        boost::optional<PauseDuringStateTransitions> doneTransitionGuard;
        doneTransitionGuard.emplace(controller(), RecipientStateEnum::kDone);

        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();

        if (testOptions.isAlsoDonor) {
            // If the recipient is also a donor, the original collection should already exist on
            // this shard.
            createSourceCollection(opCtx.get(), doc);
        }

        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        recipient->abort(false);

        doneTransitionGuard->wait(RecipientStateEnum::kDone);
        stepDown();

        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
                  ErrorCodes::InterruptedDueToReplStateChange);

        recipient.reset();
        stepUp(opCtx.get());

        auto [maybeRecipient, isPausedOrShutdown] =
            RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeRecipient);
        ASSERT_FALSE(isPausedOrShutdown);
        recipient = *maybeRecipient;

        doneTransitionGuard.reset();
        recipient->abort(false);

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());

        if (testOptions.isAlsoDonor) {
            // Verify original collection still exists after aborting.
            auto coll =
                acquireCollection(opCtx.get(),
                                  CollectionAcquisitionRequest(
                                      doc.getSourceNss(),
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(opCtx.get()),
                                      AcquisitionPrerequisites::kRead),
                                  MODE_IS);
            ASSERT_TRUE(coll.exists());
            ASSERT_EQ(coll.uuid(), doc.getSourceUUID());
        }

        // Verify the temporary collection no longer exists.
        {
            auto coll =
                acquireCollection(opCtx.get(),
                                  CollectionAcquisitionRequest(
                                      doc.getTempReshardingNss(),
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(opCtx.get()),
                                      AcquisitionPrerequisites::kRead),
                                  MODE_IS);
            ASSERT_FALSE(coll.exists());
        }
    }
}

TEST_F(ReshardingRecipientServiceTest, RenamesTemporaryReshardingCollectionWhenDone) {
    // The temporary collection is renamed by the donor service when the shard is also a donor. Only
    // on non-donor shards will the recipient service rename the temporary collection.
    bool isAlsoDonor = false;

    for (bool skipCloningAndApplying : {false, true}) {
        LOGV2(9297803,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "skipCloningAndApplying"_attr = skipCloningAndApplying);
        PauseDuringStateTransitions stateTransitionsGuard{
            controller(), {RecipientStateEnum::kApplying, RecipientStateEnum::kStrictConsistency}};

        auto doc = makeRecipientDocument({isAlsoDonor, skipCloningAndApplying});
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        // Wait to check the temporary collection has been created.
        stateTransitionsGuard.wait(RecipientStateEnum::kApplying);
        {
            // Check the temporary collection exists but is not yet renamed.
            auto coll =
                acquireCollection(opCtx.get(),
                                  CollectionAcquisitionRequest(
                                      doc.getTempReshardingNss(),
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(opCtx.get()),
                                      AcquisitionPrerequisites::kRead),
                                  MODE_IS);
            ASSERT_TRUE(coll.exists());
            ASSERT_EQ(coll.uuid(), doc.getReshardingUUID());
        }
        stateTransitionsGuard.unset(RecipientStateEnum::kApplying);

        stateTransitionsGuard.wait(RecipientStateEnum::kStrictConsistency);
        awaitChangeStreamsMonitorStarted(opCtx.get(), *recipient, doc);
        stateTransitionsGuard.unset(RecipientStateEnum::kStrictConsistency);

        awaitChangeStreamsMonitorCompleted(opCtx.get(), *recipient, doc);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());

        {
            // Ensure the temporary collection was renamed.
            auto coll =
                acquireCollection(opCtx.get(),
                                  CollectionAcquisitionRequest(
                                      doc.getSourceNss(),
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(opCtx.get()),
                                      AcquisitionPrerequisites::kRead),
                                  MODE_IS);
            ASSERT_TRUE(coll.exists());
            ASSERT_EQ(coll.uuid(), doc.getReshardingUUID());
        }
    }
}

TEST_F(ReshardingRecipientServiceTest, WritesNoopOplogEntryOnReshardDoneCatchUp) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9297804,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        PauseDuringStateTransitions stateTransitionsGuard{
            controller(), {RecipientStateEnum::kStrictConsistency, RecipientStateEnum::kDone}};

        auto doc = makeRecipientDocument(testOptions);
        auto opCtx = makeOperationContext();
        auto rawOpCtx = opCtx.get();
        RecipientStateMachine::insertStateDocument(rawOpCtx, doc);
        auto recipient = RecipientStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());

        notifyToStartCloning(rawOpCtx, *recipient, doc);
        stateTransitionsGuard.wait(RecipientStateEnum::kStrictConsistency);
        awaitChangeStreamsMonitorStarted(opCtx.get(), *recipient, doc);
        stateTransitionsGuard.unset(RecipientStateEnum::kStrictConsistency);

        awaitChangeStreamsMonitorCompleted(opCtx.get(), *recipient, doc);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);

        stateTransitionsGuard.wait(RecipientStateEnum::kDone);

        stepDown();
        stateTransitionsGuard.unset(RecipientStateEnum::kDone);
        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
                  ErrorCodes::InterruptedDueToReplStateChange);

        DBDirectClient client(opCtx.get());
        NamespaceString sourceNss =
            resharding::constructTemporaryReshardingNss(doc.getSourceNss(), doc.getSourceUUID());

        FindCommandRequest findRequest{NamespaceString::kRsOplogNamespace};
        findRequest.setFilter(BSON("ns" << sourceNss.toString_forTest() << "o2.reshardDoneCatchUp"
                                        << BSON("$exists" << true)));
        auto cursor = client.find(std::move(findRequest));

        ASSERT_TRUE(cursor->more()) << "Found no oplog entries for source collection";
        repl::OplogEntry op(cursor->next());
        ASSERT_FALSE(cursor->more())
            << "Found multiple oplog entries for source collection: " << op.getEntry() << " and "
            << cursor->nextSafe();

        ReshardDoneCatchUpChangeEventO2Field expectedChangeEvent{sourceNss,
                                                                 doc.getReshardingUUID()};
        auto receivedChangeEvent = ReshardDoneCatchUpChangeEventO2Field::parse(
            *op.getObject2(), IDLParserContext("ReshardDoneCatchUpChangeEventO2Field"));

        ASSERT_EQ(OpType_serializer(op.getOpType()), OpType_serializer(repl::OpTypeEnum::kNoop))
            << op.getEntry();
        ASSERT_EQ(*op.getUuid(), doc.getReshardingUUID()) << op.getEntry();
        ASSERT_EQ(op.getObject()["msg"].type(), BSONType::string) << op.getEntry();
        ASSERT_TRUE(receivedChangeEvent == expectedChangeEvent);
        ASSERT_TRUE(op.getFromMigrate());
        ASSERT_FALSE(bool(op.getDestinedRecipient())) << op.getEntry();

        stepUp(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, WritesNoopOplogEntryForImplicitShardCollection) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9297805,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);
        PauseDuringStateTransitions stateTransitionsGuard{
            controller(), {RecipientStateEnum::kStrictConsistency, RecipientStateEnum::kDone}};

        auto doc = makeRecipientDocument(testOptions);
        auto opCtx = makeOperationContext();
        auto rawOpCtx = opCtx.get();
        RecipientStateMachine::insertStateDocument(rawOpCtx, doc);
        auto recipient = RecipientStateMachine::getOrCreate(rawOpCtx, _service, doc.toBSON());

        notifyToStartCloning(rawOpCtx, *recipient, doc);

        stateTransitionsGuard.wait(RecipientStateEnum::kStrictConsistency);
        awaitChangeStreamsMonitorStarted(opCtx.get(), *recipient, doc);
        stateTransitionsGuard.unset(RecipientStateEnum::kStrictConsistency);

        awaitChangeStreamsMonitorCompleted(opCtx.get(), *recipient, doc);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);

        stateTransitionsGuard.wait(RecipientStateEnum::kDone);

        stepDown();
        stateTransitionsGuard.unset(RecipientStateEnum::kDone);
        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
                  ErrorCodes::InterruptedDueToReplStateChange);

        DBDirectClient client(opCtx.get());
        NamespaceString sourceNss =
            resharding::constructTemporaryReshardingNss(doc.getSourceNss(), doc.getSourceUUID());

        FindCommandRequest findRequest{NamespaceString::kRsOplogNamespace};
        findRequest.setFilter(BSON("ns" << sourceNss.toString_forTest() << "o2.shardCollection"
                                        << BSON("$exists" << true)));
        auto cursor = client.find(std::move(findRequest));

        ASSERT_TRUE(cursor->more()) << "Found no oplog entries for source collection";
        repl::OplogEntry shardCollectionOp(cursor->next());

        ASSERT_EQ(OpType_serializer(shardCollectionOp.getOpType()),
                  OpType_serializer(repl::OpTypeEnum::kNoop))
            << shardCollectionOp.getEntry();
        ASSERT_EQ(*shardCollectionOp.getUuid(), doc.getReshardingUUID())
            << shardCollectionOp.getEntry();
        ASSERT_EQ(shardCollectionOp.getObject()["msg"].type(), BSONType::object)
            << shardCollectionOp.getEntry();
        ASSERT_FALSE(shardCollectionOp.getFromMigrate());

        auto shardCollEventExpected = BSON("shardCollection" << sourceNss.toString_forTest()
                                                             << "shardKey" << newShardKeyPattern());
        ASSERT_BSONOBJ_EQ(*shardCollectionOp.getObject2(), shardCollEventExpected);

        stepUp(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, TruncatesXLErrorOnRecipientDocument) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(5568600,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        std::string xlErrMsg(6000, 'x');
        FailPointEnableBlock failpoint("reshardingRecipientFailsAfterTransitionToCloning",
                                       BSON("errmsg" << xlErrMsg));

        auto doc = makeRecipientDocument(testOptions);
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        auto localTransitionToErrorFuture = recipient->awaitInStrictConsistencyOrError();
        ASSERT_OK(localTransitionToErrorFuture.getNoThrow());

        // The recipient still waits for the abort decision from the coordinator despite it
        // having errored locally. It is therefore safe to check its local state document until
        // RecipientStateMachine::abort() is called.
        {
            auto persistedDoc = getPersistedRecipientDocument(opCtx.get(), doc.getReshardingUUID());
            auto abortReason = persistedDoc.getMutableState().getAbortReason();
            ASSERT(abortReason);
            // The actual abortReason will be slightly larger than kReshardErrorMaxBytes bytes
            // due to the primitive truncation algorithm - Check that the total size is less
            // than kReshardErrorMaxBytes + a couple additional bytes to provide a buffer for
            // the field name sizes.
            int maxReshardErrorBytesCeiling = resharding::kReshardErrorMaxBytes + 200;
            ASSERT_LT(abortReason->objsize(), maxReshardErrorBytesCeiling);
            ASSERT_EQ(abortReason->getIntField("code"),
                      ErrorCodes::ReshardCollectionTruncatedError);
        }

        recipient->abort(false);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest, SkipCloningAndApplying) {
    // Set the failpoint to force the cloners and fetchers to fail to initialize so that the
    // recipient would fail if it does not skip cloning and applying.
    FailPointEnableBlock fp("failToCreateReshardingDataReplicationForTest");

    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9110903,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);
        auto doc = makeRecipientDocument(testOptions);
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        if (!testOptions.skipCloningAndApplying) {
            ASSERT_OK(recipient->awaitInStrictConsistencyOrError().getNoThrow());

            auto persistedDoc = getPersistedRecipientDocument(opCtx.get(), doc.getReshardingUUID());
            auto abortReason = persistedDoc.getMutableState().getAbortReason();
            ASSERT(abortReason);
            ASSERT_EQ(abortReason->getIntField("code"), ErrorCodes::InternalError);
            continue;
        }

        notifyReshardingCommitting(opCtx.get(), *recipient, doc);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest, MetricsSuccessfullyShutDownOnUserCancelation) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9297806,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);
        auto doc = makeRecipientDocument(testOptions);
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        auto localTransitionToErrorFuture = recipient->awaitInStrictConsistencyOrError();
        ASSERT_OK(localTransitionToErrorFuture.getNoThrow());

        recipient->abort(true);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest, ReshardingMetricsBasic) {
    const std::vector<RecipientStateEnum> recipientStates{RecipientStateEnum::kCreatingCollection,
                                                          RecipientStateEnum::kCloning,
                                                          RecipientStateEnum::kBuildingIndex,
                                                          RecipientStateEnum::kApplying,
                                                          RecipientStateEnum::kStrictConsistency,
                                                          RecipientStateEnum::kDone};

    for (auto& testOptions : makeAllTestOptions()) {
        LOGV2(9297807,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);
        setNoChunksToCopy(testOptions);

        PauseDuringStateTransitions stateTransitionsGuard{controller(), recipientStates};
        auto opCtx = makeOperationContext();

        auto recipientDoc = makeRecipientDocument(testOptions);
        auto instanceId = BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName
                               << recipientDoc.getReshardingUUID());
        auto prevState = RecipientStateEnum::kUnused;

        auto coordinatorDoc = makeCoordinatorDocument(recipientDoc);
        insertDocuments(opCtx.get(),
                        NamespaceString::kConfigReshardingOperationsNamespace,
                        {coordinatorDoc.toBSON()});

        auto testRecipientMetrics =
            makeTestRecipientMetrics(testOptions, recipientDoc.getDonorShards());

        auto checkPersistedState = [&](RecipientStateMachine& recipient) {
            auto persistedRecipientDoc =
                getPersistedRecipientDocument(opCtx.get(), recipientDoc.getReshardingUUID());
            auto persistedCoordinatorDoc =
                getPersistedCoordinatorDocument(opCtx.get(), recipientDoc.getReshardingUUID());
            auto persistedRecipientState = persistedRecipientDoc.getMutableState().getState();

            checkRecipientMetrics(
                testOptions, testRecipientMetrics, persistedRecipientDoc, boost::none /* currOp */);
            checkCoordinatorMetrics(testOptions,
                                    testRecipientMetrics,
                                    persistedCoordinatorDoc,
                                    persistedRecipientState);

            if (persistedRecipientState >= RecipientStateEnum::kCloning) {
                ASSERT(persistedRecipientDoc.getCloneTimestamp());
            }
        };

        auto removeRecipientDocFailpoint =
            globalFailPointRegistry().find("removeRecipientDocFailpoint");
        auto timesEnteredFailPoint = removeRecipientDocFailpoint->setMode(FailPoint::alwaysOn);

        for (const auto state : recipientStates) {
            auto recipient = [&] {
                if (prevState == RecipientStateEnum::kUnused) {
                    RecipientStateMachine::insertStateDocument(opCtx.get(), recipientDoc);
                    return RecipientStateMachine::getOrCreate(
                        opCtx.get(), _service, recipientDoc.toBSON());
                } else {
                    auto [maybeRecipient, isPausedOrShutdown] =
                        RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
                    ASSERT_TRUE(maybeRecipient);
                    ASSERT_FALSE(isPausedOrShutdown);

                    // Allow the transition to prevState to succeed on this primary-only service
                    // instance.
                    stateTransitionsGuard.unset(prevState);
                    return *maybeRecipient;
                }
            }();

            mockDataReplicationStateIfNeeded(opCtx.get(),
                                             testOptions,
                                             testRecipientMetrics,
                                             recipientDoc.getCommonReshardingMetadata(),
                                             prevState,
                                             recipient->getMetricsForTest());

            if (prevState != RecipientStateEnum::kUnused) {
                // Allow the transition to prevState to succeed on this primary-only service
                // instance.
                stateTransitionsGuard.unset(prevState);
            }

            // Signal the coordinator's earliest state that allows the recipient's transition into
            // 'state' to be valid.
            switch (state) {
                case RecipientStateEnum::kCreatingCollection:
                case RecipientStateEnum::kCloning: {
                    notifyToStartCloning(opCtx.get(), *recipient, recipientDoc);
                    break;
                }
                case RecipientStateEnum::kStrictConsistency: {
                    awaitChangeStreamsMonitorStarted(opCtx.get(), *recipient, recipientDoc);
                    break;
                }
                case RecipientStateEnum::kDone: {
                    awaitChangeStreamsMonitorCompleted(opCtx.get(), *recipient, recipientDoc);
                    notifyReshardingCommitting(opCtx.get(), *recipient, recipientDoc);
                    break;
                }
                default:
                    break;
            }

            stateTransitionsGuard.wait(state);
            checkPersistedState(*recipient);

            prevState = state;
        }

        auto [maybeRecipient, isPausedOrShutdown] =
            RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeRecipient);
        ASSERT_FALSE(isPausedOrShutdown);
        auto recipient = *maybeRecipient;

        stateTransitionsGuard.unset(RecipientStateEnum::kDone);
        awaitChangeStreamsMonitorCompleted(opCtx.get(), *recipient, recipientDoc);
        notifyReshardingCommitting(opCtx.get(), *recipient, recipientDoc);

        removeRecipientDocFailpoint->waitForTimesEntered(timesEnteredFailPoint + 1);
        checkPersistedState(*recipient);

        removeRecipientDocFailpoint->setMode(FailPoint::off);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, RestoreMetricsAfterStepUp) {
    const std::vector<RecipientStateEnum> recipientStates{RecipientStateEnum::kCreatingCollection,
                                                          RecipientStateEnum::kCloning,
                                                          RecipientStateEnum::kBuildingIndex,
                                                          RecipientStateEnum::kApplying,
                                                          RecipientStateEnum::kStrictConsistency,
                                                          RecipientStateEnum::kDone};

    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(9297808,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);
        setNoChunksToCopy(testOptions);

        PauseDuringStateTransitions stateTransitionsGuard{controller(), recipientStates};
        auto opCtx = makeOperationContext();

        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
        auto prevState = RecipientStateEnum::kUnused;

        auto testRecipientMetrics = makeTestRecipientMetrics(testOptions, doc.getDonorShards());

        for (const auto state : recipientStates) {
            auto recipient = [&] {
                if (prevState == RecipientStateEnum::kUnused) {
                    RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
                    return RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
                } else {
                    auto [maybeRecipient, isPausedOrShutdown] =
                        RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
                    ASSERT_TRUE(maybeRecipient);
                    ASSERT_FALSE(isPausedOrShutdown);

                    // Allow the transition to prevState to succeed on this primary-only service
                    // instance.
                    stateTransitionsGuard.unset(prevState);
                    return *maybeRecipient;
                }
            }();

            mockDataReplicationStateIfNeeded(opCtx.get(),
                                             testOptions,
                                             testRecipientMetrics,
                                             doc.getCommonReshardingMetadata(),
                                             prevState,
                                             recipient->getMetricsForTest());

            if (prevState != RecipientStateEnum::kUnused) {
                // Allow the transition to prevState to succeed on this primary-only
                // service instance.
                stateTransitionsGuard.unset(prevState);
            }

            // Signal the coordinator's earliest state that allows the recipient's transition into
            // 'state' to be valid. This mimics the real system where, upon step up, the new
            // RecipientStateMachine instance gets refreshed with the coordinator's most recent
            // state.
            switch (state) {
                case RecipientStateEnum::kCreatingCollection:
                case RecipientStateEnum::kCloning: {
                    notifyToStartCloning(opCtx.get(), *recipient, doc);
                    break;
                }
                case RecipientStateEnum::kStrictConsistency: {
                    awaitChangeStreamsMonitorStarted(opCtx.get(), *recipient, doc);
                    break;
                }
                case RecipientStateEnum::kDone: {
                    awaitChangeStreamsMonitorCompleted(opCtx.get(), *recipient, doc);
                    notifyReshardingCommitting(opCtx.get(), *recipient, doc);
                    break;
                }
                default:
                    break;
            }
            // Step down before the transition to state can complete.
            stateTransitionsGuard.wait(state);

            auto persistedDoc = getPersistedRecipientDocument(opCtx.get(), doc.getReshardingUUID());
            auto currOp = recipient
                              ->reportForCurrentOp(
                                  MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
                                  MongoProcessInterface::CurrentOpSessionsMode::kExcludeIdle)
                              .value();

            ASSERT_EQ(currOp.getStringField("recipientState"),
                      RecipientState_serializer(persistedDoc.getMutableState().getState()));
            checkRecipientMetrics(testOptions, testRecipientMetrics, persistedDoc, currOp);

            stepDown();

            ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
                      ErrorCodes::InterruptedDueToReplStateChange);

            prevState = state;
            if (state == RecipientStateEnum::kApplying ||
                state == RecipientStateEnum::kStrictConsistency) {
                // If metrics are being verified in the next pass, ensure a retry does not alter
                // values.
                auto fp =
                    globalFailPointRegistry().find("reshardingOpCtxKilledWhileRestoringMetrics");
                fp->setMode(FailPoint::nTimes, 1);
            }

            recipient.reset();
            if (state != RecipientStateEnum::kDone) {
                stepUp(opCtx.get());
            }
        }

        stepUp(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, RestoreMetricsAfterStepUpWithMissingProgressDoc) {
    for (const auto& testOptions : makeAllTestOptions()) {
        LOGV2(9297809,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);
        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
        auto opCtx = makeOperationContext();

        auto donorShards = doc.getDonorShards();
        for (unsigned i = 0; i < donorShards.size(); i++) {
            if (i == 0) {
                continue;
            }

            RecipientMetricsForDonor metricsForDonor;
            metricsForDonor.shardId = donorShards[i].getShardId();
            metricsForDonor.metrics.oplogFetched = i;
            metricsForDonor.metrics.oplogApplied = metricsForDonor.metrics.oplogFetched;

            mockOplogFetcherAndApplierStateForDonor(opCtx.get(),
                                                    testOptions,
                                                    std::move(metricsForDonor),
                                                    doc.getCommonReshardingMetadata());
        }

        auto mutableState = doc.getMutableState();
        mutableState.setState(RecipientStateEnum::kApplying);
        mutableState.setTotalNumDocuments(0);  // Needed for performVerification.
        doc.setMutableState(mutableState);
        doc.setCloneTimestamp(Timestamp{10, 0});
        doc.setStartConfigTxnCloneTime(Date_t::now());

        auto metadata = doc.getCommonReshardingMetadata();
        metadata.setStartTime(Date_t::now());
        metadata.setPerformVerification(testOptions.performVerification);
        doc.setCommonReshardingMetadata(metadata);

        if (testOptions.performVerification) {
            doc.setChangeStreamsMonitor(createChangeStreamsMonitorContext(opCtx.get()));
        }

        createTempReshardingCollection(opCtx.get(), doc);

        PauseDuringStateTransitions stateTransitionsGuard{controller(),
                                                          RecipientStateEnum::kStrictConsistency};
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        stateTransitionsGuard.wait(RecipientStateEnum::kStrictConsistency);
        awaitChangeStreamsMonitorStarted(opCtx.get(), *recipient, doc);
        stateTransitionsGuard.unset(RecipientStateEnum::kStrictConsistency);

        awaitChangeStreamsMonitorCompleted(opCtx.get(), *recipient, doc);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest, AbortWhileChangeStreamsMonitorInProgress) {
    auto opCtx = makeOperationContext();
    auto doc = makeRecipientDocument({.isAlsoDonor = false, .performVerification = true});

    auto mutableState = doc.getMutableState();
    mutableState.setState(RecipientStateEnum::kStrictConsistency);
    mutableState.setTotalNumDocuments(0);
    doc.setMutableState(mutableState);
    doc.setCloneTimestamp(Timestamp{10, 0});
    doc.setStartConfigTxnCloneTime(Date_t::now());
    doc.setChangeStreamsMonitor(createChangeStreamsMonitorContext(opCtx.get()));

    createTempReshardingCollection(opCtx.get(), doc);
    RecipientStateMachine::insertStateDocument(opCtx.get(), doc);

    auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
    ASSERT_OK(recipient->awaitChangeStreamsMonitorStartedForTest().getNoThrow());
    recipient->abort(false);

    auto status = recipient->awaitChangeStreamsMonitorCompletedForTest().getNoThrow();
    ASSERT((status == ErrorCodes::CallbackCanceled) || (status == ErrorCodes::Interrupted));
    ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
}

TEST_F(ReshardingRecipientServiceTest, AbortAfterStepUpWithAbortReasonFromCoordinator) {
    repl::primaryOnlyServiceTestStepUpWaitForRebuildComplete.setMode(FailPoint::alwaysOn);
    const auto abortErrMsg = "Recieved abort from the resharding coordinator";

    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(8743301,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        auto removeRecipientDocFailpoint =
            globalFailPointRegistry().find("removeRecipientDocFailpoint");
        auto timesEnteredFailPoint = removeRecipientDocFailpoint->setMode(FailPoint::alwaysOn);

        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();
        if (testOptions.isAlsoDonor) {
            createSourceCollection(opCtx.get(), doc);
        }

        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        recipient->abort(false);
        removeRecipientDocFailpoint->waitForTimesEntered(timesEnteredFailPoint + 1);

        // Ensure the node is aborting with abortReason from coordinator.
        {
            auto persistedDoc = getPersistedRecipientDocument(opCtx.get(), doc.getReshardingUUID());
            auto state = persistedDoc.getMutableState().getState();
            ASSERT_EQ(state, RecipientStateEnum::kDone);

            auto abortReason = persistedDoc.getMutableState().getAbortReason();
            ASSERT(abortReason);
            ASSERT_EQ(abortReason->getIntField("code"), ErrorCodes::ReshardCollectionAborted);
            ASSERT_EQ(abortReason->getStringField("errmsg"), abortErrMsg);
        }

        stepDown();
        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
                  ErrorCodes::InterruptedDueToReplStateChange);
        recipient.reset();

        stepUp(opCtx.get());
        removeRecipientDocFailpoint->waitForTimesEntered(timesEnteredFailPoint + 2);

        auto [maybeRecipient, isPausedOrShutdown] =
            RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeRecipient);
        ASSERT_FALSE(isPausedOrShutdown);
        recipient = *maybeRecipient;

        removeRecipientDocFailpoint->setMode(FailPoint::off);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkRecipientDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, FailoverDuringErrorState) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(8916100,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        std::string errMsg("Simulating an unrecoverable error for testing");
        FailPointEnableBlock failpoint("reshardingRecipientFailsAfterTransitionToCloning",
                                       BSON("errmsg" << errMsg));

        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        auto localTransitionToErrorFuture = recipient->awaitInStrictConsistencyOrError();
        ASSERT_OK(localTransitionToErrorFuture.getNoThrow());

        stepDown();
        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
                  ErrorCodes::InterruptedDueToReplStateChange);
        recipient.reset();

        stepUp(opCtx.get());

        auto [maybeRecipient, isPausedOrShutdown] =
            RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(maybeRecipient);
        ASSERT_FALSE(isPausedOrShutdown);
        recipient = *maybeRecipient;

        {
            auto persistedDoc = getPersistedRecipientDocument(opCtx.get(), doc.getReshardingUUID());
            auto state = persistedDoc.getMutableState().getState();
            ASSERT_EQ(state, RecipientStateEnum::kError);
            ASSERT(persistedDoc.getMutableState().getAbortReason());
        }

        recipient->abort(false);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

// The collection options for the source and temporary collections are both defaulted to BSONObj(),
// so this test will pass since both collection options are equal.
TEST_F(ReshardingRecipientServiceTest, TestVerifyCollectionOptionsHappyPath) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagReshardingVerification",
                                                               true);
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9799201,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        PauseDuringStateTransitions stateTransitionsGuard{controller(),
                                                          RecipientStateEnum::kStrictConsistency};
        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        stateTransitionsGuard.wait(RecipientStateEnum::kStrictConsistency);
        awaitChangeStreamsMonitorStarted(opCtx.get(), *recipient, doc);
        stateTransitionsGuard.unset(RecipientStateEnum::kStrictConsistency);

        awaitChangeStreamsMonitorCompleted(opCtx.get(), *recipient, doc);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest,
       TestVerifyCollectionOptionsThrowsExceptionOnMismatchedOptions) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagReshardingVerification",
                                                               true);
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9799202,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        // Add dummy data to tempReshardingCollectionOptions to create mismatched collection
        // options.
        tempReshardingCollectionOptions = BSONObjBuilder().append("viewOn", "bar").obj();

        notifyToStartCloning(opCtx.get(), *recipient, doc);

        boost::optional<PauseDuringStateTransitions> stateTransitionsGuard;
        stateTransitionsGuard.emplace(controller(), RecipientStateEnum::kError);

        // Ensure we get to the errored state when we try to match options.
        // If we do not get to an errored state this test should hang here and time out.
        stateTransitionsGuard->wait(RecipientStateEnum::kError);
        stateTransitionsGuard->unset(RecipientStateEnum::kError);

        recipient->abort(false);

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());

        tempReshardingCollectionOptions = BSONObj();
    }
}

// Creates mismatched collection options with the feature flag turned off.
// If the feature was turned on we would catch the mismatched options and throw an exception.
TEST_F(ReshardingRecipientServiceTest,
       TestVerifyCollectionOptionsDoesNotPerformVerificationIfFeatureFlagIsNotSet) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagReshardingVerification",
                                                               false);
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(9799203,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        PauseDuringStateTransitions stateTransitionsGuard{controller(),
                                                          RecipientStateEnum::kStrictConsistency};
        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        // Add dummy data to tempReshardingCollectionOptions to create mismatched collection
        // options.
        tempReshardingCollectionOptions = BSONObjBuilder().append("viewOn", "bar").obj();

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        stateTransitionsGuard.wait(RecipientStateEnum::kStrictConsistency);
        awaitChangeStreamsMonitorStarted(opCtx.get(), *recipient, doc);
        stateTransitionsGuard.unset(RecipientStateEnum::kStrictConsistency);

        awaitChangeStreamsMonitorCompleted(opCtx.get(), *recipient, doc);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());

        tempReshardingCollectionOptions = BSONObj();
    }
}

TEST_F(ReshardingRecipientServiceTest, VerifyRecipientRetriesOnLockTimeoutError) {
    const std::vector<RecipientStateEnum> recipientPhases{RecipientStateEnum::kCreatingCollection,
                                                          RecipientStateEnum::kCloning,
                                                          RecipientStateEnum::kBuildingIndex,
                                                          RecipientStateEnum::kApplying,
                                                          RecipientStateEnum::kStrictConsistency};

    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(10568802,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        PauseDuringStateTransitions phaseTransitionsGuard{controller(), {recipientPhases}};
        auto doc = makeRecipientDocument(testOptions);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        // Start a thread to create randomized lock/unlock contention.
        const ResourceId resId(RESOURCE_COLLECTION, doc.getTempReshardingNss());
        Locker locker(opCtx->getServiceContext());
        AtomicWord<bool> keepRunning{true};
        stdx::thread lockThread(
            [&] { runRandomizedLocking(opCtx.get(), locker, resId, keepRunning); });

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        for (const auto& phase : recipientPhases) {
            phaseTransitionsGuard.wait(phase);
            sleepFor(Milliseconds(100));
            phaseTransitionsGuard.unset(phase);
        }

        // Signal the thread to stop and wait for its completion.
        keepRunning.store(false);
        lockThread.join();

        notifyReshardingCommitting(opCtx.get(), *recipient, doc);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    }
}

TEST_F(ReshardingRecipientServiceTest, UnrecoverableErrorDuringCreatingCollection) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(10494600,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        runUnrecoverableErrorTest(
            testOptions, RecipientStateEnum::kCreatingCollection, kRefreshCatalogCache);
    }
}

TEST_F(ReshardingRecipientServiceTest, UnrecoverableErrorDuringCloning) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(10494601,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        runUnrecoverableErrorTest(testOptions, RecipientStateEnum::kCloning, kMakeDataReplication);
    }
}

TEST_F(ReshardingRecipientServiceTest, UnrecoverableErrorDuringBuildingIndex) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(10494602,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        runUnrecoverableErrorTest(
            testOptions, RecipientStateEnum::kBuildingIndex, kGetCollectionIndexes);
    }
}

TEST_F(ReshardingRecipientServiceTest, UnrecoverableErrorDuringApplying) {
    for (const auto& testOptions : makeBasicTestOptions()) {
        LOGV2(10494603,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "testOptions"_attr = testOptions);

        runUnrecoverableErrorTest(
            testOptions, RecipientStateEnum::kApplying, kEnsureReshardingStashCollectionsEmpty);
    }
}

}  // namespace
}  // namespace mongo
