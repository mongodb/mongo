/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/resharding/resharding_coordinator.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_external_state.h"
#include "mongo/db/s/resharding/resharding_op_observer.h"
#include "mongo/db/s/resharding/resharding_service_test_helpers.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/executor/mock_async_rpc.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {
namespace resharding_coordinator_test {

class ExternalStateForTest : public ReshardingCoordinatorExternalState {
public:
    struct Options {
        std::map<ShardId, int64_t> documentsToCopy;
        std::map<ShardId, int64_t> documentsDelta;
        boost::optional<ErrorCodes::Error> getDocumentsToCopyErrorCode;
        boost::optional<ErrorCodes::Error> getDocumentsDeltaErrorCode;
        boost::optional<ErrorCodes::Error> verifyClonedErrorCode;
        boost::optional<ErrorCodes::Error> verifyFinalErrorCode;
        bool blockInGetDocumentsDelta = false;
    };

    enum class ExternalFunction {
        kTellAllDonorsToRefresh,
        kEstablishAllDonorsAsParticipants,
        kEstablishAllRecipientsAsParticipants,
        kGetDocumentsToCopyFromDonors,
        kGetDocumentsDeltaFromDonors,
    };

    explicit ExternalStateForTest(Options options)
        : ReshardingCoordinatorExternalState(), _options(std::move(options)) {}

    ParticipantShardsAndChunks calculateParticipantShardsAndChunks(
        OperationContext* opCtx,
        const ReshardingCoordinatorDocument& coordinatorDoc,
        std::vector<ReshardingZoneType> zones) override {
        std::vector<ChunkType> initialChunks;
        auto version = calculateChunkVersionForInitialChunks(opCtx);

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
        return ParticipantShardsAndChunks{
            coordinatorDoc.getDonorShards(), coordinatorDoc.getRecipientShards(), initialChunks};
    }

    bool searchIndexExistsForCollection(OperationContext* opCtx, const NamespaceString& nss) {
        if (_searchIndexResults.empty()) {
            return _searchIndexDefaultResult;
        }
        auto result = std::move(_searchIndexResults.front());
        _searchIndexResults.erase(_searchIndexResults.begin());
        return uassertStatusOK(result);
    }

    void pushSearchIndexResult(bool result) {
        _searchIndexResults.push_back(result);
    }

    void pushSearchIndexError(ErrorCodes::Error errorCode) {
        _searchIndexResults.push_back(
            Status{errorCode, "Failing call to searchIndexExistsForCollection"});
    }

    void tellAllDonorsToRefresh(OperationContext* opCtx,
                                const NamespaceString& sourceNss,
                                const UUID& reshardingUUID,
                                const std::vector<DonorShardEntry>& donorShards,
                                const std::shared_ptr<executor::TaskExecutor>& executor,
                                CancellationToken token) override {
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kTellAllDonorsToRefresh);
        resharding::sendFlushReshardingStateChangeToShards(
            opCtx,
            sourceNss,
            reshardingUUID,
            resharding::extractShardIdsFromParticipantEntries(donorShards),
            executor,
            token);
    }

    void tellAllRecipientsToRefresh(OperationContext* opCtx,
                                    const NamespaceString& nssToRefresh,
                                    const UUID& reshardingUUID,
                                    const std::vector<RecipientShardEntry>& recipientShards,
                                    const std::shared_ptr<executor::TaskExecutor>& executor,
                                    CancellationToken token) override {
        resharding::sendFlushReshardingStateChangeToShards(
            opCtx,
            nssToRefresh,
            reshardingUUID,
            resharding::extractShardIdsFromParticipantEntries(recipientShards),
            executor,
            token);
    }

