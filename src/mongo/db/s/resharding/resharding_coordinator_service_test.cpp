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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class OpObserverForTest;
class PauseDuringStateTransitions;

class CoordinatorStateTransitionController {
public:
    CoordinatorStateTransitionController() = default;

    void waitUntilStateIsReached(CoordinatorStateEnum state) {
        stdx::unique_lock lk(_mutex);
        _waitUntilUnpausedCond.wait(lk, [this, state] { return _state == state; });
    }

private:
    friend OpObserverForTest;
    friend PauseDuringStateTransitions;

    void setPauseDuringTransition(CoordinatorStateEnum state) {
        stdx::lock_guard lk(_mutex);
        _pauseDuringTransition.insert(state);
    }

    void unsetPauseDuringTransition(CoordinatorStateEnum state) {
        stdx::lock_guard lk(_mutex);
        _pauseDuringTransition.erase(state);
        _pauseDuringTransitionCond.notify_all();
    }

    void notifyNewStateAndWaitUntilUnpaused(OperationContext* opCtx,
                                            CoordinatorStateEnum newState) {
        stdx::unique_lock lk(_mutex);
        _state = newState;
        _waitUntilUnpausedCond.notify_all();
        opCtx->waitForConditionOrInterrupt(_pauseDuringTransitionCond, lk, [this, newState] {
            return _pauseDuringTransition.count(newState) == 0;
        });
    }

    Mutex _mutex = MONGO_MAKE_LATCH("CoordinatorStateTransitionController::_mutex");
    stdx::condition_variable _pauseDuringTransitionCond;
    stdx::condition_variable _waitUntilUnpausedCond;

    std::set<CoordinatorStateEnum> _pauseDuringTransition;
    CoordinatorStateEnum _state = CoordinatorStateEnum::kUnused;
};

class PauseDuringStateTransitions {
public:
    PauseDuringStateTransitions(CoordinatorStateTransitionController* controller,
                                CoordinatorStateEnum state)
        : PauseDuringStateTransitions(controller, std::vector<CoordinatorStateEnum>{state}) {}

    PauseDuringStateTransitions(CoordinatorStateTransitionController* controller,
                                std::vector<CoordinatorStateEnum> states)
        : _controller{controller}, _states{std::move(states)} {
        for (auto state : _states) {
            _controller->setPauseDuringTransition(state);
        }
    }

    ~PauseDuringStateTransitions() {
        for (auto state : _states) {
            _controller->unsetPauseDuringTransition(state);
        }
    }

    PauseDuringStateTransitions(const PauseDuringStateTransitions&) = delete;
    PauseDuringStateTransitions& operator=(const PauseDuringStateTransitions&) = delete;

    PauseDuringStateTransitions(PauseDuringStateTransitions&&) = delete;
    PauseDuringStateTransitions& operator=(PauseDuringStateTransitions&&) = delete;

    void wait(CoordinatorStateEnum state) {
        _controller->waitUntilStateIsReached(state);
    }

    void unset(CoordinatorStateEnum state) {
        _controller->unsetPauseDuringTransition(state);
    }

private:
    CoordinatorStateTransitionController* const _controller;
    const std::vector<CoordinatorStateEnum> _states;
};

class OpObserverForTest : public OpObserverNoop {
public:
    OpObserverForTest(std::shared_ptr<CoordinatorStateTransitionController> controller)
        : _controller{std::move(controller)} {}

    void onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) override {
        if (args.nss != NamespaceString::kConfigReshardingOperationsNamespace) {
            return;
        }
        auto doc =
            ReshardingCoordinatorDocument::parse({"OpObserverForTest"}, args.updateArgs.updatedDoc);

        _controller->notifyNewStateAndWaitUntilUnpaused(opCtx, doc.getState());
    }

