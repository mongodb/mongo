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

#include "mongo/db/s/resharding/resharding_coordinator_service.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/resharding/resharding_coordinator.h"
#include "mongo/db/s/resharding/resharding_coordinator_observer.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_external_state.h"
#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"
#include "mongo/db/s/resharding/resharding_op_observer.h"
#include "mongo/db/s/resharding/resharding_service_test_helpers.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/executor/mock_async_rpc.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resharding_coordinator_service_conflicting_op_in_progress_info.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <string>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using CoordinatorStateTransitionController =
    resharding_service_test_helpers::StateTransitionController<CoordinatorStateEnum>;
using OpObserverForTest = resharding_service_test_helpers::
    StateTransitionControllerOpObserver<CoordinatorStateEnum, ReshardingCoordinatorDocument>;
using PauseDuringStateTransitions =
    resharding_service_test_helpers::PauseDuringStateTransitions<CoordinatorStateEnum>;

class ExternalStateForTest : public ReshardingCoordinatorExternalState {
public:
    struct Options {
        std::map<ShardId, int64_t> documentsToCopy;
        std::map<ShardId, int64_t> documentsDelta;
        boost::optional<ErrorCodes::Error> getDocumentsToCopyErrorCode;
        boost::optional<ErrorCodes::Error> getDocumentsDeltaErrorCode;
        boost::optional<ErrorCodes::Error> verifyClonedErrorCode;
        boost::optional<ErrorCodes::Error> verifyFinalErrorCode;
    };

    enum class ExternalFunction {
        kTellAllDonorsToRefresh,
        kEstablishAllDonorsAsParticipants,
        kGetDocumentsToCopyFromDonors,
        kGetDocumentsDeltaFromDonors,
    };

    ExternalStateForTest(Options options)
        : ReshardingCoordinatorExternalState(), _options(options) {}

    ParticipantShardsAndChunks calculateParticipantShardsAndChunks(
        OperationContext* opCtx,
        const ReshardingCoordinatorDocument& coordinatorDoc,
        std::vector<ReshardingZoneType> zones) override {
        std::vector<ChunkType> initialChunks;
        auto version = calculateChunkVersionForInitialChunks(opCtx);

        // Use the provided shardIds from presetReshardedChunks to construct the
        // recipient list.
        if (const auto& chunks = coordinatorDoc.getPresetReshardedChunks()) {
            for (const auto& reshardedChunk : *chunks) {
                initialChunks.emplace_back(
                    coordinatorDoc.getReshardingUUID(),
                    ChunkRange{reshardedChunk.getMin(), reshardedChunk.getMax()},
                    version,
                    reshardedChunk.getRecipientShardId());
                version.incMinor();
            }
        }
        return ParticipantShardsAndChunks(
            {coordinatorDoc.getDonorShards(), coordinatorDoc.getRecipientShards(), initialChunks});
    }

    void tellAllDonorsToRefresh(OperationContext* opCtx,
                                const NamespaceString& sourceNss,
                                const UUID& reshardingUUID,
                                const std::vector<mongo::DonorShardEntry>& donorShards,
                                const std::shared_ptr<executor::TaskExecutor>& executor,
                                CancellationToken token) override {
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kTellAllDonorsToRefresh);

        auto donorShardIds = resharding::extractShardIdsFromParticipantEntries(donorShards);
        resharding::sendFlushReshardingStateChangeToShards(
            opCtx, sourceNss, reshardingUUID, donorShardIds, executor, token);
    }

    void tellAllRecipientsToRefresh(OperationContext* opCtx,
                                    const NamespaceString& nssToRefresh,
                                    const UUID& reshardingUUID,
                                    const std::vector<mongo::RecipientShardEntry>& recipientShards,
                                    const std::shared_ptr<executor::TaskExecutor>& executor,
                                    CancellationToken token) override {
        auto recipientShardIds = resharding::extractShardIdsFromParticipantEntries(recipientShards);
        resharding::sendFlushReshardingStateChangeToShards(
            opCtx, nssToRefresh, reshardingUUID, recipientShardIds, executor, token);
    }

    void establishAllDonorsAsParticipants(OperationContext* opCtx,
                                          const NamespaceString& sourceNss,
                                          const std::vector<mongo::DonorShardEntry>& donorShards,
                                          const std::shared_ptr<executor::TaskExecutor>& executor,
                                          CancellationToken token) override {
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kEstablishAllDonorsAsParticipants);

        auto donorShardIds = resharding::extractShardIdsFromParticipantEntries(donorShards);
        resharding::sendFlushRoutingTableCacheUpdatesToShards(
            opCtx, sourceNss, donorShardIds, executor, token);
    }


    void establishAllRecipientsAsParticipants(
        OperationContext* opCtx,
        const NamespaceString& tempNss,
        const std::vector<mongo::RecipientShardEntry>& recipientShards,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token) override {
        auto recipientShardIds = resharding::extractShardIdsFromParticipantEntries(recipientShards);
        resharding::sendFlushRoutingTableCacheUpdatesToShards(
            opCtx, tempNss, recipientShardIds, executor, token);
    }

    std::map<ShardId, int64_t> getDocumentsToCopyFromDonors(
        OperationContext* opCtx,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token,
        const UUID& reshardingUUID,
        const NamespaceString& nss,
        const Timestamp& cloneTimestamp,
        const std::map<ShardId, ShardVersion>& shardVersions) override {
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kGetDocumentsToCopyFromDonors);
        if (_options.getDocumentsToCopyErrorCode) {
            uasserted(*_options.getDocumentsToCopyErrorCode, "Failing call to getDocumentsToCopy.");
        }

        std::map<ShardId, int64_t> docsToCopy;
        for (const auto& [shardId, _] : shardVersions) {
            auto it = _options.documentsToCopy.find(shardId);
            ASSERT(it != _options.documentsToCopy.end());
            docsToCopy.emplace(shardId, it->second);
        }
        return docsToCopy;
    }

    std::map<ShardId, int64_t> getDocumentsDeltaFromDonors(
        OperationContext* opCtx,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token,
        const UUID& reshardingUUID,
        const NamespaceString& nss,
        const std::vector<ShardId>& shardIds) override {
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kGetDocumentsDeltaFromDonors);
        if (_options.getDocumentsDeltaErrorCode) {
            uasserted(*_options.getDocumentsDeltaErrorCode, "Failing call to getDocumentsDelta");
        }

        std::map<ShardId, int64_t> docsDelta;
        for (const auto& shardId : shardIds) {
            auto it = _options.documentsDelta.find(shardId);
            ASSERT(it != _options.documentsDelta.end());
            docsDelta.emplace(shardId, it->second);
        }
        return docsDelta;
    }

    void verifyClonedCollection(OperationContext* opCtx,
                                const std::shared_ptr<executor::TaskExecutor>& executor,
                                CancellationToken token,
                                const ReshardingCoordinatorDocument& coordinatorDoc) override {
        if (_options.verifyClonedErrorCode) {
            uasserted(*_options.verifyClonedErrorCode, "Failing cloned collection verification");
        }
    }

    void verifyFinalCollection(OperationContext* opCtx,
                               const ReshardingCoordinatorDocument& coordinatorDoc) override {
        if (_options.verifyFinalErrorCode) {
            uasserted(*_options.verifyFinalErrorCode, "Failing final collection verification");
        }
    }

    void throwUnrecoverableErrorIn(CoordinatorStateEnum phase, ExternalFunction func) {
        _errorFunction = std::make_tuple(phase, func);
    }

private:
    const Options _options;

    boost::optional<std::tuple<CoordinatorStateEnum, ExternalFunction>> _errorFunction =
        boost::none;

    CoordinatorStateEnum _getCurrentPhaseOnDisk(OperationContext* opCtx) {
        DBDirectClient client(opCtx);

        auto doc = client.findOne(NamespaceString::kConfigReshardingOperationsNamespace, BSONObj{});
        IDLParserContext errCtx("reshardingCoordFromTest");
        auto parseDoc = ReshardingCoordinatorDocument::parse(doc, errCtx);
        return parseDoc.getState();
    }

    void _maybeThrowErrorForFunction(OperationContext* opCtx, ExternalFunction func) {
        if (_errorFunction) {
            auto [expectedPhase, expectedFunction] = *_errorFunction;
            auto currentPhase = _getCurrentPhaseOnDisk(opCtx);

            if (currentPhase == expectedPhase && func == expectedFunction) {
                uasserted(ErrorCodes::InternalError, "Simulating unrecoverable error for testing");
            }
        }
    }
};

class ReshardingCoordinatorServiceForTest : public ReshardingCoordinatorService {
public:
    explicit ReshardingCoordinatorServiceForTest(
        ServiceContext* serviceContext, std::shared_ptr<ExternalStateForTest> externalState)
        : ReshardingCoordinatorService(serviceContext),
          _serviceContext(serviceContext),
          _externalState(externalState) {}

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override {
        return std::make_shared<ReshardingCoordinator>(
            this,
            ReshardingCoordinatorDocument::parse(initialState,
                                                 IDLParserContext("ReshardingCoordinatorStateDoc")),
            _externalState,
            _serviceContext);
    }

private:
    ServiceContext* _serviceContext;
    std::shared_ptr<ExternalStateForTest> _externalState;
};