    void establishAllDonorsAsParticipants(OperationContext* opCtx,
                                          const NamespaceString& sourceNss,
                                          const std::vector<DonorShardEntry>& donorShards,
                                          const std::shared_ptr<executor::TaskExecutor>& executor,
                                          CancellationToken token) override {
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kEstablishAllDonorsAsParticipants);
        resharding::sendFlushRoutingTableCacheUpdatesToShards(
            opCtx,
            sourceNss,
            resharding::extractShardIdsFromParticipantEntries(donorShards),
            executor,
            token);
    }

    void establishAllRecipientsAsParticipants(
        OperationContext* opCtx,
        const NamespaceString& tempNss,
        const std::vector<RecipientShardEntry>& recipientShards,
        const std::shared_ptr<executor::TaskExecutor>& executor,
        CancellationToken token) override {
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kEstablishAllRecipientsAsParticipants);
        resharding::sendFlushRoutingTableCacheUpdatesToShards(
            opCtx,
            tempNss,
            resharding::extractShardIdsFromParticipantEntries(recipientShards),
            executor,
            token);
    }

    std::map<ShardId, int64_t> getDocumentsToCopyFromDonors(
        OperationContext* opCtx,
        const std::shared_ptr<executor::TaskExecutor>&,
        CancellationToken,
        const UUID&,
        const NamespaceString&,
        const Timestamp&,
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
        const std::shared_ptr<executor::TaskExecutor>&,
        CancellationToken,
        const UUID&,
        const NamespaceString&,
        const std::vector<ShardId>& shardIds) override {
        _maybeThrowErrorForFunction(opCtx, ExternalFunction::kGetDocumentsDeltaFromDonors);
        if (_options.getDocumentsDeltaErrorCode) {
            uasserted(*_options.getDocumentsDeltaErrorCode, "Failing call to getDocumentsDelta");
        }

        if (_options.blockInGetDocumentsDelta) {
            std::unique_lock lk(_mutex);
            opCtx->waitForConditionOrInterrupt(_blockInGetDocumentsDeltaCV, lk, [this] {
                return !_doKeepBlockingInGetDocumentsDelta;
            });
        }

        std::map<ShardId, int64_t> docsDelta;
        for (const auto& shardId : shardIds) {
            auto it = _options.documentsDelta.find(shardId);
            ASSERT(it != _options.documentsDelta.end());
            docsDelta.emplace(shardId, it->second);
        }
        return docsDelta;
    }

    void verifyClonedCollection(OperationContext*,
                                const std::shared_ptr<executor::TaskExecutor>&,
                                CancellationToken,
                                const ReshardingCoordinatorDocument&) override {
        if (_options.verifyClonedErrorCode) {
            uasserted(*_options.verifyClonedErrorCode, "Failing cloned collection verification");
        }
    }

    void verifyFinalCollection(OperationContext*, const ReshardingCoordinatorDocument&) override {
        if (_options.verifyFinalErrorCode) {
            uasserted(*_options.verifyFinalErrorCode, "Failing final collection verification");
        }
    }

    // TODO (SERVER-121209) Update this function according to the changes applied under
    // SERVER-121209
    void stopMigrations(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const UUID&,
                        const OperationSessionInfo&) override {
        DBDirectClient client(opCtx);
        client.update(NamespaceString::kConfigsvrCollectionsNamespace,
                      BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                               nss, SerializationContext::stateDefault())),
                      BSON("$set" << BSON(CollectionType::kAllowMigrationsFieldName << false)));
        _bumpOneChunk(opCtx, nss);
    }

    // TODO (SERVER-121209) Update this function according to the changes applied under
    // SERVER-121209
    void resumeMigrations(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const UUID&,
                          const OperationSessionInfo&) override {
        DBDirectClient client(opCtx);
        client.update(NamespaceString::kConfigsvrCollectionsNamespace,
                      BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                               nss, SerializationContext::stateDefault())),
                      BSON("$unset" << BSON(CollectionType::kAllowMigrationsFieldName << "")));
        _bumpOneChunk(opCtx, nss);
    }

    std::unique_ptr<CausalityBarrier> buildCausalityBarrier(std::vector<ShardId>,
                                                            std::shared_ptr<executor::TaskExecutor>,
                                                            CancellationToken) override {
        _causalityBarrierInvokeCount.fetch_add(1);
        class NoOpBarrier : public CausalityBarrier {
        public:
            // Unit tests have no real shard servers. Skip the no-op retryable write to avoid
            // network errors.
            void perform(OperationContext*, const OperationSessionInfo&) override {}
        };
        return std::make_unique<NoOpBarrier>();
    }

    int getCausalityBarrierInvokeCount() const {
        return _causalityBarrierInvokeCount.load();
    }

    void throwUnrecoverableErrorIn(CoordinatorStateEnum phase, ExternalFunction func) {
        _errorFunction = std::make_tuple(phase, func);
    }

    void unblockGetDocumentsDelta() {
        std::lock_guard lk(_mutex);
        _doKeepBlockingInGetDocumentsDelta = false;
        _blockInGetDocumentsDeltaCV.notify_all();
    }