private:
    std::shared_ptr<CoordinatorStateTransitionController> _controller;
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
                if (version.getTimestamp()) {
                    initialChunks.emplace_back(
                        coordinatorDoc.getReshardingUUID(),
                        ChunkRange{reshardedChunk.getMin(), reshardedChunk.getMax()},
                        version,
                        reshardedChunk.getRecipientShardId());
                } else {
                    initialChunks.emplace_back(
                        coordinatorDoc.getTempReshardingNss(),
                        ChunkRange{reshardedChunk.getMin(), reshardedChunk.getMax()},
                        version,
                        reshardedChunk.getRecipientShardId());
                }
                version.incMinor();
            }
        }
        return ParticipantShardsAndChunks({{}, {}, initialChunks});
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
        : ReshardingCoordinatorService(serviceContext) {}

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override {
        return std::make_shared<ReshardingCoordinator>(
            this,
            ReshardingCoordinatorDocument::parse(
                IDLParserErrorContext("ReshardingCoordinatorStateDoc"), std::move(initialState)),
            std::make_shared<ExternalStateForTest>());
    }
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

        _opObserverRegistry->addObserver(std::make_unique<OpObserverImpl>());
        _opObserverRegistry->addObserver(std::make_unique<OpObserverForTest>(_controller));

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
        TransactionCoordinatorService::get(operationContext())->onStepDown();
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ConfigServerTestFixture::tearDown();
        _registry->onShutdown();
    }

    CoordinatorStateTransitionController* controller() {
        return _controller.get();
    }

    ReshardingCoordinatorDocument makeCoordinatorDoc(
        CoordinatorStateEnum state, boost::optional<Timestamp> fetchTimestamp = boost::none) {
        CommonReshardingMetadata meta(
            _reshardingUUID, _originalNss, UUID::gen(), _tempNss, _newShardKey.toBSON());
        ReshardingCoordinatorDocument doc(state,
                                          {DonorShardEntry(ShardId("shard0000"), {})},
                                          {RecipientShardEntry(ShardId("shard0001"), {})},
                                          ReshardingCoordinatorMetrics());
        doc.setCommonReshardingMetadata(meta);
        emplaceCloneTimestampIfExists(doc, cloneTimestamp);
        return doc;
    }

    void notifyDonorsReadyToDonate(ReshardingCoordinator& coordinator,
                                   ReshardingCoordinatorDocument& coordDoc) {
        auto donorShards = coordDoc.getDonorShards();
        DonorShardContext donorCtx;
        donorCtx.setState(DonorStateEnum::kDonatingInitialData);
        donorCtx.setMinFetchTimestamp(cloneTimestamp);
        for (auto& shard : donorShards) {
            shard.setMutableState(donorCtx);
        }
        coordDoc.setDonorShards(donorShards);

        coordinator.getObserver()->onReshardingParticipantTransition(coordDoc);
    }

    void notifyRecipientsFinishedCloning(ReshardingCoordinator& coordinator,
                                         ReshardingCoordinatorDocument& coordDoc) {
        auto shards = coordDoc.getRecipientShards();
        RecipientShardContext ctx;
        ctx.setState(RecipientStateEnum::kApplying);
        for (auto& shard : shards) {
            shard.setMutableState(ctx);
        }
        coordDoc.setRecipientShards(shards);

        coordinator.getObserver()->onReshardingParticipantTransition(coordDoc);
    }

    void notifyRecipientsInStrictConsistency(ReshardingCoordinator& coordinator,
                                             ReshardingCoordinatorDocument& coordDoc) {
        auto shards = coordDoc.getRecipientShards();
        RecipientShardContext ctx;
        ctx.setState(RecipientStateEnum::kStrictConsistency);
        for (auto& shard : shards) {
            shard.setMutableState(ctx);
        }
        coordDoc.setRecipientShards(shards);

        coordinator.getObserver()->onReshardingParticipantTransition(coordDoc);
    }

    void notifyDonorsDone(ReshardingCoordinator& coordinator,
                          ReshardingCoordinatorDocument& coordDoc) {
        auto donorShards = coordDoc.getDonorShards();
        DonorShardContext donorCtx;
        donorCtx.setState(DonorStateEnum::kDone);
        for (auto& shard : donorShards) {
            shard.setMutableState(donorCtx);
        }
        coordDoc.setDonorShards(donorShards);
        coordDoc.setState(CoordinatorStateEnum::kCommitting);

        coordinator.getObserver()->onReshardingParticipantTransition(coordDoc);
    }

    void notifyRecipientsDone(ReshardingCoordinator& coordinator,
                              ReshardingCoordinatorDocument& coordDoc) {
        auto shards = coordDoc.getRecipientShards();
        RecipientShardContext ctx;
        ctx.setState(RecipientStateEnum::kDone);
        for (auto& shard : shards) {
            shard.setMutableState(ctx);
        }
        coordDoc.setRecipientShards(shards);
        coordDoc.setState(CoordinatorStateEnum::kCommitting);

        coordinator.getObserver()->onReshardingParticipantTransition(coordDoc);
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
                                std::move(uuid));
        collType.setKeyPattern(shardKey);
        collType.setUnique(false);
        if (reshardingFields)
            collType.setReshardingFields(std::move(reshardingFields.get()));

        // TODO SERVER-53330: Evaluate whether or not we can include
        // CoordinatorStateEnum::kInitializing in this if statement.
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
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);

        auto coordinatorDoc = makeCoordinatorDoc(state, fetchTimestamp);

        TypeCollectionReshardingFields reshardingFields(coordinatorDoc.getReshardingUUID());
        reshardingFields.setState(coordinatorDoc.getState());
        reshardingFields.setDonorFields(TypeCollectionDonorFields(
            coordinatorDoc.getTempReshardingNss(),
            coordinatorDoc.getReshardingKey(),
            extractShardIdsFromParticipantEntries(coordinatorDoc.getRecipientShards())));

        auto originalNssCatalogEntry = makeOriginalCollectionCatalogEntry(
            coordinatorDoc,
            reshardingFields,
            std::move(epoch),
            opCtx->getServiceContext()->getPreciseClockSource()->now());
        client.insert(CollectionType::ConfigNS.ns(), originalNssCatalogEntry.toBSON());

        DatabaseType dbDoc(coordinatorDoc.getSourceNss().db().toString(),
                           coordinatorDoc.getDonorShards().front().getId(),
                           true,
                           DatabaseVersion{UUID::gen()});
        client.insert(DatabaseType::ConfigNS.ns(), dbDoc.toBSON());

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
                                      const ShardKeyPattern& shardKey,
                                      std::vector<OID> ids) {
        auto chunkRanges =
            _newShardKey.isShardKey(shardKey.toBSON()) ? _newChunkRanges : _oldChunkRanges;

        // Create two chunks, one on each shard with the given namespace and epoch
        ChunkVersion version(1, 0, epoch, boost::none /* timestamp */);
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
                                               const ShardKeyPattern& shardKey,
                                               std::vector<OID> ids) {
        auto chunks = makeChunks(uuid, epoch, shardKey, ids);

        // Only the chunk corresponding to shard0000 is stored as a donor in the coordinator state
        // document constructed.
        auto donorChunk = chunks[0];
        insertChunkAndZoneEntries(chunks, {});
        return donorChunk;
    }

    repl::PrimaryOnlyService* _service = nullptr;

    std::shared_ptr<CoordinatorStateTransitionController> _controller;

    OpObserverRegistry* _opObserverRegistry = nullptr;

    repl::PrimaryOnlyServiceRegistry* _registry = nullptr;

    NamespaceString _originalNss = NamespaceString("db.foo");
    UUID _originalUUID = UUID::gen();
    OID _originalEpoch = OID::gen();

    NamespaceString _tempNss = NamespaceString("db.system.resharding." + _originalUUID.toString());
    UUID _reshardingUUID = UUID::gen();
    OID _tempEpoch = OID::gen();
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
};