class ReshardingCoordinatorServiceTestBase : service_context_test::WithSetupTransportLayer,
                                             public ConfigServerTestFixture {
public:
    struct ReshardingOptions {
        const std::vector<ShardId> donorShardIds;
        const std::vector<ShardId> recipientShardIds;
        const std::set<ShardId> recipientShardIdsNoInitialChunks;
        bool performVerification;

        ReshardingOptions(std::vector<ShardId> donorShardIds_,
                          std::vector<ShardId> recipientShardIds_,
                          std::set<ShardId> recipientShardIdsNoInitialChunks_ = {},
                          bool performVerification_ = true)
            : donorShardIds(donorShardIds_),
              recipientShardIds(recipientShardIds_),
              recipientShardIdsNoInitialChunks(recipientShardIdsNoInitialChunks_),
              performVerification(performVerification_) {
            ASSERT_GT(donorShardIds.size(), 0);
            ASSERT_GT(recipientShardIds.size(), 0);
            ASSERT_GT(recipientShardIds.size(), recipientShardIdsNoInitialChunks.size());
        };
    };

    ReshardingOptions makeDefaultReshardingOptions() {
        // Make the resharding operation have all shards as the donors and recipients.
        auto donorShardIds = getShardIds();
        auto recipientShardIds = getShardIds();
        return {donorShardIds, recipientShardIds};
    }

    virtual ExternalStateForTest::Options getExternalStateOptions() const = 0;

    std::unique_ptr<repl::PrimaryOnlyService> makeService(
        ServiceContext* serviceContext, std::shared_ptr<ExternalStateForTest> externalState) {
        return std::make_unique<ReshardingCoordinatorServiceForTest>(serviceContext, externalState);
    }

    void setUp() override {
        ConfigServerTestFixture::setUp();

        ShardType shard0;
        shard0.setName(shardId0.toString());
        shard0.setHost(shardId0.toString() + ":1234");
        ShardType shard1;
        shard1.setName(shardId1.toString());
        shard1.setHost(shardId1.toString() + ":1234");
        setupShards({shard0, shard1});

        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});
        client.createCollection(NamespaceString::kConfigReshardingOperationsNamespace);
        client.createCollection(CollectionType::ConfigNS);

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->initializeIfNeeded(operationContext(), /* term */ 1);

        _controller = std::make_shared<CoordinatorStateTransitionController>();
        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());

        repl::createOplog(opCtx);

        auto asyncRPCMock = std::make_unique<async_rpc::NoopMockAsyncRPCRunner>();
        async_rpc::detail::AsyncRPCRunner::set(getServiceContext(), std::move(asyncRPCMock));

        _opObserverRegistry =
            dynamic_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        invariant(_opObserverRegistry);

        _opObserverRegistry->addObserver(std::make_unique<ReshardingOpObserver>());
        _opObserverRegistry->addObserver(std::make_unique<OpObserverForTest>(
            _controller,
            NamespaceString::kConfigReshardingOperationsNamespace,
            [](const ReshardingCoordinatorDocument& stateDoc) { return stateDoc.getState(); }));
        _registry = repl::PrimaryOnlyServiceRegistry::get(getServiceContext());

        _externalState = std::make_shared<ExternalStateForTest>(getExternalStateOptions());
        auto service = makeService(getServiceContext(), _externalState);
        auto serviceName = service->getServiceName();
        _registry->registerService(std::move(service));
        _service = _registry->lookupServiceByName(serviceName);

        _registry->onStartup(opCtx);
        auto term = replicationCoordinator()->getMyLastAppliedOpTime().getTerm();
        _registry->onStepUpComplete(opCtx, term);
    }

    void tearDown() override {
        globalFailPointRegistry().disableAllFailpoints();

        TransactionCoordinatorService::get(operationContext())->interrupt();
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ConfigServerTestFixture::tearDown();
        _registry->onShutdown();
    }

    CoordinatorStateTransitionController* controller() {
        return _controller.get();
    }

    ExternalStateForTest* externalState() {
        return _externalState.get();
    }

    ReshardingCoordinatorDocument makeCoordinatorDoc(CoordinatorStateEnum state,
                                                     UUID reshardingUUID,
                                                     NamespaceString originalNss,
                                                     NamespaceString tempNss,
                                                     const ShardKeyPattern& newShardKey,
                                                     const ReshardingOptions& reshardingOptions) {
        CommonReshardingMetadata meta(
            reshardingUUID, originalNss, UUID::gen(), tempNss, newShardKey.toBSON());
        meta.setPerformVerification(reshardingOptions.performVerification);
        meta.setStartTime(getServiceContext()->getFastClockSource()->now());

        std::vector<DonorShardEntry> donorShards;
        std::transform(reshardingOptions.donorShardIds.begin(),
                       reshardingOptions.donorShardIds.end(),
                       std::back_inserter(donorShards),
                       [](auto shardId) { return DonorShardEntry{shardId, {}}; });

        std::vector<RecipientShardEntry> recipientShards;
        std::transform(reshardingOptions.recipientShardIds.begin(),
                       reshardingOptions.recipientShardIds.end(),
                       std::back_inserter(recipientShards),
                       [](auto shardId) { return RecipientShardEntry{shardId, {}}; });

        ReshardingCoordinatorDocument doc(state, donorShards, recipientShards);
        doc.setCommonReshardingMetadata(meta);
        resharding::emplaceCloneTimestampIfExists(doc, _cloneTimestamp);

        // Set demo mode to true for testing purposes to avoid the delay before commit monitor
        // queries recipient.
        doc.setDemoMode(true);

        return doc;
    }

    std::shared_ptr<ReshardingCoordinator> getCoordinator(
        OperationContext* opCtx, repl::PrimaryOnlyService::InstanceID instanceId) {
        auto coordinator = getCoordinatorIfExists(opCtx, instanceId);
        ASSERT_TRUE(bool(coordinator));
        return coordinator;
    }

    std::shared_ptr<ReshardingCoordinator> getCoordinatorIfExists(
        OperationContext* opCtx, repl::PrimaryOnlyService::InstanceID instanceId) {
        auto [coordinatorOpt, _] = ReshardingCoordinator::lookup(opCtx, _service, instanceId);
        return coordinatorOpt ? *coordinatorOpt : nullptr;
    }

    BSONObj getCoordinatorDocBSON(OperationContext* opCtx) {
        DBDirectClient client(opCtx);
        return client.findOne(NamespaceString::kConfigReshardingOperationsNamespace, BSONObj{});
    }

    ReshardingCoordinatorDocument getCoordinatorDoc(OperationContext* opCtx) {
        DBDirectClient client(opCtx);

        auto doc = client.findOne(NamespaceString::kConfigReshardingOperationsNamespace, BSONObj{});
        IDLParserContext errCtx("reshardingCoordFromTest");
        return ReshardingCoordinatorDocument::parse(doc, errCtx);
    }

    void checkCoordinatorDocumentRemoved(OperationContext* opCtx) {
        const auto coordinatorColl = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest{NamespaceString::kConfigReshardingOperationsNamespace,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kRead},
            MODE_IS);
        ASSERT_TRUE(coordinatorColl.exists());
        ASSERT_TRUE(bool(coordinatorColl.getCollectionPtr()->isEmpty(opCtx)));
    }

    CollectionType getTemporaryCollectionCatalogEntry(
        OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {
        DBDirectClient client(opCtx);
        auto doc = client.findOne(CollectionType::ConfigNS,
                                  BSON(CollectionType::kNssFieldName
                                       << coordinatorDoc.getTempReshardingNss().ns_forTest()));
        return CollectionType{std::move(doc)};
    }

    void updateCoordinatorDoc(OperationContext* opCtx,
                              const UUID& reshardingUUID,
                              const BSONObj& updates) {
        DBDirectClient client(opCtx);

        const BSONObj query(
            BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName << reshardingUUID));
        client.update(NamespaceString::kConfigReshardingOperationsNamespace, query, updates);
    }

    void waitUntilCommittedCoordinatorDocReach(OperationContext* opCtx,
                                               CoordinatorStateEnum state) {
        DBDirectClient client(opCtx);

        while (true) {
            auto coordinatorDoc = getCoordinatorDocBSON(opCtx);

            auto currentState = coordinatorDoc.getStringField("state");
            if (currentState == CoordinatorState_serializer(state)) {
                break;
            }

            sleepmillis(50);
        }
    }

    void makeDonorsReadyToDonateWithAssert(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);
        ASSERT_NE(coordDoc.getStartTime(), Date_t::min());

        auto donorShards = coordDoc.getDonorShards();

        BSONObj updates = BSON(
            "$set" << BSON(
                ReshardingCoordinatorDocument::kDonorShardsFieldName + ".$[].mutableState.state"
                << DonorState_serializer(DonorStateEnum::kDonatingInitialData)
                << ReshardingCoordinatorDocument::kDonorShardsFieldName +
                    ".$[].mutableState.minFetchTimestamp"
                << _cloneTimestamp
                << ReshardingCoordinatorDocument::kDonorShardsFieldName +
                    ".$[].mutableState.bytesToClone"
                << static_cast<int64_t>(totalApproxBytesToClone / donorShards.size())
                << ReshardingCoordinatorDocument::kDonorShardsFieldName +
                    ".$[].mutableState.documentsToClone"
                << static_cast<int64_t>(totalApproxDocumentsToClone / donorShards.size())));

        updateCoordinatorDoc(opCtx, coordDoc.getReshardingUUID(), updates);
    }

    void makeRecipientsFinishedCloningWithAssert(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);
        ASSERT_NE(coordDoc.getMetrics()->getDocumentCopy()->getStart(), Date_t::min());

        BSONObj updates = BSON(
            "$set" << BSON(
                ReshardingCoordinatorDocument::kRecipientShardsFieldName + ".$[].mutableState.state"
                << RecipientState_serializer(RecipientStateEnum::kApplying)));

        updateCoordinatorDoc(opCtx, coordDoc.getReshardingUUID(), updates);
    }

    void makeRecipientsBeInStrictConsistencyWithAssert(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);
        ASSERT_LTE(coordDoc.getMetrics()->getOplogApplication()->getStart(),
                   coordDoc.getMetrics()->getOplogApplication()->getStop());

        BSONObj updates = BSON(
            "$set" << BSON(
                ReshardingCoordinatorDocument::kRecipientShardsFieldName + ".$[].mutableState.state"
                << RecipientState_serializer(RecipientStateEnum::kStrictConsistency)));

        updateCoordinatorDoc(opCtx, coordDoc.getReshardingUUID(), updates);
    }

    void makeDonorsProceedToDone(OperationContext* opCtx, UUID reshardingUUID) {
        BSONObj updates = BSON(
            "$set" << BSON(
                ReshardingCoordinatorDocument::kDonorShardsFieldName + ".$[].mutableState.state"
                << DonorState_serializer(DonorStateEnum::kDone)));

        updateCoordinatorDoc(opCtx, reshardingUUID, updates);
    }

    void makeDonorsProceedToDone(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);
        makeDonorsProceedToDone(opCtx, coordDoc.getReshardingUUID());
    }

    void makeDonorsProceedToDoneWithAssert(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);
        ASSERT_LTE(coordDoc.getMetrics()->getDocumentCopy()->getStart(),
                   coordDoc.getMetrics()->getDocumentCopy()->getStop());

        makeDonorsProceedToDone(opCtx, coordDoc.getReshardingUUID());
    }

    void makeRecipientsProceedToDone(OperationContext* opCtx, UUID reshardingUUID) {
        BSONObj updates = BSON(
            "$set" << BSON(
                ReshardingCoordinatorDocument::kRecipientShardsFieldName + ".$[].mutableState.state"
                << RecipientState_serializer(RecipientStateEnum::kDone)));

        updateCoordinatorDoc(opCtx, reshardingUUID, updates);
    }

    void makeRecipientsProceedToDone(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);
        makeRecipientsProceedToDone(opCtx, coordDoc.getReshardingUUID());
    }

    void makeRecipientsProceedToDoneWithAssert(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);
        ASSERT_LTE(coordDoc.getMetrics()->getDocumentCopy()->getStart(),
                   coordDoc.getMetrics()->getDocumentCopy()->getStop());
        makeRecipientsProceedToDone(opCtx, coordDoc.getReshardingUUID());
    }

    void makeRecipientsReturnErrorWithAssert(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);
        ASSERT_NE(coordDoc.getMetrics()->getDocumentCopy()->getStart(), Date_t::min());

        Status abortReasonStatus{ErrorCodes::SnapshotUnavailable, "test simulated error"};
        BSONObjBuilder tmpBuilder;
        abortReasonStatus.serialize(&tmpBuilder);

        BSONObj updates = BSON(
            "$set" << BSON(
                ReshardingCoordinatorDocument::kRecipientShardsFieldName + ".$[].mutableState.state"
                << RecipientState_serializer(RecipientStateEnum::kError)
                << ReshardingCoordinatorDocument::kRecipientShardsFieldName +
                    ".$[].mutableState.abortReason"
                << tmpBuilder.obj()));

        updateCoordinatorDoc(opCtx, coordDoc.getReshardingUUID(), updates);
    }

    CollectionType makeOriginalCollectionCatalogEntry(
        ReshardingCoordinatorDocument coordinatorDoc,
        boost::optional<TypeCollectionReshardingFields> reshardingFields,
        OID epoch,
        Date_t lastUpdated) {
        UUID uuid = UUID::gen();
        BSONObj shardKey;
        if (coordinatorDoc.getState() >= CoordinatorStateEnum::kCommitting) {
            uuid = _reshardingUUID;
            shardKey = _newShardKey.toBSON();
        } else {
            uuid = _originalUUID;
            shardKey = _oldShardKey.toBSON();
        }

        CollectionType collType(coordinatorDoc.getSourceNss(),
                                std::move(epoch),
                                Timestamp(1, 2),
                                lastUpdated,
                                std::move(uuid),
                                shardKey);
        if (reshardingFields)
            collType.setReshardingFields(std::move(reshardingFields.value()));

        if (coordinatorDoc.getState() == CoordinatorStateEnum::kDone ||
            coordinatorDoc.getState() == CoordinatorStateEnum::kAborting) {
            collType.setAllowMigrations(true);
        } else if (coordinatorDoc.getState() >= CoordinatorStateEnum::kPreparingToDonate) {
            collType.setAllowMigrations(false);
        }
        return collType;
    }

    ReshardingCoordinatorDocument insertStateAndCatalogEntries(
        CoordinatorStateEnum state, OID epoch, const ReshardingOptions& reshardingOptions) {
        return insertStateAndCatalogEntries(
            state, epoch, _reshardingUUID, _originalNss, _tempNss, _newShardKey, reshardingOptions);
    }

    ReshardingCoordinatorDocument insertStateAndCatalogEntries(
        CoordinatorStateEnum state,
        OID epoch,
        UUID reshardingUUID,
        NamespaceString originalNss,
        NamespaceString tempNss,
        const ShardKeyPattern& newShardKey,
        const ReshardingOptions& reshardingOptions) {
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);

        auto coordinatorDoc = makeCoordinatorDoc(
            state, reshardingUUID, originalNss, tempNss, newShardKey, reshardingOptions);

        TypeCollectionReshardingFields reshardingFields(coordinatorDoc.getReshardingUUID());
        reshardingFields.setState(coordinatorDoc.getState());
        reshardingFields.setDonorFields(
            TypeCollectionDonorFields(coordinatorDoc.getTempReshardingNss(),
                                      coordinatorDoc.getReshardingKey(),
                                      resharding::extractShardIdsFromParticipantEntries(
                                          coordinatorDoc.getRecipientShards())));

        auto originalNssCatalogEntry = makeOriginalCollectionCatalogEntry(
            coordinatorDoc,
            reshardingFields,
            std::move(epoch),
            opCtx->getServiceContext()->getPreciseClockSource()->now());
        client.insert(CollectionType::ConfigNS, originalNssCatalogEntry.toBSON());

        DatabaseType dbDoc(coordinatorDoc.getSourceNss().dbName(),
                           coordinatorDoc.getDonorShards().front().getId(),
                           DatabaseVersion{UUID::gen(), Timestamp(1, 1)});
        client.insert(NamespaceString::kConfigDatabasesNamespace, dbDoc.toBSON());

        return coordinatorDoc;
    }

    void insertChunkAndZoneEntries(std::vector<ChunkType> chunks, std::vector<TagsType> zones) {
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);

        for (const auto& chunk : chunks) {
            client.insert(NamespaceString::kConfigsvrChunksNamespace, chunk.toConfigBSON());
        }

        for (const auto& zone : zones) {
            client.insert(TagsType::ConfigNS, zone.toBSON());
        }
    }

    std::vector<ChunkType> makeChunks(const UUID& uuid,
                                      OID epoch,
                                      const Timestamp& timestamp,
                                      const ShardKeyPattern& shardKey,
                                      const std::vector<ShardId>& shardIds) {
        auto chunkRanges =
            _newShardKey.isShardKey(shardKey.toBSON()) ? _newChunkRanges : _oldChunkRanges;
        ASSERT_GTE(chunkRanges.size(), shardIds.size());

        int currentIndex = 0;
        auto getNextShardId = [&] {
            return shardIds[currentIndex++ % shardIds.size()];
        };

        // Use round-robin to distribute chunks among the given shards.
        ChunkVersion version({epoch, timestamp}, {1, 0});
        ChunkType chunk1(uuid, chunkRanges[0], version, getNextShardId());
        chunk1.setName(OID::gen());
        version.incMinor();
        ChunkType chunk2(uuid, chunkRanges[1], version, getNextShardId());
        chunk2.setName(OID::gen());

        return std::vector<ChunkType>{chunk1, chunk2};
    }

    void makeAndInsertChunksForDonorShard(const UUID& uuid,
                                          OID epoch,
                                          const Timestamp& timestamp,
                                          const ShardKeyPattern& shardKey,
                                          const std::vector<ShardId> shardIds) {
        auto chunks = makeChunks(uuid, epoch, timestamp, shardKey, shardIds);
        insertChunkAndZoneEntries(std::move(chunks), {});
    }

    void stepUp(OperationContext* opCtx) {
        auto replCoord = repl::ReplicationCoordinator::get(getServiceContext());
        auto currOpTime = replCoord->getMyLastAppliedOpTime();

        // Advance the term and last applied opTime. We retain the timestamp component of the
        // current last applied opTime to avoid log messages from
        // ReplClientInfo::setLastOpToSystemLastOpTime() about the opTime having moved backwards.
        ++_term;
        auto newOpTime = repl::OpTime{currOpTime.getTimestamp(), _term};

        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        ASSERT_OK(replCoord->updateTerm(opCtx, _term));
        replCoord->setMyLastAppliedOpTimeAndWallTimeForward({newOpTime, {}});

        _registry->onStepUpComplete(opCtx, _term);
    }

    void stepDown(OperationContext* opCtx) {
        ASSERT_OK(repl::ReplicationCoordinator::get(getServiceContext())
                      ->setFollowerMode(repl::MemberState::RS_SECONDARY));
        _registry->onStepDown();

        // Some opCtx can be created via AlternativeClientRegion and not tied to any resharding
        // cancellation token, so we also need to simulate the repl step down killOp thread.

        auto serviceCtx = opCtx->getServiceContext();
        for (ServiceContext::LockedClientsCursor cursor(serviceCtx);
             Client* client = cursor.next();) {
            if (!client->canKillOperationInStepdown()) {
                continue;
            }

            ClientLock lk(client);
            OperationContext* toKill = client->getOperationContext();

            if (toKill && !toKill->isKillPending() && toKill->getOpID() != opCtx->getOpID()) {
                auto locker = shard_role_details::getLocker(toKill);
                if (toKill->shouldAlwaysInterruptAtStepDownOrUp() ||
                    locker->wasGlobalLockTakenInModeConflictingWithWrites()) {
                    serviceCtx->killOperation(lk, toKill);
                }
            }
        }
    }

    void killAllReshardingCoordinatorOps() {
        for (ServiceContext::LockedClientsCursor cursor(getServiceContext());
             Client* client = cursor.next();) {
            invariant(client);

            ClientLock lk(client);
            if (auto opCtx = client->getOperationContext()) {
                StringData desc(client->desc());

                // Resharding coordinator doc changes are run under WithTransaction, which uses
                // AlternativeSessionRegion.
                if (desc.find("alternative-session-region") != std::string::npos) {
                    getServiceContext()->killOperation(lk, opCtx);
                }
            }
        }
    }

    auto initializeAndGetCoordinator() {
        return initializeAndGetCoordinator(
            _reshardingUUID, _originalNss, _tempNss, _newShardKey, _originalUUID, _oldShardKey);
    }

    std::shared_ptr<ReshardingCoordinator> initializeAndGetCoordinator(
        UUID reshardingUUID,
        NamespaceString originalNss,
        NamespaceString tempNss,
        const ShardKeyPattern& newShardKey,
        UUID originalUUID,
        const ShardKeyPattern& oldShardKey,
        boost::optional<ReshardingOptions> reshardingOptions = boost::none) {
        if (!reshardingOptions) {
            reshardingOptions.emplace(makeDefaultReshardingOptions());
        }

        auto doc = insertStateAndCatalogEntries(CoordinatorStateEnum::kUnused,
                                                _originalEpoch,
                                                reshardingUUID,
                                                originalNss,
                                                tempNss,
                                                newShardKey,
                                                *reshardingOptions);
        auto opCtx = operationContext();

        makeAndInsertChunksForDonorShard(originalUUID,
                                         _originalEpoch,
                                         _originalTimestamp,
                                         oldShardKey,
                                         reshardingOptions->donorShardIds);

        std::vector<ShardId> recipientShardIdsForInitialChunks;
        std::copy_if(reshardingOptions->recipientShardIds.begin(),
                     reshardingOptions->recipientShardIds.end(),
                     std::back_inserter(recipientShardIdsForInitialChunks),
                     [&](auto shardId) {
                         return !reshardingOptions->recipientShardIdsNoInitialChunks.contains(
                             shardId);
                     });

        auto initialChunks = makeChunks(reshardingUUID,
                                        _tempEpoch,
                                        _tempTimestamp,
                                        newShardKey,
                                        recipientShardIdsForInitialChunks);

        std::vector<ReshardedChunk> presetReshardedChunks;
        for (const auto& chunk : initialChunks) {
            presetReshardedChunks.emplace_back(chunk.getShard(), chunk.getMin(), chunk.getMax());
        }

        doc.setPresetReshardedChunks(presetReshardedChunks);

        return ReshardingCoordinator::getOrCreate(opCtx, _service, doc.toBSON());
    }

    using TransitionFunctionMap = stdx::unordered_map<CoordinatorStateEnum, std::function<void()>>;

    void runReshardingToCompletion() {
        runReshardingToCompletion(TransitionFunctionMap{});
    }

    void runReshardingToCompletion(
        const TransitionFunctionMap& transitionFunctions,
        std::unique_ptr<PauseDuringStateTransitions> stateTransitionsGuard = nullptr,
        const std::vector<CoordinatorStateEnum> states = {CoordinatorStateEnum::kPreparingToDonate,
                                                          CoordinatorStateEnum::kCloning,
                                                          CoordinatorStateEnum::kApplying,
                                                          CoordinatorStateEnum::kBlockingWrites,
                                                          CoordinatorStateEnum::kCommitting},
        boost::optional<ReshardingOptions> reshardingOptions = boost::none,
        boost::optional<CoordinatorStateEnum> errorState = boost::none) {
        auto runFunctionForState = [&](CoordinatorStateEnum state) {
            auto it = transitionFunctions.find(state);
            if (it == transitionFunctions.end()) {
                return;
            }
            it->second();
        };

        if (!stateTransitionsGuard) {
            stateTransitionsGuard =
                std::make_unique<PauseDuringStateTransitions>(controller(), states);
        }

        auto opCtx = operationContext();
        auto coordinator = initializeAndGetCoordinator(_reshardingUUID,
                                                       _originalNss,
                                                       _tempNss,
                                                       _newShardKey,
                                                       _originalUUID,
                                                       _oldShardKey,
                                                       reshardingOptions);

        for (const auto state : states) {
            stateTransitionsGuard->wait(state);
            runFunctionForState(state);
            stateTransitionsGuard->unset(state);

            if (errorState && state == *errorState) {
                waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kAborting);

                makeRecipientsProceedToDone(opCtx);
                makeDonorsProceedToDone(opCtx);

                ASSERT_EQ(coordinator->getCompletionFuture().getNoThrow(),
                          ErrorCodes::InternalError);
                checkCoordinatorDocumentRemoved(opCtx);
                return;
            } else {
                waitUntilCommittedCoordinatorDocReach(opCtx, state);
            }

            switch (state) {
                case CoordinatorStateEnum::kPreparingToDonate:
                    makeDonorsReadyToDonateWithAssert(opCtx);
                    break;
                case CoordinatorStateEnum::kCloning:
                    makeRecipientsFinishedCloningWithAssert(opCtx);
                    break;
                case CoordinatorStateEnum::kApplying:
                    coordinator->onOkayToEnterCritical();
                    break;
                case CoordinatorStateEnum::kBlockingWrites:
                    makeRecipientsBeInStrictConsistencyWithAssert(opCtx);
                    break;
                case CoordinatorStateEnum::kCommitting:
                    makeDonorsProceedToDoneWithAssert(opCtx);
                    makeRecipientsProceedToDoneWithAssert(opCtx);
                    break;
                default:
                    break;
            }
        }
        coordinator->getCompletionFuture().get(opCtx);

        BSONObjBuilder bob;
        ReshardingCumulativeMetrics::getForResharding(operationContext()->getServiceContext())
            ->reportForServerStatus(&bob);
        auto cumulativeMetricsBSON = bob.obj();
        ASSERT_EQ(cumulativeMetricsBSON["resharding"]["countStarted"].numberInt(), 1);
        ASSERT_EQ(cumulativeMetricsBSON["resharding"]["countSucceeded"].numberInt(), 1);
    }

    void runReshardingWithUnrecoverableError(
        CoordinatorStateEnum errorState,
        ExternalStateForTest::ExternalFunction errorFunction,
        ErrorCodes::Error expectedError = ErrorCodes::InternalError) {
        std::vector<CoordinatorStateEnum> states;
        const std::vector<CoordinatorStateEnum> allStates = {
            CoordinatorStateEnum::kPreparingToDonate,
            CoordinatorStateEnum::kCloning,
            CoordinatorStateEnum::kApplying,
            CoordinatorStateEnum::kBlockingWrites};

        for (const auto& state : allStates) {
            states.push_back(state);
            if (state == errorState)
                break;
        }

        externalState()->throwUnrecoverableErrorIn(errorState, errorFunction);

        runReshardingToCompletion(
            {},
            std::make_unique<PauseDuringStateTransitions>(controller(), states),
            states,
            makeDefaultReshardingOptions(),
            errorState);
    }

    int64_t getDocumentsToCopyForDonor(const ShardId& shardId) {
        auto it = documentsToCopy.find(shardId);
        ASSERT(it != documentsToCopy.end());
        return it->second;
    }

    int64_t getDocumentsDeltaForDonor(const ShardId& shardId) {
        auto it = documentsDelta.find(shardId);
        ASSERT(it != documentsDelta.end());
        return it->second;
    }

    void checkDonorDocumentsToCopyMetrics(const ReshardingCoordinatorDocument& coordinatorDoc) {
        if (coordinatorDoc.getState() < CoordinatorStateEnum::kApplying) {
            return;
        }
        for (const auto& donorShard : coordinatorDoc.getDonorShards()) {
            if (coordinatorDoc.getCommonReshardingMetadata().getPerformVerification()) {
                ASSERT_EQUALS(*donorShard.getDocumentsToCopy(),
                              getDocumentsToCopyForDonor(donorShard.getId()));
            } else {
                ASSERT_FALSE(donorShard.getDocumentsToCopy().has_value());
            }
        }
    }

    void checkDonorDocumentsFinalMetrics(const ReshardingCoordinatorDocument& coordinatorDoc) {
        if (coordinatorDoc.getState() < CoordinatorStateEnum::kBlockingWrites) {
            return;
        }
        for (auto& donorShardEntry : coordinatorDoc.getDonorShards()) {
            if (coordinatorDoc.getCommonReshardingMetadata().getPerformVerification()) {
                ASSERT_EQUALS(donorShardEntry.getDocumentsFinal(),
                              *donorShardEntry.getDocumentsToCopy() +
                                  getDocumentsDeltaForDonor(donorShardEntry.getId()));
            } else {
                ASSERT_FALSE(donorShardEntry.getDocumentsFinal().has_value());
            }
        }
    }

    void runReshardingToCompletionAssertApproxToCopyMetrics(
        const ReshardingOptions& reshardingOptions) {
        long numRecipientsToClone = reshardingOptions.recipientShardIds.size() -
            reshardingOptions.recipientShardIdsNoInitialChunks.size();
        long expectedApproxBytesToClone = totalApproxBytesToClone / numRecipientsToClone;
        long expectedApproxDocumentsToClone = totalApproxDocumentsToClone / numRecipientsToClone;

        auto checkPersistentStates = [&] {
            auto opCtx = operationContext();
            auto coordinatorDoc = getCoordinatorDoc(opCtx);
            if (coordinatorDoc.getState() >= CoordinatorStateEnum::kCloning) {
                ASSERT_EQ(*coordinatorDoc.getApproxBytesToCopy(), expectedApproxBytesToClone);
                ASSERT_EQ(*coordinatorDoc.getApproxDocumentsToCopy(),
                          expectedApproxDocumentsToClone);

                auto collectionDoc = getTemporaryCollectionCatalogEntry(opCtx, coordinatorDoc);
                auto recipientFields = collectionDoc.getReshardingFields()->getRecipientFields();
                ASSERT_EQ(*recipientFields->getApproxBytesToCopy(), expectedApproxBytesToClone);
                ASSERT_EQ(*recipientFields->getApproxDocumentsToCopy(),
                          expectedApproxDocumentsToClone);
            }
        };

        auto transitionFunctions =
            TransitionFunctionMap{{CoordinatorStateEnum::kCloning, checkPersistentStates},
                                  {CoordinatorStateEnum::kApplying, checkPersistentStates},
                                  {CoordinatorStateEnum::kBlockingWrites, checkPersistentStates},
                                  {CoordinatorStateEnum::kCommitting, checkPersistentStates}};
        auto states = {CoordinatorStateEnum::kPreparingToDonate,
                       CoordinatorStateEnum::kCloning,
                       CoordinatorStateEnum::kApplying,
                       CoordinatorStateEnum::kBlockingWrites,
                       CoordinatorStateEnum::kCommitting};
        runReshardingToCompletion(
            transitionFunctions, nullptr /* stateTransitionsGuard */, states, reshardingOptions);
    }

    repl::PrimaryOnlyService* _service = nullptr;

    std::shared_ptr<CoordinatorStateTransitionController> _controller;
    std::shared_ptr<ExternalStateForTest> _externalState;

    OpObserverRegistry* _opObserverRegistry = nullptr;

    repl::PrimaryOnlyServiceRegistry* _registry = nullptr;

    NamespaceString _originalNss = NamespaceString::createNamespaceString_forTest("db.foo");
    UUID _originalUUID = UUID::gen();
    OID _originalEpoch = OID::gen();
    Timestamp _originalTimestamp = Timestamp(1);

    NamespaceString _tempNss = NamespaceString::createNamespaceString_forTest(
        "db.system.resharding." + _originalUUID.toString());
    UUID _reshardingUUID = UUID::gen();
    OID _tempEpoch = OID::gen();
    Timestamp _tempTimestamp = Timestamp(2);
    ShardKeyPattern _oldShardKey = ShardKeyPattern(BSON("oldShardKey" << 1));
    ShardKeyPattern _newShardKey = ShardKeyPattern(BSON("newShardKey" << 1));

    const std::vector<ChunkRange> _oldChunkRanges = {
        ChunkRange(_oldShardKey.getKeyPattern().globalMin(), BSON("oldShardKey" << 12345)),
        ChunkRange(BSON("oldShardKey" << 12345), _oldShardKey.getKeyPattern().globalMax()),
    };
    const std::vector<ChunkRange> _newChunkRanges = {
        ChunkRange(_newShardKey.getKeyPattern().globalMin(), BSON("newShardKey" << 0)),
        ChunkRange(BSON("newShardKey" << 0), _newShardKey.getKeyPattern().globalMax()),
    };

    Timestamp _cloneTimestamp = Timestamp(Date_t::now());

    RAIIServerParameterControllerForTest _serverParamController{
        "reshardingMinimumOperationDurationMillis", 0};

    long long _term = 0;