private:
    const Options _options;

    std::atomic<int> _causalityBarrierInvokeCount{0};

    boost::optional<std::tuple<CoordinatorStateEnum, ExternalFunction>> _errorFunction =
        boost::none;

    std::mutex _mutex;
    stdx::condition_variable _blockInGetDocumentsDeltaCV;
    bool _doKeepBlockingInGetDocumentsDelta = true;

    std::vector<StatusWith<bool>> _searchIndexResults;
    bool _searchIndexDefaultResult{false};

    // Bumps the minor version of one chunk belonging to the collection at 'nss' in config.chunks.
    // This keeps the catalog cache invariant satisfied after allowMigrations is toggled: the
    // invariant requires that any change to allowMigrations is accompanied by a placement version
    // bump so that cached routing tables are invalidated on the next refresh.
    void _bumpOneChunk(OperationContext* opCtx, const NamespaceString& nss) {
        DBDirectClient client(opCtx);
        auto collDoc =
            client.findOne(NamespaceString::kConfigsvrCollectionsNamespace,
                           BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                                    nss, SerializationContext::stateDefault())));
        if (collDoc.isEmpty())
            return;
        auto collUUID = uassertStatusOK(UUID::parse(collDoc[CollectionType::kUuidFieldName]));
        FindCommandRequest findChunk(NamespaceString::kConfigsvrChunksNamespace);
        findChunk.setFilter(BSON(ChunkType::collectionUUID() << collUUID));
        findChunk.setSort(BSON(ChunkType::lastmod.name() << -1));
        auto chunkDoc = client.findOne(std::move(findChunk));
        if (chunkDoc.isEmpty())
            return;
        auto current = chunkDoc[ChunkType::lastmod.name()].timestamp();
        Timestamp bumped(current.getSecs(), current.getInc() + 1);
        client.update(NamespaceString::kConfigsvrChunksNamespace,
                      BSON("_id" << chunkDoc["_id"]),
                      BSON("$set" << BSON(ChunkType::lastmod.name() << bumped)));
    }

    CoordinatorStateEnum _getCurrentPhaseOnDisk(OperationContext* opCtx) {
        DBDirectClient client(opCtx);
        auto doc = client.findOne(NamespaceString::kConfigReshardingOperationsNamespace, BSONObj{});
        IDLParserContext errCtx("reshardingCoordFromTest");
        return ReshardingCoordinatorDocument::parse(doc, errCtx).getState();
    }

    void _maybeThrowErrorForFunction(OperationContext* opCtx, ExternalFunction func) {
        if (_errorFunction) {
            auto [expectedPhase, expectedFunction] = *_errorFunction;
            if (_getCurrentPhaseOnDisk(opCtx) == expectedPhase && func == expectedFunction) {
                uasserted(ErrorCodes::InternalError, "Simulating unrecoverable error for testing");
            }
        }
    }
};

class ReshardingCoordinatorServiceForTest : public ReshardingCoordinatorService {
public:
    ReshardingCoordinatorServiceForTest(ServiceContext* serviceContext,
                                        std::shared_ptr<ExternalStateForTest> externalState)
        : ReshardingCoordinatorService(serviceContext),
          _serviceContext(serviceContext),
          _externalState(std::move(externalState)) {}

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

/**
 * Wraps a delegate AsyncRPCRunner and asserts that every command sent by the
 * ReshardingCoordinator carries both `lsid` and `txnNumber` (i.e. OperationSessionInfo),
 * which are required for replay protection.
 */
class OsiCheckingAsyncRPCRunner : public async_rpc::detail::AsyncRPCRunner {
public:
    explicit OsiCheckingAsyncRPCRunner(std::unique_ptr<async_rpc::detail::AsyncRPCRunner> inner)
        : _inner(std::move(inner)) {}