TEST_F(ReshardingCoordinatorServiceTest, ReshardingCoordinatorSuccessfullyTransitionsTokDone) {
    const std::vector<CoordinatorStateEnum> coordinatorStates{
        CoordinatorStateEnum::kPreparingToDonate,
        CoordinatorStateEnum::kCloning,
        CoordinatorStateEnum::kApplying,
        CoordinatorStateEnum::kBlockingWrites,
        CoordinatorStateEnum::kCommitting};
    PauseDuringStateTransitions stateTransitionsGuard{controller(), coordinatorStates};

    auto doc = insertStateAndCatalogEntries(CoordinatorStateEnum::kUnused, _originalEpoch);
    auto opCtx = operationContext();
    auto donorChunk = makeAndInsertChunksForDonorShard(
        _originalUUID, _originalEpoch, _oldShardKey, std::vector{OID::gen(), OID::gen()});

    auto initialChunks =
        makeChunks(_reshardingUUID, _tempEpoch, _newShardKey, std::vector{OID::gen(), OID::gen()});

    std::vector<ReshardedChunk> presetReshardedChunks;
    for (const auto& chunk : initialChunks) {
        presetReshardedChunks.emplace_back(chunk.getShard(), chunk.getMin(), chunk.getMax());
    }

    doc.setPresetReshardedChunks(presetReshardedChunks);

    auto coordinator = ReshardingCoordinator::getOrCreate(opCtx, _service, doc.toBSON());
    stateTransitionsGuard.wait(CoordinatorStateEnum::kPreparingToDonate);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kPreparingToDonate);
    notifyDonorsReadyToDonate(*coordinator, doc);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kCloning);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kCloning);

    notifyRecipientsFinishedCloning(*coordinator, doc);
    stateTransitionsGuard.wait(CoordinatorStateEnum::kApplying);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kApplying);

    coordinator->onOkayToEnterCritical();
    stateTransitionsGuard.wait(CoordinatorStateEnum::kBlockingWrites);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kBlockingWrites);

    notifyRecipientsInStrictConsistency(*coordinator, doc);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kCommitting);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kCommitting);

    notifyDonorsDone(*coordinator, doc);
    notifyRecipientsDone(*coordinator, doc);

    coordinator->getCompletionFuture().get(opCtx);
}

}  // namespace
}  // namespace mongo