protected:
    std::vector<ShardId> getShardIds() const {
        return {shardId0, shardId1};
    }

    const ShardId shardId0{"shard0000"};
    const ShardId shardId1{"shard0001"};

    const long totalApproxBytesToClone = 10000;
    const long totalApproxDocumentsToClone = 100;

    const std::map<ShardId, int64_t> documentsToCopy{
        {shardId0, 65},
        {shardId1, 55},
    };

    const std::map<ShardId, int64_t> documentsDelta{
        {shardId0, 10},
        {shardId1, 20},
    };
};

class ReshardingCoordinatorServiceTest : public ReshardingCoordinatorServiceTestBase {
public:
    using enum ExternalStateForTest::ExternalFunction;

    ExternalStateForTest::Options getExternalStateOptions() const override {
        return {.documentsToCopy = documentsToCopy, .documentsDelta = documentsDelta};
    }
};

TEST_F(ReshardingCoordinatorServiceTest, ReshardingCoordinatorSuccessfullyTransitionsTokDone) {
    runReshardingToCompletion();
}

TEST_F(ReshardingCoordinatorServiceTest, ReshardingCoordinatorTransitionsTokDoneWithInterrupt) {
    auto reshardingOptions = makeDefaultReshardingOptions();
    reshardingOptions.performVerification = true;
    const auto interrupt = [this] {
        killAllReshardingCoordinatorOps();
    };
    runReshardingToCompletion(
        TransitionFunctionMap{{CoordinatorStateEnum::kPreparingToDonate, interrupt},
                              {CoordinatorStateEnum::kCloning, interrupt},
                              {CoordinatorStateEnum::kApplying, interrupt},
                              {CoordinatorStateEnum::kBlockingWrites, interrupt}},
        nullptr /* stateTransitionsGuard */,
        {CoordinatorStateEnum::kPreparingToDonate,
         CoordinatorStateEnum::kCloning,
         CoordinatorStateEnum::kApplying,
         CoordinatorStateEnum::kBlockingWrites,
         CoordinatorStateEnum::kCommitting},
        reshardingOptions);
}

