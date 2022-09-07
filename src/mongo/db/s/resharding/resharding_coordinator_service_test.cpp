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

#include <boost/optional.hpp>
#include <functional>

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_op_observer.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_service_test_helpers.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/resharding/resharding_coordinator_service_conflicting_op_in_progress_info.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using CoordinatorStateTransitionController =
    resharding_service_test_helpers::StateTransitionController<CoordinatorStateEnum>;
using OpObserverForTest =
    resharding_service_test_helpers::OpObserverForTest<CoordinatorStateEnum,
                                                       ReshardingCoordinatorDocument>;
using PauseDuringStateTransitions =
    resharding_service_test_helpers::PauseDuringStateTransitions<CoordinatorStateEnum>;

class CoordinatorOpObserverForTest : public OpObserverForTest {
public:
    CoordinatorOpObserverForTest(std::shared_ptr<CoordinatorStateTransitionController> controller)
        : OpObserverForTest(std::move(controller),
                            NamespaceString::kConfigReshardingOperationsNamespace) {}

    CoordinatorStateEnum getState(const ReshardingCoordinatorDocument& coordinatorDoc) override {
        return coordinatorDoc.getState();
    }
};

class ExternalStateForTest : public ReshardingCoordinatorExternalState {
    ParticipantShardsAndChunks calculateParticipantShardsAndChunks(
        OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) override {
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

    void sendCommandToShards(OperationContext* opCtx,
                             StringData dbName,
                             const BSONObj& command,
                             const std::vector<ShardId>& shardIds,
                             const std::shared_ptr<executor::TaskExecutor>& executor) override {}
};

class ReshardingCoordinatorServiceForTest : public ReshardingCoordinatorService {
public:
    explicit ReshardingCoordinatorServiceForTest(ServiceContext* serviceContext)
        : ReshardingCoordinatorService(serviceContext), _serviceContext(serviceContext) {}

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override {
        return std::make_shared<ReshardingCoordinator>(
            this,
            ReshardingCoordinatorDocument::parse(IDLParserContext("ReshardingCoordinatorStateDoc"),
                                                 std::move(initialState)),
            std::make_shared<ExternalStateForTest>(),
            _serviceContext);
    }

private:
    ServiceContext* _serviceContext;
};

class ReshardingCoordinatorServiceTest : public ConfigServerTestFixture {
public:
    using ReshardingCoordinator = ReshardingCoordinatorService::ReshardingCoordinator;

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) {
        return std::make_unique<ReshardingCoordinatorServiceForTest>(serviceContext);
    }