    ExecutorFuture<async_rpc::detail::AsyncRPCInternalResponse> _sendCommand(
        std::shared_ptr<executor::TaskExecutor> exec,
        CancellationToken token,
        OperationContext* opCtx,
        async_rpc::Targeter* targeter,
        const TargetingMetadata& targetingMetadata,
        const DatabaseName& dbName,
        BSONObj cmdBSON,
        BatonHandle baton,
        boost::optional<UUID> clientOperationKey) final {
        auto cmdName = cmdBSON.firstElementFieldNameStringData();
        if (!kOsiExemptCommands.count(cmdName) &&
            resharding::gFeatureFlagReshardingInitNoRefresh.isEnabled(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            ASSERT(cmdBSON.hasField("lsid"))
                << "ReshardingCoordinator RPC '" << cmdName << "' missing lsid (OSI)";
            ASSERT(cmdBSON.hasField("txnNumber"))
                << "ReshardingCoordinator RPC '" << cmdName << "' missing txnNumber (OSI)";
        }
        return _inner->_sendCommand(std::move(exec),
                                    std::move(token),
                                    opCtx,
                                    targeter,
                                    targetingMetadata,
                                    dbName,
                                    std::move(cmdBSON),
                                    std::move(baton),
                                    clientOperationKey);
    }

private:
    // Commands sent by the coordinator that are exempt from carrying OSI.
    // _flushReshardingStateChange is idempotent, so OSI-based deduplication is unnecessary.
    // One instance is also sent post-commit on a best-effort basis, after the coordinator
    // document and its associated session have already been removed, making it impossible
    // to include OSI. This command is expected to be removed once reshardingFields are no
    // longer written to config.collections, when shards authoritatively manage their own
    // filtering metadata.
    static inline const StringSet kOsiExemptCommands{
        "_flushReshardingStateChange",
    };

    std::unique_ptr<async_rpc::detail::AsyncRPCRunner> _inner;
};

/**
 * Provenance-agnostic base fixture for reshardingCoordinator tests.
 */
class ReshardingCoordinatorServiceTestCommon : public service_context_test::WithSetupTransportLayer,
                                               public ConfigServerTestFixture {
public:
    using CoordinatorStateTransitionController =
        resharding_service_test_helpers::StateTransitionController<CoordinatorStateEnum>;
    using OpObserverForTest = resharding_service_test_helpers::
        StateTransitionControllerOpObserver<CoordinatorStateEnum, ReshardingCoordinatorDocument>;

    virtual std::vector<ShardId> getShardIds() const = 0;
    virtual ExternalStateForTest::Options getExternalStateOptions() const = 0;

    std::unique_ptr<repl::PrimaryOnlyService> makeService(
        ServiceContext* serviceContext, std::shared_ptr<ExternalStateForTest> externalState) {
        async_rpc::detail::AsyncRPCRunner::set(
            serviceContext,
            std::make_unique<OsiCheckingAsyncRPCRunner>(
                std::make_unique<async_rpc::NoopMockAsyncRPCRunner>()));
        return std::make_unique<ReshardingCoordinatorServiceForTest>(serviceContext, externalState);
    }

    void setUp() override {
        ConfigServerTestFixture::setUp();

        std::vector<ShardType> shards;
        for (const auto& id : getShardIds()) {
            ShardType s;
            s.setName(id.toString());
            s.setHost(id.toString() + ":1234");
            shards.push_back(std::move(s));
        }
        setupShards(shards);

        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});
        client.createCollection(NamespaceString::kConfigReshardingOperationsNamespace);
        client.createCollection(NamespaceString::kConfigsvrCollectionsNamespace);

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(opCtx)->initializeIfNeeded(opCtx, /*term*/ 1);

        _controller = std::make_shared<CoordinatorStateTransitionController>();
        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());
        repl::createOplog(opCtx);

        _opObserverRegistry =
            dynamic_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        invariant(_opObserverRegistry);