TEST_F(ReshardingCoordinatorServiceTest,
       ReshardingCoordinatorTransitionsTokDoneWithTransactionFailWC) {
    const auto failNextTransaction = [] {
        globalFailPointRegistry()
            .find("shardingCatalogManagerWithTransactionFailWCAfterCommit")
            ->setMode(FailPoint::nTimes, 1);
    };
    // PauseDuringStateTransitions relies on updates to the coordinator state document on disk to
    // decide when to unpause. kInitializing is the initial state written to disk (i.e. not an
    // update), but we still want to verify correct behavior if the transaction to transition to
    // kInitializing fails, so call failNextTransaction() before calling
    // runReshardingToCompletion().
    failNextTransaction();
    runReshardingToCompletion(
        TransitionFunctionMap{{CoordinatorStateEnum::kPreparingToDonate, failNextTransaction},
                              {CoordinatorStateEnum::kCloning, failNextTransaction},
                              {CoordinatorStateEnum::kApplying, failNextTransaction},
                              {CoordinatorStateEnum::kBlockingWrites, failNextTransaction}});
}

TEST_F(ReshardingCoordinatorServiceTest, StepDownStepUpDuringInitializing) {
    PauseDuringStateTransitions stateTransitionsGuard{controller(),
                                                      CoordinatorStateEnum::kPreparingToDonate};

    auto opCtx = operationContext();
    auto pauseAfterInsertCoordinatorDoc =
        globalFailPointRegistry().find("pauseAfterInsertCoordinatorDoc");
    auto timesEnteredFailPoint = pauseAfterInsertCoordinatorDoc->setMode(FailPoint::alwaysOn, 0);

    auto reshardingOptions = makeDefaultReshardingOptions();
    auto doc = insertStateAndCatalogEntries(
        CoordinatorStateEnum::kUnused, _originalEpoch, reshardingOptions);

    makeAndInsertChunksForDonorShard(_originalUUID,
                                     _originalEpoch,
                                     _originalTimestamp,
                                     _oldShardKey,
                                     reshardingOptions.donorShardIds);

    auto initialChunks = makeChunks(_reshardingUUID,
                                    _tempEpoch,
                                    _tempTimestamp,
                                    _newShardKey,
                                    reshardingOptions.recipientShardIds);

    std::vector<ReshardedChunk> presetReshardedChunks;
    for (const auto& chunk : initialChunks) {
        presetReshardedChunks.emplace_back(chunk.getShard(), chunk.getMin(), chunk.getMax());
    }

    doc.setPresetReshardedChunks(presetReshardedChunks);

    (void)ReshardingCoordinator::getOrCreate(opCtx, _service, doc.toBSON());
    auto instanceId =
        BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

    pauseAfterInsertCoordinatorDoc->waitForTimesEntered(timesEnteredFailPoint + 1);

    auto coordinator = getCoordinator(opCtx, instanceId);
    stepDown(opCtx);
    pauseAfterInsertCoordinatorDoc->setMode(FailPoint::off, 0);
    ASSERT_EQ(coordinator->getCompletionFuture().getNoThrow(), ErrorCodes::CallbackCanceled);

    coordinator.reset();
    stepUp(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kPreparingToDonate);

    // Ensure that promises are not fulfilled on the new coordinator.
    auto newCoordinator = getCoordinator(opCtx, instanceId);
    auto newObserver = newCoordinator->getObserver();
    ASSERT_FALSE(newObserver->awaitAllDonorsReadyToDonate().isReady());
    ASSERT_FALSE(newObserver->awaitAllRecipientsFinishedCloning().isReady());
    ASSERT_FALSE(newObserver->awaitAllRecipientsInStrictConsistency().isReady());
    ASSERT_FALSE(newObserver->awaitAllDonorsDone().isReady());
    ASSERT_FALSE(newObserver->awaitAllRecipientsDone().isReady());

    stepDown(opCtx);
    ASSERT_EQ(newCoordinator->getCompletionFuture().getNoThrow(), ErrorCodes::CallbackCanceled);
}