    void setUp() override {
        ConfigServerTestFixture::setUp();

        ShardType shard0;
        shard0.setName("shard0000");
        shard0.setHost("shard0000:1234");
        ShardType shard1;
        shard1.setName("shard0001");
        shard1.setHost("shard0001:1234");
        setupShards({shard0, shard1});

        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns());
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});
        client.createCollection(NamespaceString::kConfigReshardingOperationsNamespace.ns());
        client.createCollection(CollectionType::ConfigNS.ns());

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->onShardingInitialization(operationContext(), true);

        _controller = std::make_shared<CoordinatorStateTransitionController>();
        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());

        repl::createOplog(opCtx);

        _opObserverRegistry =
            dynamic_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        invariant(_opObserverRegistry);

        _opObserverRegistry->addObserver(std::make_unique<ReshardingOpObserver>());
        _opObserverRegistry->addObserver(
            std::make_unique<CoordinatorOpObserverForTest>(_controller));
        _opObserverRegistry->addObserver(
            std::make_unique<repl::PrimaryOnlyServiceOpObserver>(getServiceContext()));

        _registry = repl::PrimaryOnlyServiceRegistry::get(getServiceContext());
        auto service = makeService(getServiceContext());
        auto serviceName = service->getServiceName();
        _registry->registerService(std::move(service));
        _service = _registry->lookupServiceByName(serviceName);

        _registry->onStartup(opCtx);
        auto term = replicationCoordinator()->getMyLastAppliedOpTime().getTerm();
        _registry->onStepUpComplete(opCtx, term);
    }

    void tearDown() override {
        globalFailPointRegistry().disableAllFailpoints();

        TransactionCoordinatorService::get(operationContext())->onStepDown();
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ConfigServerTestFixture::tearDown();
        _registry->onShutdown();
    }

    CoordinatorStateTransitionController* controller() {
        return _controller.get();
    }

    ReshardingCoordinatorDocument makeCoordinatorDoc(
        CoordinatorStateEnum state,
        UUID reshardingUUID,
        NamespaceString originalNss,
        NamespaceString tempNss,
        const ShardKeyPattern& newShardKey,
        boost::optional<Timestamp> fetchTimestamp = boost::none) {
        CommonReshardingMetadata meta(
            reshardingUUID, originalNss, UUID::gen(), tempNss, newShardKey.toBSON());

        meta.setStartTime(getServiceContext()->getFastClockSource()->now());

        ReshardingCoordinatorDocument doc(state,
                                          {DonorShardEntry(ShardId("shard0000"), {})},
                                          {RecipientShardEntry(ShardId("shard0001"), {})});

        doc.setCommonReshardingMetadata(meta);
        resharding::emplaceCloneTimestampIfExists(doc, cloneTimestamp);
        return doc;
    }

    std::shared_ptr<ReshardingCoordinatorService::ReshardingCoordinator> getCoordinator(
        OperationContext* opCtx, repl::PrimaryOnlyService::InstanceID instanceId) {
        auto coordinator = getCoordinatorIfExists(opCtx, instanceId);
        ASSERT_TRUE(bool(coordinator));
        return coordinator;
    }

    std::shared_ptr<ReshardingCoordinatorService::ReshardingCoordinator> getCoordinatorIfExists(
        OperationContext* opCtx, repl::PrimaryOnlyService::InstanceID instanceId) {
        auto coordinatorOpt = ReshardingCoordinatorService::ReshardingCoordinator::lookup(
            opCtx, _service, instanceId);
        if (!coordinatorOpt) {
            return nullptr;
        }

        auto coordinator = *coordinatorOpt;
        return coordinator ? coordinator : nullptr;
    }

    ReshardingCoordinatorDocument getCoordinatorDoc(OperationContext* opCtx) {
        DBDirectClient client(opCtx);

        auto doc = client.findOne(NamespaceString::kConfigReshardingOperationsNamespace, BSONObj{});
        IDLParserContext errCtx("reshardingCoordFromTest");
        return ReshardingCoordinatorDocument::parse(errCtx, doc);
    }

    void replaceCoordinatorDoc(OperationContext* opCtx,
                               const ReshardingCoordinatorDocument& newDoc) {
        DBDirectClient client(opCtx);

        const BSONObj query(BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName
                                 << newDoc.getReshardingUUID()));
        client.update(
            NamespaceString::kConfigReshardingOperationsNamespace.ns(), {}, newDoc.toBSON());
    }

    void waitUntilCommittedCoordinatorDocReach(OperationContext* opCtx,
                                               CoordinatorStateEnum state) {
        DBDirectClient client(opCtx);

        while (true) {
            auto coordinatorDoc = getCoordinatorDoc(opCtx);

            if (coordinatorDoc.getState() == state) {
                break;
            }

            sleepmillis(50);
        }
    }

    void makeDonorsReadyToDonate(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);

        auto donorShards = coordDoc.getDonorShards();
        DonorShardContext donorCtx;
        donorCtx.setState(DonorStateEnum::kDonatingInitialData);
        donorCtx.setMinFetchTimestamp(cloneTimestamp);
        for (auto& shard : donorShards) {
            shard.setMutableState(donorCtx);
        }
        coordDoc.setDonorShards(donorShards);

        replaceCoordinatorDoc(opCtx, coordDoc);
    }

    void makeRecipientsFinishedCloning(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);

        auto shards = coordDoc.getRecipientShards();
        RecipientShardContext ctx;
        ctx.setState(RecipientStateEnum::kApplying);
        for (auto& shard : shards) {
            shard.setMutableState(ctx);
        }
        coordDoc.setRecipientShards(shards);

        replaceCoordinatorDoc(opCtx, coordDoc);
    }

    void makeRecipientsBeInStrictConsistency(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);

        auto shards = coordDoc.getRecipientShards();
        RecipientShardContext ctx;
        ctx.setState(RecipientStateEnum::kStrictConsistency);
        for (auto& shard : shards) {
            shard.setMutableState(ctx);
        }
        coordDoc.setRecipientShards(shards);

        replaceCoordinatorDoc(opCtx, coordDoc);
    }

    void makeDonorsProceedToDone(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);
        auto donorShards = coordDoc.getDonorShards();
        DonorShardContext donorCtx;
        donorCtx.setState(DonorStateEnum::kDone);
        for (auto& shard : donorShards) {
            shard.setMutableState(donorCtx);
        }
        coordDoc.setDonorShards(donorShards);

        replaceCoordinatorDoc(opCtx, coordDoc);
    }

    void makeRecipientsProceedToDone(OperationContext* opCtx) {
        auto coordDoc = getCoordinatorDoc(opCtx);
        auto shards = coordDoc.getRecipientShards();
        RecipientShardContext ctx;
        ctx.setState(RecipientStateEnum::kDone);
        for (auto& shard : shards) {
            shard.setMutableState(ctx);
        }
        coordDoc.setRecipientShards(shards);

        replaceCoordinatorDoc(opCtx, coordDoc);
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
        CoordinatorStateEnum state,
        OID epoch,
        boost::optional<Timestamp> fetchTimestamp = boost::none) {
        return insertStateAndCatalogEntries(
            state, epoch, _originalUUID, _originalNss, _tempNss, _newShardKey, fetchTimestamp);
    }

    ReshardingCoordinatorDocument insertStateAndCatalogEntries(
        CoordinatorStateEnum state,
        OID epoch,
        UUID reshardingUUID,
        NamespaceString originalNss,
        NamespaceString tempNss,
        const ShardKeyPattern& newShardKey,
        boost::optional<Timestamp> fetchTimestamp = boost::none) {
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);

        auto coordinatorDoc = makeCoordinatorDoc(
            state, reshardingUUID, originalNss, tempNss, newShardKey, fetchTimestamp);

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
        client.insert(CollectionType::ConfigNS.ns(), originalNssCatalogEntry.toBSON());

        DatabaseType dbDoc(coordinatorDoc.getSourceNss().db().toString(),
                           coordinatorDoc.getDonorShards().front().getId(),
                           DatabaseVersion{UUID::gen(), Timestamp(1, 1)});
        client.insert(NamespaceString::kConfigDatabasesNamespace.ns(), dbDoc.toBSON());

        return coordinatorDoc;
    }

    void insertChunkAndZoneEntries(std::vector<ChunkType> chunks, std::vector<TagsType> zones) {
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);

        for (const auto& chunk : chunks) {
            client.insert(ChunkType::ConfigNS.ns(), chunk.toConfigBSON());
        }

        for (const auto& zone : zones) {
            client.insert(TagsType::ConfigNS.ns(), zone.toBSON());
        }
    }

    std::vector<ChunkType> makeChunks(const UUID& uuid,
                                      OID epoch,
                                      const Timestamp& timestamp,
                                      const ShardKeyPattern& shardKey,
                                      std::vector<OID> ids) {
        auto chunkRanges =
            _newShardKey.isShardKey(shardKey.toBSON()) ? _newChunkRanges : _oldChunkRanges;

        // Create two chunks, one on each shard with the given namespace and epoch
        ChunkVersion version({epoch, timestamp}, {1, 0});
        ChunkType chunk1(uuid, chunkRanges[0], version, ShardId("shard0000"));
        chunk1.setName(ids[0]);
        version.incMinor();
        ChunkType chunk2(uuid, chunkRanges[1], version, ShardId("shard0001"));
        chunk2.setName(ids[1]);

        return std::vector<ChunkType>{chunk1, chunk2};
    }

    // Returns the chunk for the donor shard.
    ChunkType makeAndInsertChunksForDonorShard(const UUID& uuid,
                                               OID epoch,
                                               const Timestamp& timestamp,
                                               const ShardKeyPattern& shardKey,
                                               std::vector<OID> ids) {
        auto chunks = makeChunks(uuid, epoch, timestamp, shardKey, ids);

        // Only the chunk corresponding to shard0000 is stored as a donor in the coordinator state
        // document constructed.
        auto donorChunk = chunks[0];
        insertChunkAndZoneEntries(chunks, {});
        return donorChunk;
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
        replCoord->setMyLastAppliedOpTimeAndWallTime({newOpTime, {}});

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
            stdx::lock_guard<Client> lk(*client);
            if (client->isFromSystemConnection() && !client->canKillSystemOperationInStepdown(lk)) {
                continue;
            }

            OperationContext* toKill = client->getOperationContext();

            if (toKill && !toKill->isKillPending() && toKill->getOpID() != opCtx->getOpID()) {
                auto locker = toKill->lockState();
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

            stdx::lock_guard<Client> lk(*client);
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
        boost::optional<Timestamp> fetchTimestamp = boost::none) {
        auto doc = insertStateAndCatalogEntries(CoordinatorStateEnum::kUnused,
                                                _originalEpoch,
                                                reshardingUUID,
                                                originalNss,
                                                tempNss,
                                                newShardKey,
                                                fetchTimestamp);
        auto opCtx = operationContext();
        auto donorChunk = makeAndInsertChunksForDonorShard(originalUUID,
                                                           _originalEpoch,
                                                           _originalTimestamp,
                                                           oldShardKey,
                                                           std::vector{OID::gen(), OID::gen()});
        auto initialChunks = makeChunks(reshardingUUID,
                                        _tempEpoch,
                                        _tempTimestamp,
                                        newShardKey,
                                        std::vector{OID::gen(), OID::gen()});

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

    void runReshardingToCompletion(const TransitionFunctionMap& transitionFunctions) {
        auto runFunctionForState = [&](CoordinatorStateEnum state) {
            auto it = transitionFunctions.find(state);
            if (it == transitionFunctions.end()) {
                return;
            }
            it->second();
        };

        const std::vector<CoordinatorStateEnum> states{CoordinatorStateEnum::kPreparingToDonate,
                                                       CoordinatorStateEnum::kCloning,
                                                       CoordinatorStateEnum::kApplying,
                                                       CoordinatorStateEnum::kBlockingWrites,
                                                       CoordinatorStateEnum::kCommitting};
        PauseDuringStateTransitions stateTransitionsGuard{controller(), states};

        auto opCtx = operationContext();
        auto coordinator = initializeAndGetCoordinator();

        stateTransitionsGuard.wait(CoordinatorStateEnum::kPreparingToDonate);
        runFunctionForState(CoordinatorStateEnum::kPreparingToDonate);
        stateTransitionsGuard.unset(CoordinatorStateEnum::kPreparingToDonate);
        waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);
        makeDonorsReadyToDonate(opCtx);

        stateTransitionsGuard.wait(CoordinatorStateEnum::kCloning);
        runFunctionForState(CoordinatorStateEnum::kCloning);
        stateTransitionsGuard.unset(CoordinatorStateEnum::kCloning);
        waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCloning);

        makeRecipientsFinishedCloning(opCtx);
        stateTransitionsGuard.wait(CoordinatorStateEnum::kApplying);
        runFunctionForState(CoordinatorStateEnum::kApplying);
        stateTransitionsGuard.unset(CoordinatorStateEnum::kApplying);
        waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kApplying);

        coordinator->onOkayToEnterCritical();
        stateTransitionsGuard.wait(CoordinatorStateEnum::kBlockingWrites);
        runFunctionForState(CoordinatorStateEnum::kBlockingWrites);
        stateTransitionsGuard.unset(CoordinatorStateEnum::kBlockingWrites);
        waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kBlockingWrites);

        makeRecipientsBeInStrictConsistency(opCtx);

        stateTransitionsGuard.wait(CoordinatorStateEnum::kCommitting);
        runFunctionForState(CoordinatorStateEnum::kCommitting);
        stateTransitionsGuard.unset(CoordinatorStateEnum::kCommitting);

        waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCommitting);

        makeDonorsProceedToDone(opCtx);
        makeRecipientsProceedToDone(opCtx);

        coordinator->getCompletionFuture().get(opCtx);
    }

    repl::PrimaryOnlyService* _service = nullptr;

    std::shared_ptr<CoordinatorStateTransitionController> _controller;

    OpObserverRegistry* _opObserverRegistry = nullptr;

    repl::PrimaryOnlyServiceRegistry* _registry = nullptr;

    NamespaceString _originalNss = NamespaceString("db.foo");
    UUID _originalUUID = UUID::gen();
    OID _originalEpoch = OID::gen();
    Timestamp _originalTimestamp = Timestamp(1);

    NamespaceString _tempNss = NamespaceString("db.system.resharding." + _originalUUID.toString());
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

    Timestamp cloneTimestamp = Timestamp(Date_t::now());

    RAIIServerParameterControllerForTest serverParamController{
        "reshardingMinimumOperationDurationMillis", 0};

    const std::vector<ShardId> _recipientShards = {{"shard0001"}};

    long long _term = 0;
};