        _opObserverRegistry->addObserver(std::make_unique<ReshardingOpObserver>());
        _opObserverRegistry->addObserver(std::make_unique<OpObserverForTest>(
            _controller,
            NamespaceString::kConfigReshardingOperationsNamespace,
            [](const ReshardingCoordinatorDocument& doc) { return doc.getState(); }));
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
        externalState()->unblockGetDocumentsDelta();
        TransactionCoordinatorService::get(operationContext())->interruptForStepDown();
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
                                         PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kRead},
            MODE_IS);
        ASSERT_TRUE(coordinatorColl.exists());
        ASSERT_TRUE(bool(coordinatorColl.getCollectionPtr()->isEmpty(opCtx)));
    }

    CollectionType getCollectionCatalogEntry(OperationContext* opCtx) {
        DBDirectClient client(opCtx);
        auto doc = client.findOne(NamespaceString::kConfigsvrCollectionsNamespace,
                                  BSON(CollectionType::kNssFieldName << _originalNss.ns_forTest()));
        return CollectionType{std::move(doc)};
    }

    CollectionType getTemporaryCollectionCatalogEntry(
        OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {
        DBDirectClient client(opCtx);
        auto doc = client.findOne(NamespaceString::kConfigsvrCollectionsNamespace,
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
        while (true) {
            auto coordinatorDoc = getCoordinatorDocBSON(opCtx);
            auto currentState = coordinatorDoc.getStringField("state");
            if (currentState == idl::serialize(state)) {
                break;
            }
            sleepmillis(50);
        }
    }

    void makeDonorsReadyToDonateWithAssert(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);
        ASSERT_NE(coordDoc.getStartTime(), Date_t::min());

        auto donorShards = coordDoc.getDonorShards();
        auto keyPre = [](StringData suffix) {
            return fmt::format("{}.$[].mutableState.{}",
                               ReshardingCoordinatorDocument::kDonorShardsFieldName,
                               suffix);
        };

        BSONObjBuilder updates;
        {
            BSONObjBuilder{updates.subobjStart("$set")}
                .append(keyPre("state"), idl::serialize(DonorStateEnum::kDonatingInitialData))
                .append(keyPre("minFetchTimestamp"), _cloneTimestamp)
                .append(keyPre("bytesToClone"),
                        static_cast<long long>(totalApproxBytesToClone / donorShards.size()))
                .append(keyPre("documentsToClone"),
                        static_cast<long long>(totalApproxDocumentsToClone / donorShards.size()));
        }
        updateCoordinatorDoc(opCtx, coordDoc.getReshardingUUID(), updates.obj());
    }

    void makeRecipientsFinishedCloningWithAssert(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);
        ASSERT_NE(coordDoc.getMetrics()->getDocumentCopy()->getStart(), Date_t::min());

        BSONObj updates = BSON(
            "$set" << BSON(std::string(ReshardingCoordinatorDocument::kRecipientShardsFieldName) +
                               ".$[].mutableState.state"
                           << idl::serialize(RecipientStateEnum::kApplying)));
        updateCoordinatorDoc(opCtx, coordDoc.getReshardingUUID(), updates);
    }

    void makeRecipientsBeInStrictConsistencyWithAssert(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);
        ASSERT_LTE(coordDoc.getMetrics()->getOplogApplication()->getStart(),
                   coordDoc.getMetrics()->getOplogApplication()->getStop());

        BSONObj updates = BSON(
            "$set" << BSON(std::string(ReshardingCoordinatorDocument::kRecipientShardsFieldName) +
                               ".$[].mutableState.state"
                           << idl::serialize(RecipientStateEnum::kStrictConsistency)));
        updateCoordinatorDoc(opCtx, coordDoc.getReshardingUUID(), updates);
    }

    void makeDonorsProceedToDone(OperationContext* opCtx, UUID reshardingUUID) {
        BSONObj updates =
            BSON("$set" << BSON(std::string(ReshardingCoordinatorDocument::kDonorShardsFieldName) +
                                    ".$[].mutableState.state"
                                << idl::serialize(DonorStateEnum::kDone)));
        updateCoordinatorDoc(opCtx, reshardingUUID, updates);
    }

    void makeDonorsProceedToDone(OperationContext* opCtx) {
        makeDonorsProceedToDone(opCtx, getCoordinatorDoc(opCtx).getReshardingUUID());
    }

    void makeDonorsProceedToDoneWithAssert(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);
        ASSERT_LTE(coordDoc.getMetrics()->getDocumentCopy()->getStart(),
                   coordDoc.getMetrics()->getDocumentCopy()->getStop());
        makeDonorsProceedToDone(opCtx, coordDoc.getReshardingUUID());
    }

    void makeRecipientsProceedToDone(OperationContext* opCtx, UUID reshardingUUID) {
        BSONObj updates = BSON(
            "$set" << BSON(std::string(ReshardingCoordinatorDocument::kRecipientShardsFieldName) +
                               ".$[].mutableState.state"
                           << idl::serialize(RecipientStateEnum::kDone)));
        updateCoordinatorDoc(opCtx, reshardingUUID, updates);
    }

    void makeRecipientsProceedToDone(OperationContext* opCtx) {
        makeRecipientsProceedToDone(opCtx, getCoordinatorDoc(opCtx).getReshardingUUID());
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

        BSONObj updates =
            BSON("$set" << BSON(
                     std::string(ReshardingCoordinatorDocument::kRecipientShardsFieldName) +
                         ".$[].mutableState.state"
                     << idl::serialize(RecipientStateEnum::kError)
                     << std::string(ReshardingCoordinatorDocument::kRecipientShardsFieldName) +
                         ".$[].mutableState.abortReason"
                     << tmpBuilder.obj()));
        updateCoordinatorDoc(opCtx, coordDoc.getReshardingUUID(), updates);
    }

    void stepUp(OperationContext* opCtx) {
        auto replCoord = repl::ReplicationCoordinator::get(getServiceContext());
        auto currOpTime = replCoord->getMyLastAppliedOpTime();
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

protected:
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

    Timestamp _cloneTimestamp = Timestamp(Date_t::now());

    RAIIServerParameterControllerForTest _serverParamController{
        "reshardingMinimumOperationDurationMillis", 0};

    long long _term = 0;

    static constexpr long totalApproxBytesToClone = 10000;
    static constexpr long totalApproxDocumentsToClone = 100;
};

}  // namespace resharding_coordinator_test
}  // namespace mongo