/**
 * Test stepping down right when coordinator doc is being updated. Causing the change to be
 * rolled back and redo the work again on step up.
 */
TEST_F(ReshardingCoordinatorServiceTest, StepDownStepUpEachTransition) {
    const std::vector<CoordinatorStateEnum> coordinatorStates{
        // Skip kInitializing, as we don't write that state transition to storage.
        CoordinatorStateEnum::kPreparingToDonate,
        CoordinatorStateEnum::kCloning,
        CoordinatorStateEnum::kApplying,
        CoordinatorStateEnum::kBlockingWrites,
        CoordinatorStateEnum::kCommitting};
    PauseDuringStateTransitions stateTransitionsGuard{controller(), coordinatorStates};

    auto reshardingOptions = makeDefaultReshardingOptions();
    auto doc = insertStateAndCatalogEntries(
        CoordinatorStateEnum::kUnused, _originalEpoch, reshardingOptions);
    auto opCtx = operationContext();
    makeAndInsertChunksForDonorShard(_originalUUID,
                                     _originalEpoch,
                                     _originalTimestamp,
                                     _oldShardKey,
                                     reshardingOptions.donorShardIds);

    auto initialChunks = makeChunks(_reshardingUUID,
                                    _tempEpoch,
                                    _tempTimestamp,
                                    _newShardKey,
                                    reshardingOptions.recipientShardIds);

    std::vector<ReshardedChunk> presetReshardedChunks;
    for (const auto& chunk : initialChunks) {
        presetReshardedChunks.emplace_back(chunk.getShard(), chunk.getMin(), chunk.getMax());
    }

    doc.setPresetReshardedChunks(presetReshardedChunks);

    (void)ReshardingCoordinator::getOrCreate(opCtx, _service, doc.toBSON());
    auto instanceId =
        BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

    for (const auto state : coordinatorStates) {
        auto coordinator = getCoordinator(opCtx, instanceId);

        LOGV2(5093701,
              "Running step down test case",
              "stepDownAfter"_attr = CoordinatorState_serializer(state));

        switch (state) {
            case CoordinatorStateEnum::kCloning: {
                makeDonorsReadyToDonateWithAssert(opCtx);
                break;
            }

            case CoordinatorStateEnum::kApplying: {
                makeRecipientsFinishedCloningWithAssert(opCtx);
                break;
            }

            case CoordinatorStateEnum::kBlockingWrites: {
                // Pretend that the recipients are already ready to commit.
                coordinator->onOkayToEnterCritical();
                break;
            }

            case CoordinatorStateEnum::kCommitting: {
                makeRecipientsBeInStrictConsistencyWithAssert(opCtx);
                break;
            }

            default:
                break;
        }

        stateTransitionsGuard.wait(state);

        stepDown(opCtx);

        ASSERT_EQ(coordinator->getCompletionFuture().getNoThrow(), ErrorCodes::CallbackCanceled);

        coordinator.reset();

        stepUp(opCtx);

        stateTransitionsGuard.unset(state);

        if (state == CoordinatorStateEnum::kBlockingWrites) {
            // We have to fake this again as the effects are not persistent.
            coordinator = getCoordinator(opCtx, instanceId);
            coordinator->onOkayToEnterCritical();
        }

        auto coordinatorDoc = getCoordinatorDoc(opCtx);
        checkDonorDocumentsToCopyMetrics(coordinatorDoc);
        checkDonorDocumentsFinalMetrics(coordinatorDoc);

        // 'done' state is never written to storage so don't wait for it.
        waitUntilCommittedCoordinatorDocReach(opCtx, state);
    }

    makeDonorsProceedToDoneWithAssert(opCtx);
    makeRecipientsProceedToDoneWithAssert(opCtx);

    // Join the coordinator if it has not yet been cleaned up.
    if (auto coordinator = getCoordinatorIfExists(opCtx, instanceId)) {
        coordinator->getCompletionFuture().get(opCtx);
    }

    {
        DBDirectClient client(opCtx);

        // config.chunks should have been moved to the new UUID
        FindCommandRequest findRequest{NamespaceString::kConfigsvrChunksNamespace};
        findRequest.setFilter(BSON(ChunkType::collectionUUID() << doc.getReshardingUUID()));
        auto chunkCursor = client.find(std::move(findRequest));
        std::vector<ChunkType> foundChunks;
        while (chunkCursor->more()) {
            auto d = uassertStatusOK(ChunkType::parseFromConfigBSON(
                chunkCursor->nextSafe().getOwned(), _originalEpoch, _originalTimestamp));
            foundChunks.push_back(d);
        }
        ASSERT_EQUALS(foundChunks.size(), initialChunks.size());

        // config.collections should not have the document with the old UUID.
        std::vector<ChunkType> foundCollections;
        auto collection =
            client.findOne(CollectionType::ConfigNS,
                           BSON(CollectionType::kNssFieldName << doc.getSourceNss().ns_forTest()));

        ASSERT_EQUALS(collection.isEmpty(), false);
        ASSERT_EQUALS(
            UUID::parse(collection.getField(CollectionType::kUuidFieldName)).getValue().toString(),
            doc.getReshardingUUID().toString());
    }
}