TEST_F(ReshardingCoordinatorServiceTest, ReshardingCoordinatorSuccessfullyTransitionsTokDone) {
    runReshardingToCompletion();
}

TEST_F(ReshardingCoordinatorServiceTest, ReshardingCoordinatorTransitionsTokDoneWithInterrupt) {

    const auto interrupt = [this] { killAllReshardingCoordinatorOps(); };
    runReshardingToCompletion(
        TransitionFunctionMap{{CoordinatorStateEnum::kPreparingToDonate, interrupt},
                              {CoordinatorStateEnum::kCloning, interrupt},
                              {CoordinatorStateEnum::kApplying, interrupt},
                              {CoordinatorStateEnum::kBlockingWrites, interrupt}});
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
    auto pauseBeforeInsertCoordinatorDoc =
        globalFailPointRegistry().find("pauseBeforeInsertCoordinatorDoc");
    auto timesEnteredFailPoint = pauseBeforeInsertCoordinatorDoc->setMode(FailPoint::alwaysOn, 0);

    auto doc = insertStateAndCatalogEntries(CoordinatorStateEnum::kUnused, _originalEpoch);
    doc.setRecipientShards({});
    doc.setDonorShards({});

    auto donorChunk = makeAndInsertChunksForDonorShard(_originalUUID,
                                                       _originalEpoch,
                                                       _originalTimestamp,
                                                       _oldShardKey,
                                                       std::vector{OID::gen(), OID::gen()});

    auto initialChunks = makeChunks(_reshardingUUID,
                                    _tempEpoch,
                                    _tempTimestamp,
                                    _newShardKey,
                                    std::vector{OID::gen(), OID::gen()});

    std::vector<ReshardedChunk> presetReshardedChunks;
    for (const auto& chunk : initialChunks) {
        presetReshardedChunks.emplace_back(chunk.getShard(), chunk.getMin(), chunk.getMax());
    }

    doc.setPresetReshardedChunks(presetReshardedChunks);

    (void)ReshardingCoordinator::getOrCreate(opCtx, _service, doc.toBSON());
    auto instanceId =
        BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

    pauseBeforeInsertCoordinatorDoc->waitForTimesEntered(timesEnteredFailPoint + 1);

    auto coordinator = getCoordinator(opCtx, instanceId);
    stepDown(opCtx);
    pauseBeforeInsertCoordinatorDoc->setMode(FailPoint::off, 0);
    ASSERT_EQ(coordinator->getCompletionFuture().getNoThrow(),
              ErrorCodes::InterruptedDueToReplStateChange);

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
    ASSERT_EQ(newCoordinator->getCompletionFuture().getNoThrow(),
              ErrorCodes::InterruptedDueToReplStateChange);
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

    auto doc = insertStateAndCatalogEntries(CoordinatorStateEnum::kUnused, _originalEpoch);
    auto opCtx = operationContext();
    auto donorChunk = makeAndInsertChunksForDonorShard(_originalUUID,
                                                       _originalEpoch,
                                                       _originalTimestamp,
                                                       _oldShardKey,
                                                       std::vector{OID::gen(), OID::gen()});

    auto initialChunks = makeChunks(_reshardingUUID,
                                    _tempEpoch,
                                    _tempTimestamp,
                                    _newShardKey,
                                    std::vector{OID::gen(), OID::gen()});

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
              "stepDownAfter"_attr = (CoordinatorState_serializer(state).toString()));

        switch (state) {
            case CoordinatorStateEnum::kCloning: {
                makeDonorsReadyToDonate(opCtx);
                break;
            }

            case CoordinatorStateEnum::kApplying: {
                makeRecipientsFinishedCloning(opCtx);
                break;
            }

            case CoordinatorStateEnum::kBlockingWrites: {
                // Pretend that the recipients are already ready to commit.
                coordinator->onOkayToEnterCritical();
                break;
            }

            case CoordinatorStateEnum::kCommitting: {
                makeRecipientsBeInStrictConsistency(opCtx);
                break;
            }

            default:
                break;
        }

        stateTransitionsGuard.wait(state);

        stepDown(opCtx);

        ASSERT_EQ(coordinator->getCompletionFuture().getNoThrow(),
                  ErrorCodes::InterruptedDueToReplStateChange);

        coordinator.reset();

        stepUp(opCtx);

        stateTransitionsGuard.unset(state);

        if (state == CoordinatorStateEnum::kBlockingWrites) {
            // We have to fake this again as the effects are not persistent.
            coordinator = getCoordinator(opCtx, instanceId);
            coordinator->onOkayToEnterCritical();
        }

        // 'done' state is never written to storage so don't wait for it.
        waitUntilCommittedCoordinatorDocReach(opCtx, state);
    }

    makeDonorsProceedToDone(opCtx);
    makeRecipientsProceedToDone(opCtx);

    // Join the coordinator if it has not yet been cleaned up.
    if (auto coordinator = getCoordinatorIfExists(opCtx, instanceId)) {
        coordinator->getCompletionFuture().get(opCtx);
    }

    {
        DBDirectClient client(opCtx);

        // config.chunks should have been moved to the new UUID
        FindCommandRequest findRequest{ChunkType::ConfigNS};
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
                           BSON(CollectionType::kNssFieldName << doc.getSourceNss().ns()));

        ASSERT_EQUALS(collection.isEmpty(), false);
        ASSERT_EQUALS(
            UUID::parse(collection.getField(CollectionType::kUuidFieldName)).getValue().toString(),
            doc.getReshardingUUID().toString());
    }
}