TEST_F(ReshardingCoordinatorServiceTest, ReportForCurrentOpAfterCompletion) {
    auto pauseAfterInsertCoordinatorDoc =
        globalFailPointRegistry().find("pauseAfterInsertCoordinatorDoc");
    auto timesEnteredFailPoint = pauseAfterInsertCoordinatorDoc->setMode(FailPoint::alwaysOn);

    auto coordinator = initializeAndGetCoordinator();
    auto instanceId =
        BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName << _reshardingUUID);

    // Wait until we know we've inserted the coordinator doc, but before the coordinator contacts
    // any participants so that the coordinator does not have to "wait" for participants to abort
    // before finishing aborting itself
    pauseAfterInsertCoordinatorDoc->waitForTimesEntered(timesEnteredFailPoint + 1);

    // Force a failover, and wait for the state machine to fulfill the completion promise. At this
    // point, the resharding metrics will have been unregistered from the cumulative metrics.
    stepDown(operationContext());
    pauseAfterInsertCoordinatorDoc->setMode(FailPoint::off);
    ASSERT_EQ(coordinator->getCompletionFuture().getNoThrow(), ErrorCodes::CallbackCanceled);

    // Now call step up. The old coordinator object has not yet been destroyed because we still hold
    // a shared pointer to it ('coordinator') - this can happen in production after a failover if a
    // state machine is slow to clean up. Wait for the coordinator to have started, but again don't
    // let it move to a state where it contacts participants.
    auto pauseBeforeCTHolderInitialization =
        globalFailPointRegistry().find("pauseBeforeCTHolderInitialization");
    timesEnteredFailPoint = pauseBeforeCTHolderInitialization->setMode(FailPoint::alwaysOn);

    stepUp(operationContext());
    pauseBeforeCTHolderInitialization->waitForTimesEntered(timesEnteredFailPoint + 1);

    // Assert that the old coordinator object will return a currentOp report, because the resharding
    // metrics still exist on the coordinator object itelf.
    ASSERT(coordinator->reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode::kExcludeIdle,
        MongoProcessInterface::CurrentOpSessionsMode::kIncludeIdle));

    // Ensure a new coordinator can start and register resharding metrics, despite the "zombie"
    // state machine still existing.
    auto newCoordinator = getCoordinator(operationContext(), instanceId);
    ASSERT_NE(coordinator, newCoordinator);

    // No need to finish the resharding op, so we just cancel the op.
    newCoordinator->abort(true /* skipQuiescePeriod */);
    pauseBeforeCTHolderInitialization->setMode(FailPoint::off);
    newCoordinator->getCompletionFuture().wait();
}

TEST_F(ReshardingCoordinatorServiceTest, ReshardingCoordinatorFailsIfMigrationNotAllowed) {
    auto reshardingOptions = makeDefaultReshardingOptions();
    auto doc = insertStateAndCatalogEntries(
        CoordinatorStateEnum::kUnused, _originalEpoch, reshardingOptions);
    auto opCtx = operationContext();
    makeAndInsertChunksForDonorShard(_originalUUID,
                                     _originalEpoch,
                                     _originalTimestamp,
                                     _oldShardKey,
                                     reshardingOptions.donorShardIds);

    auto initialChunks = makeChunks(_reshardingUUID,
                                    _tempEpoch,
                                    _tempTimestamp,
                                    _newShardKey,
                                    reshardingOptions.recipientShardIds);

    std::vector<ReshardedChunk> presetReshardedChunks;
    for (const auto& chunk : initialChunks) {
        presetReshardedChunks.emplace_back(chunk.getShard(), chunk.getMin(), chunk.getMax());
    }

    doc.setPresetReshardedChunks(presetReshardedChunks);

    {
        DBDirectClient client(opCtx);
        client.update(CollectionType::ConfigNS,
                      BSON(CollectionType::kNssFieldName << _originalNss.ns_forTest()),
                      BSON("$set" << BSON(CollectionType::kAllowMigrationsFieldName << false)));
    }

    auto coordinator = ReshardingCoordinator::getOrCreate(opCtx, _service, doc.toBSON());
    ASSERT_THROWS_CODE(coordinator->getCompletionFuture().get(opCtx), DBException, 5808201);

    // Check that reshardCollection keeps allowMigrations setting intact.
    {
        DBDirectClient client(opCtx);
        CollectionType collDoc(
            client.findOne(CollectionType::ConfigNS,
                           BSON(CollectionType::kNssFieldName << _originalNss.ns_forTest())));
        ASSERT_FALSE(collDoc.getAllowMigrations());
    }
}

TEST_F(ReshardingCoordinatorServiceTest, MultipleReshardingOperationsFail) {
    const std::vector<CoordinatorStateEnum> states = {CoordinatorStateEnum::kPreparingToDonate,
                                                      CoordinatorStateEnum::kCloning,
                                                      CoordinatorStateEnum::kApplying,
                                                      CoordinatorStateEnum::kBlockingWrites,
                                                      CoordinatorStateEnum::kCommitting};

    auto stateTransitionsGuard =
        std::make_unique<PauseDuringStateTransitions>(controller(), states);
    auto coordinator = initializeAndGetCoordinator();

    // Asserts that a resharding op with same namespace and same shard key fails with
    // ReshardingCoordinatorServiceConflictingOperationInProgress
    ASSERT_THROWS_WITH_CHECK(
        initializeAndGetCoordinator(
            UUID::gen(), _originalNss, _tempNss, _newShardKey, UUID::gen(), _oldShardKey),
        DBException,
        [&](const DBException& ex) {
            ASSERT_EQ(ex.code(),
                      ErrorCodes::ReshardingCoordinatorServiceConflictingOperationInProgress);
            ASSERT_EQ(ex.extraInfo<ReshardingCoordinatorServiceConflictingOperationInProgressInfo>()
                          ->getInstance(),
                      coordinator);
        });

    // Asserts that a resharding op with different namespace and different shard key fails with
    // ConflictingOperationInProgress.
    ASSERT_THROWS_CODE(
        initializeAndGetCoordinator(UUID::gen(),
                                    NamespaceString::createNamespaceString_forTest("db.moo"),
                                    NamespaceString::createNamespaceString_forTest(
                                        "db.system.resharding." + UUID::gen().toString()),
                                    ShardKeyPattern(BSON("shardKeyV1" << 1)),
                                    UUID::gen(),
                                    ShardKeyPattern(BSON("shardKeyV2" << 1))),
        DBException,
        ErrorCodes::ConflictingOperationInProgress);

    // Asserts that a resharding op with same namespace and different shard key fails with
    // ConflictingOperationInProgress.
    ASSERT_THROWS_CODE(
        initializeAndGetCoordinator(UUID::gen(),
                                    _originalNss,
                                    NamespaceString::createNamespaceString_forTest(
                                        "db.system.resharding." + UUID::gen().toString()),
                                    ShardKeyPattern(BSON("shardKeyV1" << 1)),
                                    UUID::gen(),
                                    _oldShardKey),
        DBException,
        ErrorCodes::ConflictingOperationInProgress);

    runReshardingToCompletion(TransitionFunctionMap{}, std::move(stateTransitionsGuard));
}

TEST_F(ReshardingCoordinatorServiceTest, SuccessfullyAbortReshardOperationImmediately) {
    auto pauseBeforeCTHolderInitialization =
        globalFailPointRegistry().find("pauseBeforeCTHolderInitialization");
    auto timesEnteredFailPoint = pauseBeforeCTHolderInitialization->setMode(FailPoint::alwaysOn, 0);
    auto coordinator = initializeAndGetCoordinator();
    coordinator->abort();
    pauseBeforeCTHolderInitialization->waitForTimesEntered(timesEnteredFailPoint + 1);
    pauseBeforeCTHolderInitialization->setMode(FailPoint::off, 0);
    coordinator->getCompletionFuture().wait();
}

TEST_F(ReshardingCoordinatorServiceTest, AbortingReshardingOperationIncrementsMetrics) {
    auto pauseAfterInsertCoordinatorDoc =
        globalFailPointRegistry().find("pauseAfterInsertCoordinatorDoc");
    auto timesEnteredFailPoint = pauseAfterInsertCoordinatorDoc->setMode(FailPoint::alwaysOn, 0);
    auto coordinator = initializeAndGetCoordinator();
    pauseAfterInsertCoordinatorDoc->waitForTimesEntered(timesEnteredFailPoint + 1);
    coordinator->abort();
    pauseAfterInsertCoordinatorDoc->setMode(FailPoint::off, 0);
    coordinator->getCompletionFuture().wait();

    BSONObjBuilder bob;
    ReshardingCumulativeMetrics::getForResharding(operationContext()->getServiceContext())
        ->reportForServerStatus(&bob);
    auto cumulativeMetricsBSON = bob.obj();

    ASSERT_EQ(cumulativeMetricsBSON["resharding"]["countStarted"].numberInt(), 1);
    ASSERT_EQ(cumulativeMetricsBSON["resharding"]["countCanceled"].numberInt(), 1);
}

TEST_F(ReshardingCoordinatorServiceTest, CoordinatorReturnsErrorCode) {
    const std::vector<CoordinatorStateEnum> states = {CoordinatorStateEnum::kPreparingToDonate,
                                                      CoordinatorStateEnum::kCloning,
                                                      CoordinatorStateEnum::kAborting};

    PauseDuringStateTransitions stateTransitionsGuard{controller(), states};

    auto opCtx = operationContext();
    auto coordinator = initializeAndGetCoordinator();

    stateTransitionsGuard.wait(CoordinatorStateEnum::kPreparingToDonate);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kPreparingToDonate);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);

    makeDonorsReadyToDonateWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kCloning);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kCloning);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCloning);

    makeRecipientsReturnErrorWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kAborting);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kAborting);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kAborting);

    makeRecipientsProceedToDone(opCtx);
    makeDonorsProceedToDone(opCtx);

    ASSERT_THROWS_CODE(coordinator->getCompletionFuture().get(opCtx),
                       DBException,
                       ErrorCodes::SnapshotUnavailable);
    BSONObjBuilder bob;
    ReshardingCumulativeMetrics::getForResharding(operationContext()->getServiceContext())
        ->reportForServerStatus(&bob);
    auto cumulativeMetricsBSON = bob.obj();

    ASSERT_EQ(cumulativeMetricsBSON["resharding"]["countStarted"].numberInt(), 1);
    ASSERT_EQ(cumulativeMetricsBSON["resharding"]["countFailed"].numberInt(), 1);
}

TEST_F(ReshardingCoordinatorServiceTest, CoordinatorReturnsErrorCodeAfterRestart) {
    const std::vector<CoordinatorStateEnum> states = {CoordinatorStateEnum::kPreparingToDonate,
                                                      CoordinatorStateEnum::kCloning,
                                                      CoordinatorStateEnum::kAborting};

    PauseDuringStateTransitions stateTransitionsGuard{controller(), states};

    auto opCtx = operationContext();
    auto coordinator = initializeAndGetCoordinator();

    stateTransitionsGuard.wait(CoordinatorStateEnum::kPreparingToDonate);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kPreparingToDonate);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);

    makeDonorsReadyToDonateWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kCloning);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kCloning);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCloning);


    makeRecipientsReturnErrorWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kAborting);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kAborting);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kAborting);


    stepDown(opCtx);
    ASSERT_THROWS_WITH_CHECK(
        coordinator->getCompletionFuture().get(), DBException, [&](const DBException& ex) {
            ASSERT_TRUE(ex.code() == ErrorCodes::CallbackCanceled ||
                        ex.code() == ErrorCodes::InterruptedDueToReplStateChange);
        });

    coordinator.reset();
    stepUp(opCtx);

    makeRecipientsProceedToDone(opCtx);
    makeDonorsProceedToDone(opCtx);

    auto instanceId =
        BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName << _reshardingUUID);
    coordinator = getCoordinator(opCtx, instanceId);

    ASSERT_THROWS_CODE(coordinator->getCompletionFuture().get(opCtx),
                       DBException,
                       ErrorCodes::SnapshotUnavailable);
}

TEST_F(ReshardingCoordinatorServiceTest, ZeroNumRecipientShardsNoInitialChunks) {
    auto donorShardIds = getShardIds();
    auto recipientShardIds = getShardIds();
    std::set<ShardId> recipientShardIdsNoInitialChunks = {};
    auto reshardingOptions =
        ReshardingOptions(donorShardIds, recipientShardIds, recipientShardIdsNoInitialChunks);

    runReshardingToCompletionAssertApproxToCopyMetrics(reshardingOptions);
}

TEST_F(ReshardingCoordinatorServiceTest, NonZeroNumRecipientShardsNoInitialChunks) {
    auto donorShardIds = getShardIds();
    auto recipientShardIds = getShardIds();
    std::set<ShardId> recipientShardIdsNoInitialChunks = {shardId0};
    auto reshardingOptions =
        ReshardingOptions(donorShardIds, recipientShardIds, recipientShardIdsNoInitialChunks);

    runReshardingToCompletionAssertApproxToCopyMetrics(reshardingOptions);
}

TEST_F(ReshardingCoordinatorServiceTest, CoordinatorHonorsCriticalSectionTimeoutAfterStepUp) {
    const std::vector<CoordinatorStateEnum> states = {
        CoordinatorStateEnum::kPreparingToDonate,
        CoordinatorStateEnum::kCloning,
        CoordinatorStateEnum::kApplying,
        CoordinatorStateEnum::kBlockingWrites,
        CoordinatorStateEnum::kAborting,
    };

    PauseDuringStateTransitions stateTransitionsGuard{controller(), states};

    auto opCtx = operationContext();

    auto reshardingOptions = makeDefaultReshardingOptions();
    auto coordinator = initializeAndGetCoordinator(_reshardingUUID,
                                                   _originalNss,
                                                   _tempNss,
                                                   _newShardKey,
                                                   _originalUUID,
                                                   _oldShardKey,
                                                   reshardingOptions);
    auto instanceId =
        BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName << _reshardingUUID);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kPreparingToDonate);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kPreparingToDonate);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);

    makeDonorsReadyToDonateWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kCloning);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kCloning);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCloning);

    makeRecipientsFinishedCloningWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kApplying);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kApplying);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kApplying);

    coordinator->onOkayToEnterCritical();

    stateTransitionsGuard.wait(CoordinatorStateEnum::kBlockingWrites);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kBlockingWrites);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kBlockingWrites);

    stepDown(opCtx);

    coordinator.reset();

    // Reset the critical section timeout to earlier time to guarantee timeout event.
    auto coordDoc = getCoordinatorDoc(opCtx);
    auto expiresAt = coordDoc.getCriticalSectionExpiresAt();
    auto now = Date_t::now();
    invariant(expiresAt && expiresAt.value() > now);
    LOGV2_DEBUG(9697800, 5, "Resetting critical section expiry time", "expiresAt"_attr = now);

    BSONObj updates = BSON(
        "$set" << BSON(ReshardingCoordinatorDocument::kCriticalSectionExpiresAtFieldName << now));

    updateCoordinatorDoc(opCtx, coordDoc.getReshardingUUID(), updates);

    stepUp(opCtx);

    instanceId = BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName << _reshardingUUID);
    coordinator = getCoordinator(opCtx, instanceId);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kAborting);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kAborting);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kAborting);

    makeRecipientsProceedToDone(opCtx);
    makeDonorsProceedToDone(opCtx);

    ASSERT_THROWS_CODE(coordinator->getCompletionFuture().get(opCtx),
                       DBException,
                       ErrorCodes::ReshardingCriticalSectionTimeout);
}

TEST_F(ReshardingCoordinatorServiceTest, FeatureFlagReshardingCloneNoRefreshSendsCloneCmd) {
    const std::vector<CoordinatorStateEnum> states = {
        CoordinatorStateEnum::kPreparingToDonate,
    };

    RAIIServerParameterControllerForTest noRefreshFeatureFlagController(
        "featureFlagReshardingCloneNoRefresh", true);
    auto pauseBeforeTellingRecipientsToClone =
        globalFailPointRegistry().find("reshardingPauseBeforeTellingRecipientsToClone");
    auto timesEnteredFailPoint =
        pauseBeforeTellingRecipientsToClone->setMode(FailPoint::alwaysOn, 0);

    PauseDuringStateTransitions stateTransitionsGuard{controller(), states};

    auto opCtx = operationContext();

    auto reshardingOptions = makeDefaultReshardingOptions();
    auto coordinator = initializeAndGetCoordinator(_reshardingUUID,
                                                   _originalNss,
                                                   _tempNss,
                                                   _newShardKey,
                                                   _originalUUID,
                                                   _oldShardKey,
                                                   reshardingOptions);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kPreparingToDonate);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kPreparingToDonate);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);

    makeDonorsReadyToDonateWithAssert(opCtx);
    pauseBeforeTellingRecipientsToClone->waitForTimesEntered(timesEnteredFailPoint + 1);
    stepDown(opCtx);
    pauseBeforeTellingRecipientsToClone->setMode(FailPoint::off, 0);
}

class ReshardingCoordinatorServiceFailCloningVerificationTest
    : public ReshardingCoordinatorServiceTestBase {
public:
    ExternalStateForTest::Options getExternalStateOptions() const override {
        return {.documentsToCopy = documentsToCopy,
                .documentsDelta = documentsDelta,
                .verifyClonedErrorCode = verifyClonedErrorCode};
    }

protected:
    const ErrorCodes::Error verifyClonedErrorCode{9858201};
};

TEST_F(ReshardingCoordinatorServiceFailCloningVerificationTest, AbortIfPerformVerification) {
    const std::vector<CoordinatorStateEnum> states = {CoordinatorStateEnum::kPreparingToDonate,
                                                      CoordinatorStateEnum::kCloning,
                                                      CoordinatorStateEnum::kAborting};

    PauseDuringStateTransitions stateTransitionsGuard{controller(), states};

    auto opCtx = operationContext();

    auto reshardingOptions = makeDefaultReshardingOptions();
    auto coordinator = initializeAndGetCoordinator(_reshardingUUID,
                                                   _originalNss,
                                                   _tempNss,
                                                   _newShardKey,
                                                   _originalUUID,
                                                   _oldShardKey,
                                                   reshardingOptions);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kPreparingToDonate);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kPreparingToDonate);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);

    makeDonorsReadyToDonateWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kCloning);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kCloning);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCloning);

    makeRecipientsFinishedCloningWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kAborting);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kAborting);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kAborting);

    makeRecipientsProceedToDone(opCtx);
    makeDonorsProceedToDone(opCtx);

    ASSERT_THROWS_CODE(
        coordinator->getCompletionFuture().get(opCtx), DBException, verifyClonedErrorCode);
}

TEST_F(ReshardingCoordinatorServiceFailCloningVerificationTest, CommitIfNotPerformVerification) {
    auto reshardingOptions = makeDefaultReshardingOptions();
    reshardingOptions.performVerification = false;

    runReshardingToCompletion(TransitionFunctionMap{},
                              nullptr /* stateTransitionsGuard */,
                              {CoordinatorStateEnum::kPreparingToDonate,
                               CoordinatorStateEnum::kCloning,
                               CoordinatorStateEnum::kApplying,
                               CoordinatorStateEnum::kBlockingWrites,
                               CoordinatorStateEnum::kCommitting},
                              reshardingOptions);
}

class ReshardingCoordinatorServiceFailFinalVerificationTest
    : public ReshardingCoordinatorServiceTestBase {
public:
    ExternalStateForTest::Options getExternalStateOptions() const override {
        return {.documentsToCopy = documentsToCopy,
                .documentsDelta = documentsDelta,
                .verifyFinalErrorCode = verifyFinalErrorCode};
    }

protected:
    const ErrorCodes::Error verifyFinalErrorCode{9858601};
};

TEST_F(ReshardingCoordinatorServiceFailFinalVerificationTest, AbortIfPerformVerification) {
    const std::vector<CoordinatorStateEnum> states = {CoordinatorStateEnum::kPreparingToDonate,
                                                      CoordinatorStateEnum::kCloning,
                                                      CoordinatorStateEnum::kBlockingWrites,
                                                      CoordinatorStateEnum::kAborting};

    PauseDuringStateTransitions stateTransitionsGuard{controller(), states};

    auto opCtx = operationContext();

    auto reshardingOptions = makeDefaultReshardingOptions();
    auto coordinator = initializeAndGetCoordinator(_reshardingUUID,
                                                   _originalNss,
                                                   _tempNss,
                                                   _newShardKey,
                                                   _originalUUID,
                                                   _oldShardKey,
                                                   reshardingOptions);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kPreparingToDonate);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kPreparingToDonate);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);

    makeDonorsReadyToDonateWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kCloning);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kCloning);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCloning);

    makeRecipientsFinishedCloningWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kApplying);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kApplying);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kApplying);

    coordinator->onOkayToEnterCritical();

    stateTransitionsGuard.wait(CoordinatorStateEnum::kBlockingWrites);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kBlockingWrites);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kBlockingWrites);

    makeRecipientsBeInStrictConsistencyWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kAborting);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kAborting);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kAborting);

    makeRecipientsProceedToDone(opCtx);
    makeDonorsProceedToDone(opCtx);

    ASSERT_THROWS_CODE(
        coordinator->getCompletionFuture().get(opCtx), DBException, verifyFinalErrorCode);
}

TEST_F(ReshardingCoordinatorServiceFailFinalVerificationTest, CommitIfNotPerformVerification) {
    auto reshardingOptions = makeDefaultReshardingOptions();
    reshardingOptions.performVerification = false;

    runReshardingToCompletion(TransitionFunctionMap{},
                              nullptr /* stateTransitionsGuard */,
                              {CoordinatorStateEnum::kPreparingToDonate,
                               CoordinatorStateEnum::kCloning,
                               CoordinatorStateEnum::kApplying,
                               CoordinatorStateEnum::kBlockingWrites,
                               CoordinatorStateEnum::kCommitting},
                              reshardingOptions);
}