TEST_F(ReshardingCoordinatorServiceTest, ReshardingCoordinatorFailsIfMigrationNotAllowed) {
    auto doc = insertStateAndCatalogEntries(CoordinatorStateEnum::kUnused, _originalEpoch);
    auto opCtx = operationContext();
    auto donorChunk = makeAndInsertChunksForDonorShard(_originalUUID,
                                                       _originalEpoch,
                                                       _originalTimestamp,
                                                       _oldShardKey,
                                                       std::vector{OID::gen(), OID::gen()});

    auto initialChunks = makeChunks(_reshardingUUID,
                                    _tempEpoch,
                                    _tempTimestamp,
                                    _newShardKey,
                                    std::vector{OID::gen(), OID::gen()});

    std::vector<ReshardedChunk> presetReshardedChunks;
    for (const auto& chunk : initialChunks) {
        presetReshardedChunks.emplace_back(chunk.getShard(), chunk.getMin(), chunk.getMax());
    }

    doc.setPresetReshardedChunks(presetReshardedChunks);

    {
        DBDirectClient client(opCtx);
        client.update(CollectionType::ConfigNS.ns(),
                      BSON(CollectionType::kNssFieldName << _originalNss.ns()),
                      BSON("$set" << BSON(CollectionType::kAllowMigrationsFieldName << false)));
    }

    auto coordinator = ReshardingCoordinator::getOrCreate(opCtx, _service, doc.toBSON());
    ASSERT_THROWS_CODE(coordinator->getCompletionFuture().get(opCtx), DBException, 5808201);

    // Check that reshardCollection keeps allowMigrations setting intact.
    {
        DBDirectClient client(opCtx);
        CollectionType collDoc(client.findOne(
            CollectionType::ConfigNS, BSON(CollectionType::kNssFieldName << _originalNss.ns())));
        ASSERT_FALSE(collDoc.getAllowMigrations());
    }
}

TEST_F(ReshardingCoordinatorServiceTest, MultipleReshardingOperationsFail) {
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
    ASSERT_THROWS_CODE(initializeAndGetCoordinator(
                           UUID::gen(),
                           NamespaceString("db.moo"),
                           NamespaceString("db.system.resharding." + UUID::gen().toString()),
                           ShardKeyPattern(BSON("shardKeyV1" << 1)),
                           UUID::gen(),
                           ShardKeyPattern(BSON("shardKeyV2" << 1))),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);

    // Asserts that a resharding op with same namespace and different shard key fails with
    // ConflictingOperationInProgress.
    ASSERT_THROWS_CODE(initializeAndGetCoordinator(
                           UUID::gen(),
                           _originalNss,
                           NamespaceString("db.system.resharding." + UUID::gen().toString()),
                           ShardKeyPattern(BSON("shardKeyV1" << 1)),
                           UUID::gen(),
                           _oldShardKey),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);

    coordinator->abort();
    coordinator->getCompletionFuture().wait();
}

}  // namespace
}  // namespace mongo