TEST_F(ReshardingCoordinatorServiceTest,
       CoordinatorDocDonorShardEntriesShouldHaveDocumentsToCopyAndFinal) {
    auto reshardingOptions = makeDefaultReshardingOptions();
    auto checkPersistentStates = [&] {
        auto opCtx = operationContext();
        auto coordinatorDoc = getCoordinatorDoc(opCtx);
        checkDonorDocumentsToCopyMetrics(coordinatorDoc);
        checkDonorDocumentsFinalMetrics(coordinatorDoc);
    };

    auto transitionFunctions =
        TransitionFunctionMap{{CoordinatorStateEnum::kCloning, checkPersistentStates},
                              {CoordinatorStateEnum::kApplying, checkPersistentStates},
                              {CoordinatorStateEnum::kBlockingWrites, checkPersistentStates},
                              {CoordinatorStateEnum::kCommitting, checkPersistentStates}};
    auto states = {CoordinatorStateEnum::kPreparingToDonate,
                   CoordinatorStateEnum::kCloning,
                   CoordinatorStateEnum::kApplying,
                   CoordinatorStateEnum::kBlockingWrites,
                   CoordinatorStateEnum::kCommitting};
    runReshardingToCompletion(
        transitionFunctions, nullptr /* stateTransitionsGuard */, states, reshardingOptions);
}

TEST_F(ReshardingCoordinatorServiceTest, UnrecoverableErrorDuringPreparingToDonate) {
    runReshardingWithUnrecoverableError(CoordinatorStateEnum::kPreparingToDonate,
                                        kEstablishAllDonorsAsParticipants);
}

TEST_F(ReshardingCoordinatorServiceTest, UnrecoverableErrorDuringCloning) {
    runReshardingWithUnrecoverableError(CoordinatorStateEnum::kCloning,
                                        kGetDocumentsToCopyFromDonors);
}

TEST_F(ReshardingCoordinatorServiceTest, UnrecoverableErrorDuringApplying) {
    runReshardingWithUnrecoverableError(CoordinatorStateEnum::kApplying, kTellAllDonorsToRefresh);
}

TEST_F(ReshardingCoordinatorServiceTest, UnrecoverableErrorDuringBlockingWrites) {
    runReshardingWithUnrecoverableError(CoordinatorStateEnum::kBlockingWrites,
                                        kGetDocumentsDeltaFromDonors);
}

class ReshardingCoordinatorServiceFailGetDocumentsToCopy
    : public ReshardingCoordinatorServiceTestBase {
public:
    ExternalStateForTest::Options getExternalStateOptions() const override {
        return {.documentsToCopy = documentsToCopy,
                .documentsDelta = documentsDelta,
                .getDocumentsToCopyErrorCode = getDocumentsToCopyErrorCode};
    }

protected:
    const ErrorCodes::Error getDocumentsToCopyErrorCode{9858108};
};

TEST_F(ReshardingCoordinatorServiceFailGetDocumentsToCopy, AbortIfPerformVerification) {
    const std::vector<CoordinatorStateEnum> states = {CoordinatorStateEnum::kPreparingToDonate,
                                                      CoordinatorStateEnum::kCloning,
                                                      CoordinatorStateEnum::kAborting};

    PauseDuringStateTransitions stateTransitionsGuard{controller(), states};

    auto opCtx = operationContext();

    auto reshardingOptions = makeDefaultReshardingOptions();
    auto coordinator = initializeAndGetCoordinator(_reshardingUUID,
                                                   _originalNss,
                                                   _tempNss,
                                                   _newShardKey,
                                                   _originalUUID,
                                                   _oldShardKey,
                                                   reshardingOptions);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kPreparingToDonate);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kPreparingToDonate);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);

    makeDonorsReadyToDonateWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kCloning);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kCloning);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCloning);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kAborting);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kAborting);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kAborting);

    makeRecipientsProceedToDone(opCtx);
    makeDonorsProceedToDone(opCtx);

    ASSERT_THROWS_CODE(
        coordinator->getCompletionFuture().get(opCtx), DBException, getDocumentsToCopyErrorCode);
}

TEST_F(
    ReshardingCoordinatorServiceFailGetDocumentsToCopy,
    CoordinatorDocDonorShardEntriesShouldNotHaveDocumentsToCopyAndFinalIfNotPerformVerification) {
    auto reshardingOptions = makeDefaultReshardingOptions();
    reshardingOptions.performVerification = false;
    auto checkPersistentStates = [&] {
        auto opCtx = operationContext();
        auto coordinatorDoc = getCoordinatorDoc(opCtx);
        checkDonorDocumentsToCopyMetrics(coordinatorDoc);
        checkDonorDocumentsFinalMetrics(coordinatorDoc);
    };

    auto transitionFunctions =
        TransitionFunctionMap{{CoordinatorStateEnum::kCloning, checkPersistentStates},
                              {CoordinatorStateEnum::kApplying, checkPersistentStates},
                              {CoordinatorStateEnum::kBlockingWrites, checkPersistentStates},
                              {CoordinatorStateEnum::kCommitting, checkPersistentStates}};
    auto states = {CoordinatorStateEnum::kPreparingToDonate,
                   CoordinatorStateEnum::kCloning,
                   CoordinatorStateEnum::kApplying,
                   CoordinatorStateEnum::kBlockingWrites,
                   CoordinatorStateEnum::kCommitting};
    runReshardingToCompletion(
        transitionFunctions, nullptr /* stateTransitionsGuard */, states, reshardingOptions);
}

class ReshardingCoordinatorServiceReturnZeroFromGetDocumentsToCopy
    : public ReshardingCoordinatorServiceTestBase {
public:
    ExternalStateForTest::Options getExternalStateOptions() const override {
        return {.documentsToCopy = documentsToCopy, .documentsDelta = documentsDelta};
    }

protected:
    const std::map<ShardId, int64_t> documentsToCopy = {
        {shardId0, 0},
        {shardId1, 0},
    };
};

TEST_F(ReshardingCoordinatorServiceReturnZeroFromGetDocumentsToCopy,
       CoordinatorDocDonorShardEntriesShouldHaveDocumentsToCopyEvenWithZeroDocuments) {
    auto reshardingOptions = makeDefaultReshardingOptions();
    auto checkPersistentStates = [&] {
        auto opCtx = operationContext();
        auto coordinatorDoc = getCoordinatorDoc(opCtx);
        if (coordinatorDoc.getState() >= CoordinatorStateEnum::kApplying) {
            for (const auto& donorShardEntry : coordinatorDoc.getDonorShards()) {
                ASSERT_TRUE(donorShardEntry.getDocumentsToCopy().has_value());
                ASSERT_EQUALS(*donorShardEntry.getDocumentsToCopy(), (int64_t)0);
            }
        }
    };

    auto transitionFunctions =
        TransitionFunctionMap{{CoordinatorStateEnum::kCloning, checkPersistentStates},
                              {CoordinatorStateEnum::kApplying, checkPersistentStates},
                              {CoordinatorStateEnum::kBlockingWrites, checkPersistentStates},
                              {CoordinatorStateEnum::kCommitting, checkPersistentStates}};
    auto states = {CoordinatorStateEnum::kPreparingToDonate,
                   CoordinatorStateEnum::kCloning,
                   CoordinatorStateEnum::kApplying,
                   CoordinatorStateEnum::kBlockingWrites,
                   CoordinatorStateEnum::kCommitting};
    runReshardingToCompletion(
        transitionFunctions, nullptr /* stateTransitionsGuard */, states, reshardingOptions);
}

class ReshardingCoordinatorServiceFailGetDocumentsDelta
    : public ReshardingCoordinatorServiceTestBase {
public:
    ExternalStateForTest::Options getExternalStateOptions() const override {
        return {.documentsToCopy = documentsToCopy,
                .documentsDelta = documentsDelta,
                .getDocumentsDeltaErrorCode = getDocumentsDeltaErrorCode};
    }

protected:
    const ErrorCodes::Error getDocumentsDeltaErrorCode{9858608};
};

TEST_F(ReshardingCoordinatorServiceFailGetDocumentsDelta, AbortIfPerformVerification) {
    const std::vector<CoordinatorStateEnum> states = {CoordinatorStateEnum::kPreparingToDonate,
                                                      CoordinatorStateEnum::kCloning,
                                                      CoordinatorStateEnum::kBlockingWrites,
                                                      CoordinatorStateEnum::kAborting};

    PauseDuringStateTransitions stateTransitionsGuard{controller(), states};

    auto opCtx = operationContext();

    auto reshardingOptions = makeDefaultReshardingOptions();
    auto coordinator = initializeAndGetCoordinator(_reshardingUUID,
                                                   _originalNss,
                                                   _tempNss,
                                                   _newShardKey,
                                                   _originalUUID,
                                                   _oldShardKey,
                                                   reshardingOptions);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kPreparingToDonate);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kPreparingToDonate);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);

    makeDonorsReadyToDonateWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kCloning);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kCloning);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCloning);

    makeRecipientsFinishedCloningWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kApplying);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kApplying);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kApplying);

    coordinator->onOkayToEnterCritical();

    stateTransitionsGuard.wait(CoordinatorStateEnum::kBlockingWrites);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kBlockingWrites);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kBlockingWrites);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kAborting);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kAborting);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kAborting);

    makeRecipientsProceedToDone(opCtx);
    makeDonorsProceedToDone(opCtx);

    ASSERT_THROWS_CODE(
        coordinator->getCompletionFuture().get(opCtx), DBException, getDocumentsDeltaErrorCode);
}

TEST_F(ReshardingCoordinatorServiceFailGetDocumentsDelta,
       CoordinatorDocDonorShardEntriesShouldNotHaveDocumentsFinalIfNotPerformVerification) {
    auto reshardingOptions = makeDefaultReshardingOptions();
    reshardingOptions.performVerification = false;
    auto checkPersistentStates = [&] {
        auto opCtx = operationContext();
        auto coordinatorDoc = getCoordinatorDoc(opCtx);
        checkDonorDocumentsFinalMetrics(coordinatorDoc);
    };

    auto transitionFunctions =
        TransitionFunctionMap{{CoordinatorStateEnum::kCloning, checkPersistentStates},
                              {CoordinatorStateEnum::kApplying, checkPersistentStates},
                              {CoordinatorStateEnum::kBlockingWrites, checkPersistentStates},
                              {CoordinatorStateEnum::kCommitting, checkPersistentStates}};
    auto states = {CoordinatorStateEnum::kPreparingToDonate,
                   CoordinatorStateEnum::kCloning,
                   CoordinatorStateEnum::kApplying,
                   CoordinatorStateEnum::kBlockingWrites,
                   CoordinatorStateEnum::kCommitting};
    runReshardingToCompletion(
        transitionFunctions, nullptr /* stateTransitionsGuard */, states, reshardingOptions);
}

}  // namespace
}  // namespace mongo
