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
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/s/resharding/resharding_coordinator.h"
#include "mongo/db/s/resharding/resharding_coordinator_observer.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_test_fixture.h"
#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"
#include "mongo/db/s/resharding/resharding_service_test_helpers.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resharding_coordinator_service_conflicting_op_in_progress_info.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"

#include <functional>
#include <string>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

constexpr Milliseconds kOneDayMillis{24 * 3600 * 1000};

using CoordinatorStateTransitionController =
    resharding_service_test_helpers::StateTransitionController<CoordinatorStateEnum>;
using OpObserverForTest = resharding_service_test_helpers::
    StateTransitionControllerOpObserver<CoordinatorStateEnum, ReshardingCoordinatorDocument>;
using PauseDuringStateTransitions =
    resharding_service_test_helpers::PauseDuringStateTransitions<CoordinatorStateEnum>;

using resharding_coordinator_test::ExternalStateForTest;
using resharding_coordinator_test::ReshardingCoordinatorServiceForTest;
using resharding_coordinator_test::ReshardingCoordinatorServiceTestCommon;

class ReshardingCoordinatorServiceTestBase : public ReshardingCoordinatorServiceTestCommon {
public:
    struct ReshardingOptions {
        const std::vector<ShardId> donorShardIds;
        const std::vector<ShardId> recipientShardIds;
        const std::set<ShardId> recipientShardIdsNoInitialChunks;
        bool performVerification;
        boost::optional<UUID> userReshardingUUID;

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

    ExternalStateForTest::Options getExternalStateOptions() const override = 0;

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
        doc.setUserReshardingUUID(reshardingOptions.userReshardingUUID);

        // Set demo mode to true for testing purposes to avoid the delay before commit monitor
        // queries recipient.
        doc.setDemoMode(true);

        return doc;
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
        client.insert(NamespaceString::kConfigsvrCollectionsNamespace,
                      originalNssCatalogEntry.toBSON());

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

    static std::vector<CoordinatorStateEnum> defaultReshardingCompletionStates() {
        return {CoordinatorStateEnum::kPreparingToDonate,
                CoordinatorStateEnum::kCloning,
                CoordinatorStateEnum::kApplying,
                CoordinatorStateEnum::kBlockingWrites,
                CoordinatorStateEnum::kCommitting};
    }

    void runReshardingToCompletion() {
        runReshardingToCompletion(TransitionFunctionMap{});
    }

    void runReshardingToCompletion(
        const TransitionFunctionMap& transitionFunctions,
        std::unique_ptr<PauseDuringStateTransitions> stateTransitionsGuard = nullptr,
        std::vector<CoordinatorStateEnum> states = defaultReshardingCompletionStates(),
        boost::optional<ReshardingOptions> reshardingOptions = boost::none,
        boost::optional<CoordinatorStateEnum> errorState = boost::none,
        boost::optional<CoordinatorStateEnum> failoverAtState = boost::none) {
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
            ASSERT_FALSE(getCollectionCatalogEntry(opCtx).getAllowMigrations());
            runFunctionForState(state);
            stateTransitionsGuard->unset(state);

            if (errorState && state == *errorState) {
                // With featureFlagReshardingInitNoRefresh enabled, the abort path skips the
                // observer wait and races kAborting -> kDone, so polling for kAborting and
                // advancing participants would either spin or fail.
                if (!resharding::gFeatureFlagReshardingInitNoRefresh
                         .isEnabledAndIgnoreFCVUnsafe()) {
                    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kAborting);
                    makeRecipientsProceedToDone(opCtx);
                    makeDonorsProceedToDone(opCtx);
                }

                ASSERT_EQ(coordinator->getCompletionFuture().getNoThrow(),
                          ErrorCodes::InternalError);
                checkCoordinatorDocumentRemoved(opCtx);
                return;
            } else {
                waitUntilCommittedCoordinatorDocReach(opCtx, state);
                if (failoverAtState && state == *failoverAtState) {
                    stepDown(opCtx);
                    ASSERT_EQ(coordinator->getCompletionFuture().getNoThrow(),
                              ErrorCodes::CallbackCanceled);
                    coordinator.reset();

                    stepUp(opCtx);
                    auto instanceId = BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName
                                           << _reshardingUUID);
                    coordinator = getCoordinator(opCtx, instanceId);
                }
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
        ASSERT_TRUE(getCollectionCatalogEntry(opCtx).getAllowMigrations());

        BSONObjBuilder bob;
        ReshardingCumulativeMetrics::getForResharding(operationContext()->getServiceContext())
            ->reportForServerStatus(&bob);
        auto cumulativeMetricsBSON = bob.obj();
        ASSERT_EQ(cumulativeMetricsBSON["resharding"]["countStarted"].numberInt(), 1);
        ASSERT_EQ(cumulativeMetricsBSON["resharding"]["countSucceeded"].numberInt(), 1);
    }

    /**
     * Same as runReshardingToCompletion(), but performs a `stepDown`/`stepUp`
     * immediately after `failoverAtState` is reached.
     */
    void runReshardingToCompletionWithFailoverAt(CoordinatorStateEnum failoverAtState) {
        runReshardingToCompletion(TransitionFunctionMap{},
                                  nullptr /* stateTransitionsGuard */,
                                  defaultReshardingCompletionStates(),
                                  boost::none /* reshardingOptions */,
                                  boost::none /* errorState */,
                                  failoverAtState);
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

    void runReshardingAbortWithoutQuiescingBeforeFailover(
        CoordinatorStateEnum state, resharding::AbortType abortTypeAfterFailover) {
        // Set a large quiesce period to test that there is no quiescing both before and after
        // failover, regardless of the abort type after failover.
        RAIIServerParameterControllerForTest quiescePeriodMillis{
            "reshardingCoordinatorQuiescePeriodMillis", kOneDayMillis.count()};

        // Set the failpoint to pause the coordinator before it removes the state doc. Otherwise,
        // the doc would get removed as soon as the resharding operation aborts since there is
        // no quiescing.
        auto pauseCoordinatorBeforeRemovingStateDoc =
            globalFailPointRegistry().find("reshardingPauseCoordinatorBeforeRemovingStateDoc");
        pauseCoordinatorBeforeRemovingStateDoc->setMode(FailPoint::alwaysOn);

        auto opCtx = operationContext();
        auto coordinator = initializeAndGetCoordinator();

        if (state >= CoordinatorStateEnum::kPreparingToDonate) {
            waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);
        }

        if (state >= CoordinatorStateEnum::kCloning) {
            makeDonorsReadyToDonateWithAssert(opCtx);
            waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCloning);
        }

        if (state >= CoordinatorStateEnum::kApplying) {
            makeRecipientsFinishedCloningWithAssert(opCtx);
            waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kApplying);
        }

        if (state >= CoordinatorStateEnum::kBlockingWrites) {
            coordinator->onOkayToEnterCritical();
            waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kBlockingWrites);
        }

        // Initiate an abort with FCV change as the reason.
        auto abortReason0 = resharding::kFCVChangeAbortReason;
        coordinator->abort({abortReason0, resharding::AbortType::kAbortWithQuiesce});

        // Wait for the coordinator to transition to the "aborting" state.
        waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kAborting);

        stepDown(opCtx);
        coordinator.reset();
        // Unset the failpoint above to allow the coordinator to remove the state doc after stepup.
        pauseCoordinatorBeforeRemovingStateDoc->setMode(FailPoint::off, 0);

        // Pause the coordinator before it initializes the cancellation token holder after stepping
        // up.
        auto pauseBeforeCTHolderInitialization =
            globalFailPointRegistry().find("pauseBeforeCTHolderInitialization");
        auto timesEnteredFailPoint =
            pauseBeforeCTHolderInitialization->setMode(FailPoint::alwaysOn);

        stepUp(opCtx);
        pauseBeforeCTHolderInitialization->waitForTimesEntered(timesEnteredFailPoint + 1);

        auto instanceId =
            BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName << _reshardingUUID);
        coordinator = getCoordinator(opCtx, instanceId);

        // Try to abort again with user abort as the reason.
        auto abortReason1 = resharding::kUserAbortReason;
        coordinator->abort({abortReason1, abortTypeAfterFailover});

        // Unset the failpoint to allow the coordinator to initialize the cancellation token holder
        // and start recovering the resharding operation.
        pauseBeforeCTHolderInitialization->setMode(FailPoint::off, 0);

        makeRecipientsProceedToDone(opCtx);
        makeDonorsProceedToDone(opCtx);

        // Wait for completion and verify the original abort reason is still used.
        ASSERT_EQ(coordinator->getCompletionFuture().getNoThrow(), abortReason0);

        // Verify allowMigrations is restored after abort completes.
        ASSERT_TRUE(getCollectionCatalogEntry(opCtx).getAllowMigrations());

        // There should be no quiescing regardless of the abort type after failover, i.e. the wait
        // should finish immediately.
        coordinator->getQuiescePeriodFinishedFuture().wait();
    }

    void runReshardingAbortWithQuiescingBeforeFailover(
        CoordinatorStateEnum state, resharding::AbortType abortTypeAfterFailover) {
        // Set a large quiesce period to test that there is quiescing before failover and no
        // quiescing after failover if the abort type after failover specifies skip quiescing.
        RAIIServerParameterControllerForTest quiescePeriodMillis{
            "reshardingCoordinatorQuiescePeriodMillis", kOneDayMillis.count()};

        auto opCtx = operationContext();

        auto reshardingOptions = makeDefaultReshardingOptions();
        // Set a user resharding UUID to enable quiescing.
        reshardingOptions.userReshardingUUID = UUID::gen();
        auto coordinator = initializeAndGetCoordinator(_reshardingUUID,
                                                       _originalNss,
                                                       _tempNss,
                                                       _newShardKey,
                                                       _originalUUID,
                                                       _oldShardKey,
                                                       reshardingOptions);

        if (state >= CoordinatorStateEnum::kPreparingToDonate) {
            waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);
        }

        if (state >= CoordinatorStateEnum::kCloning) {
            makeDonorsReadyToDonateWithAssert(opCtx);
            waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCloning);
        }

        if (state >= CoordinatorStateEnum::kApplying) {
            makeRecipientsFinishedCloningWithAssert(opCtx);
            waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kApplying);
        }

        if (state >= CoordinatorStateEnum::kBlockingWrites) {
            coordinator->onOkayToEnterCritical();
            waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kBlockingWrites);
        }

        // Initiate an abort with FCV change as the reason.
        auto abortReason0 = resharding::kFCVChangeAbortReason;
        coordinator->abort({abortReason0, resharding::AbortType::kAbortWithQuiesce});

        // Wait for the coordinator to transition to the "quiesced" state.
        waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kQuiesced);

        // Verify allowMigrations is restored after reaching kQuiesced.
        ASSERT_TRUE(getCollectionCatalogEntry(opCtx).getAllowMigrations());

        stepDown(opCtx);
        coordinator.reset();

        // Pause the coordinator before it initializes the cancellation token holder after stepping
        // up.
        auto pauseBeforeCTHolderInitialization =
            globalFailPointRegistry().find("pauseBeforeCTHolderInitialization");
        auto timesEnteredFailPoint =
            pauseBeforeCTHolderInitialization->setMode(FailPoint::alwaysOn);

        stepUp(opCtx);
        pauseBeforeCTHolderInitialization->waitForTimesEntered(timesEnteredFailPoint + 1);

        auto instanceId =
            BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName << _reshardingUUID);
        coordinator = getCoordinator(opCtx, instanceId);

        // Try to abort again with user abort as the reason.
        auto abortReason1 = resharding::kUserAbortReason;
        coordinator->abort({abortReason1, abortTypeAfterFailover});
        pauseBeforeCTHolderInitialization->setMode(FailPoint::off, 0);

        // Wait for completion and verify the original abort reason is still used.
        ASSERT_EQ(coordinator->getCompletionFuture().getNoThrow(), abortReason0);
        // If the abort requests after failover specifies skip quiescing, there should be
        // no quiescing.
        if (abortTypeAfterFailover == resharding::AbortType::kAbortSkipQuiesce) {
            coordinator->getQuiescePeriodFinishedFuture().wait();
        } else {
            ASSERT(!coordinator->getQuiescePeriodFinishedFuture().isReady());
        }
    }

    const std::vector<ChunkRange> _oldChunkRanges = {
        ChunkRange(_oldShardKey.getKeyPattern().globalMin(), BSON("oldShardKey" << 12345)),
        ChunkRange(BSON("oldShardKey" << 12345), _oldShardKey.getKeyPattern().globalMax()),
    };
    const std::vector<ChunkRange> _newChunkRanges = {
        ChunkRange(_newShardKey.getKeyPattern().globalMin(), BSON("newShardKey" << 0)),
        ChunkRange(BSON("newShardKey" << 0), _newShardKey.getKeyPattern().globalMax()),
    };

protected:
    std::vector<ShardId> getShardIds() const override {
        return {shardId0, shardId1};
    }

    const ShardId shardId0{"shard0000"};
    const ShardId shardId1{"shard0001"};

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

class ReshardingCoordinatorServiceCriticalSectionWithBlockingDeltaTest
    : public ReshardingCoordinatorServiceTestBase {
public:
    ExternalStateForTest::Options getExternalStateOptions() const override {
        return {.documentsToCopy = documentsToCopy,
                .documentsDelta = documentsDelta,
                .blockInGetDocumentsDelta = true};
    }
};

TEST_F(ReshardingCoordinatorServiceCriticalSectionWithBlockingDeltaTest,
       CriticalSectionTimeoutAbortsWhileDeltaFetchIsInProgress) {
    RAIIServerParameterControllerForTest criticalSectionTimeout{
        "reshardingCriticalSectionTimeoutMillis", 1};

    PauseDuringStateTransitions stateTransitionsGuard{controller(),
                                                      CoordinatorStateEnum::kAborting};
    auto opCtx = operationContext();

    auto reshardingOptions = makeDefaultReshardingOptions();
    reshardingOptions.performVerification = true;

    auto coordinator = initializeAndGetCoordinator(_reshardingUUID,
                                                   _originalNss,
                                                   _tempNss,
                                                   _newShardKey,
                                                   _originalUUID,
                                                   _oldShardKey,
                                                   reshardingOptions);

    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);
    makeDonorsReadyToDonateWithAssert(opCtx);

    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCloning);
    makeRecipientsFinishedCloningWithAssert(opCtx);
    coordinator->onOkayToEnterCritical();

    // The coordinator now transitions to kBlockingWrites. The delta collector is launched
    // asynchronously that is configured to be stuck forever. The delta fetcher getting
    // stucked should not prevent the 1ms critical section timeout from aborting resharding.

    stateTransitionsGuard.wait(CoordinatorStateEnum::kAborting);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kAborting);

    ASSERT_THROWS_CODE(coordinator->getCompletionFuture().get(opCtx),
                       DBException,
                       ErrorCodes::ReshardingCriticalSectionTimeout);
}

TEST_F(ReshardingCoordinatorServiceTest, ReshardingCoordinatorSuccessfullyTransitionsTokDone) {
    runReshardingToCompletion();
}

TEST_F(ReshardingCoordinatorServiceTest, ReshardingCoordinatorSuccessfulWithRefresh) {
    RAIIServerParameterControllerForTest noRefreshFeatureFlagController(
        "featureFlagReshardingInitNoRefresh", false);
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

        LOGV2(5093701, "Running step down test case", "stepDownAfter"_attr = idl::serialize(state));

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
            client.findOne(NamespaceString::kConfigsvrCollectionsNamespace,
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
    newCoordinator->abort({resharding::kUserAbortReason, resharding::AbortType::kAbortSkipQuiesce});
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
        client.update(NamespaceString::kConfigsvrCollectionsNamespace,
                      BSON(CollectionType::kNssFieldName << _originalNss.ns_forTest()),
                      BSON("$set" << BSON(CollectionType::kAllowMigrationsFieldName << false)));
    }

    auto coordinator = ReshardingCoordinator::getOrCreate(opCtx, _service, doc.toBSON());
    ASSERT_THROWS_CODE(coordinator->getCompletionFuture().get(opCtx), DBException, 5808201);

    // Check that reshardCollection keeps allowMigrations setting intact.
    {
        DBDirectClient client(opCtx);
        CollectionType collDoc(
            client.findOne(NamespaceString::kConfigsvrCollectionsNamespace,
                           BSON(CollectionType::kNssFieldName << _originalNss.ns_forTest())));
        ASSERT_FALSE(collDoc.getAllowMigrations());
    }
}

TEST_F(ReshardingCoordinatorServiceTest,
       AllowMigrationsRestoredOnAbortBeforeInitializationCompletes) {
    auto fpAfterStopMigrations =
        globalFailPointRegistry().find("pauseAfterStoppingActiveMigrations");
    auto timesEnteredFailPoint = fpAfterStopMigrations->setMode(FailPoint::alwaysOn, 0);
    auto coordinator = initializeAndGetCoordinator();
    fpAfterStopMigrations->waitForTimesEntered(timesEnteredFailPoint + 1);

    ASSERT_FALSE(getCollectionCatalogEntry(operationContext()).getAllowMigrations());

    coordinator->abort({resharding::kUserAbortReason, resharding::AbortType::kAbortSkipQuiesce});
    fpAfterStopMigrations->setMode(FailPoint::off, 0);
    coordinator->getCompletionFuture().wait();

    ASSERT_TRUE(getCollectionCatalogEntry(operationContext()).getAllowMigrations());
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
    coordinator->abort({resharding::kUserAbortReason, resharding::AbortType::kAbortWithQuiesce});
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
    coordinator->abort({resharding::kUserAbortReason, resharding::AbortType::kAbortSkipQuiesce});
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
    stepDown(opCtx);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kAborting);

    ASSERT_THROWS_WITH_CHECK(
        coordinator->getCompletionFuture().get(), DBException, [&](const DBException& ex) {
            ASSERT_TRUE(ex.code() == ErrorCodes::CallbackCanceled ||
                        ex.code() == ErrorCodes::InterruptedDueToReplStateChange);
        });

    coordinator.reset();
    stepUp(opCtx);

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

    ASSERT_THROWS_CODE(coordinator->getCompletionFuture().get(opCtx),
                       DBException,
                       ErrorCodes::ReshardingCriticalSectionTimeout);
}

TEST_F(ReshardingCoordinatorServiceTest, ReshardingSendsCloneCmdNotRefresh) {
    const std::vector<CoordinatorStateEnum> states = {
        CoordinatorStateEnum::kPreparingToDonate,
    };

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

TEST_F(ReshardingCoordinatorServiceTest, CausalityBarrierSkippedOnInitialRun) {
    runReshardingToCompletion();
    ASSERT_EQ(externalState()->getCausalityBarrierInvokeCount(), 0);
}

TEST_F(ReshardingCoordinatorServiceTest, CausalityBarrierInvokedOnRecovery) {
    runReshardingToCompletionWithFailoverAt(CoordinatorStateEnum::kPreparingToDonate);
    ASSERT_EQ(externalState()->getCausalityBarrierInvokeCount(), 1);
}

TEST_F(ReshardingCoordinatorServiceTest, CausalityBarrierSkippedOnRecoveryWithoutFeatureFlag) {
    RAIIServerParameterControllerForTest noRefreshFeatureFlagController(
        "featureFlagReshardingInitNoRefresh", false);

    runReshardingToCompletionWithFailoverAt(CoordinatorStateEnum::kPreparingToDonate);
    ASSERT_EQ(externalState()->getCausalityBarrierInvokeCount(), 0);
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

    ASSERT_THROWS_CODE(
        coordinator->getCompletionFuture().get(opCtx), DBException, verifyFinalErrorCode);
}

class ReshardingCoordinatorServiceIncompleteDataVerificationTest
    : public ReshardingCoordinatorServiceTestBase {
public:
    ExternalStateForTest::Options getExternalStateOptions() const override {
        return {.documentsToCopy = documentsToCopy,
                .documentsDelta = documentsDelta,
                .verifyFinalErrorCode = ErrorCodes::ReshardingValidationIncompleteData};
    }
};

TEST_F(ReshardingCoordinatorServiceIncompleteDataVerificationTest,
       CommitIfIncompleteDataVerificationError) {
    auto reshardingOptions = makeDefaultReshardingOptions();

    runReshardingToCompletion(TransitionFunctionMap{},
                              nullptr /* stateTransitionsGuard */,
                              {CoordinatorStateEnum::kPreparingToDonate,
                               CoordinatorStateEnum::kCloning,
                               CoordinatorStateEnum::kApplying,
                               CoordinatorStateEnum::kBlockingWrites,
                               CoordinatorStateEnum::kCommitting},
                              reshardingOptions);
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

TEST_F(ReshardingCoordinatorServiceTest, ReshardingSendsDonorInitCmd) {
    // Force the legacy refresh path to throw. Successful resharding completion
    // confirms that the new init-cmd path was used instead.
    externalState()->throwUnrecoverableErrorIn(CoordinatorStateEnum::kPreparingToDonate,
                                               kEstablishAllDonorsAsParticipants);

    runReshardingToCompletion();
}

TEST_F(ReshardingCoordinatorServiceTest, ReshardingSendsRecipientInitCmd) {
    // Force the legacy refresh path to throw. Successful resharding completion
    // confirms that the new init-cmd path was used instead.
    externalState()->throwUnrecoverableErrorIn(CoordinatorStateEnum::kPreparingToDonate,
                                               kEstablishAllRecipientsAsParticipants);

    runReshardingToCompletion();
}

TEST_F(ReshardingCoordinatorServiceTest,
       UnrecoverableErrorWhileEstablishingDonorsDuringPreparingToDonate) {
    // Force the legacy path so establishAllDonorsAsParticipants is called.
    RAIIServerParameterControllerForTest noRefreshFeatureFlagController(
        "featureFlagReshardingInitNoRefresh", false);

    runReshardingWithUnrecoverableError(CoordinatorStateEnum::kPreparingToDonate,
                                        kEstablishAllDonorsAsParticipants);
}

TEST_F(ReshardingCoordinatorServiceTest,
       UnrecoverableErrorWhileEstablishingRecipientsDuringPreparingToDonate) {
    // Force the legacy path so kEstablishAllRecipientsAsParticipants is called.
    RAIIServerParameterControllerForTest noRefreshFeatureFlagController(
        "featureFlagReshardingInitNoRefresh", false);

    runReshardingWithUnrecoverableError(CoordinatorStateEnum::kPreparingToDonate,
                                        kEstablishAllRecipientsAsParticipants);
}

TEST_F(ReshardingCoordinatorServiceTest, UnrecoverableErrorDuringCloning) {
    runReshardingWithUnrecoverableError(CoordinatorStateEnum::kCloning,
                                        kGetDocumentsToCopyFromDonors);
}

TEST_F(ReshardingCoordinatorServiceTest, UnrecoverableErrorDuringApplying) {
    RAIIServerParameterControllerForTest noRefreshFeatureFlagController(
        "featureFlagReshardingNoRefreshApplyingAndBlockingWrites", false);
    runReshardingWithUnrecoverableError(CoordinatorStateEnum::kApplying, kTellAllDonorsToRefresh);
}

TEST_F(ReshardingCoordinatorServiceTest, UnrecoverableErrorDuringBlockingWrites) {
    RAIIServerParameterControllerForTest noRefreshFeatureFlagController(
        "featureFlagReshardingNoRefreshApplyingAndBlockingWrites", false);
    runReshardingWithUnrecoverableError(CoordinatorStateEnum::kBlockingWrites,
                                        kTellAllDonorsToRefresh);
}

TEST_F(ReshardingCoordinatorServiceTest, UnrecoverableErrorInDeltaCollectorDuringBlockingWrites) {
    // The delta collector runs asynchronously after the coordinator enters kBlockingWrites. Its
    // error is only surfaced when _verifyFinalCollection awaits the delta future, which happens
    // after all recipients reach strict consistency. Unlike kTellAllDonorsToRefresh (which fails
    // synchronously before the recipients wait), we must drive recipients to strict consistency
    // so the coordinator calls _verifyFinalCollection and observes the error. This means
    // runReshardingWithUnrecoverableError cannot be used here.
    PauseDuringStateTransitions stateTransitionsGuard{
        controller(), {CoordinatorStateEnum::kBlockingWrites, CoordinatorStateEnum::kAborting}};

    externalState()->throwUnrecoverableErrorIn(CoordinatorStateEnum::kBlockingWrites,
                                               kGetDocumentsDeltaFromDonors);

    auto opCtx = operationContext();
    auto coordinator = initializeAndGetCoordinator();

    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);
    makeDonorsReadyToDonateWithAssert(opCtx);

    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCloning);
    makeRecipientsFinishedCloningWithAssert(opCtx);

    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kApplying);
    coordinator->onOkayToEnterCritical();

    stateTransitionsGuard.wait(CoordinatorStateEnum::kBlockingWrites);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kBlockingWrites);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kBlockingWrites);
    makeRecipientsBeInStrictConsistencyWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kAborting);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kAborting);

    ASSERT_EQ(coordinator->getCompletionFuture().getNoThrow(), ErrorCodes::InternalError);
    checkCoordinatorDocumentRemoved(opCtx);
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
                                                      CoordinatorStateEnum::kApplying,
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

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithoutQuiescingBeforeFailover_AbortWithQuiescingAfterFailover_PreparingToDonate) {
    runReshardingAbortWithoutQuiescingBeforeFailover(CoordinatorStateEnum::kPreparingToDonate,
                                                     resharding::AbortType::kAbortWithQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithoutQuiescingBeforeFailover_AbortWithoutQuiescingAfterFailover_PreparingToDonate) {
    runReshardingAbortWithoutQuiescingBeforeFailover(CoordinatorStateEnum::kPreparingToDonate,
                                                     resharding::AbortType::kAbortSkipQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithQuiescingBeforeFailover_AbortWithQuiescingAfterFailover_PreparingToDonate) {
    runReshardingAbortWithQuiescingBeforeFailover(CoordinatorStateEnum::kPreparingToDonate,
                                                  resharding::AbortType::kAbortWithQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithQuiescingBeforeFailover_AbortWithoutQuiescingAfterFailover_PreparingToDonate) {
    runReshardingAbortWithQuiescingBeforeFailover(CoordinatorStateEnum::kPreparingToDonate,
                                                  resharding::AbortType::kAbortSkipQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithoutQuiescingBeforeFailover_AbortWithQuiescingAfterFailover_Cloning) {
    runReshardingAbortWithoutQuiescingBeforeFailover(CoordinatorStateEnum::kCloning,
                                                     resharding::AbortType::kAbortWithQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithoutQuiescingBeforeFailover_AbortWithoutQuiescingAfterFailover_Cloning) {
    runReshardingAbortWithoutQuiescingBeforeFailover(CoordinatorStateEnum::kCloning,
                                                     resharding::AbortType::kAbortSkipQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithQuiescingBeforeFailover_AbortWithQuiescingAfterFailover_Cloning) {
    runReshardingAbortWithQuiescingBeforeFailover(CoordinatorStateEnum::kCloning,
                                                  resharding::AbortType::kAbortWithQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithQuiescingBeforeFailover_AbortWithoutQuiescingAfterFailover_Cloning) {
    runReshardingAbortWithQuiescingBeforeFailover(CoordinatorStateEnum::kCloning,
                                                  resharding::AbortType::kAbortSkipQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithoutQuiescingBeforeFailover_AbortWithQuiescingAfterFailover_Applying) {
    runReshardingAbortWithoutQuiescingBeforeFailover(CoordinatorStateEnum::kApplying,
                                                     resharding::AbortType::kAbortWithQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithoutQuiescingBeforeFailover_AbortWithoutQuiescingAfterFailover_Applying) {
    runReshardingAbortWithoutQuiescingBeforeFailover(CoordinatorStateEnum::kApplying,
                                                     resharding::AbortType::kAbortSkipQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithQuiescingBeforeFailover_AbortWithQuiescingAfterFailover_Applying) {
    runReshardingAbortWithQuiescingBeforeFailover(CoordinatorStateEnum::kApplying,
                                                  resharding::AbortType::kAbortWithQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithQuiescingBeforeFailover_AbortWithoutQuiescingAfterFailover_Applying) {
    runReshardingAbortWithQuiescingBeforeFailover(CoordinatorStateEnum::kApplying,
                                                  resharding::AbortType::kAbortSkipQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithoutQuiescingBeforeFailover_AbortWithQuiescingAfterFailover_BlockingWrites) {
    runReshardingAbortWithoutQuiescingBeforeFailover(CoordinatorStateEnum::kBlockingWrites,
                                                     resharding::AbortType::kAbortWithQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithoutQuiescingBeforeFailover_AbortWithoutQuiescingAfterFailover_BlockingWrites) {
    runReshardingAbortWithoutQuiescingBeforeFailover(CoordinatorStateEnum::kBlockingWrites,
                                                     resharding::AbortType::kAbortSkipQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithQuiescingBeforeFailover_AbortWithQuiescingAfterFailover_BlockingWrites) {
    runReshardingAbortWithQuiescingBeforeFailover(CoordinatorStateEnum::kBlockingWrites,
                                                  resharding::AbortType::kAbortWithQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest,
       AbortWithQuiescingBeforeFailover_AbortWithoutQuiescingAfterFailover_BlockingWrites) {
    runReshardingAbortWithQuiescingBeforeFailover(CoordinatorStateEnum::kBlockingWrites,
                                                  resharding::AbortType::kAbortSkipQuiesce);
}

TEST_F(ReshardingCoordinatorServiceTest, AbortDuringCommitDoesNotCauseInfiniteRetryLoop) {
    const std::vector<CoordinatorStateEnum> states = {CoordinatorStateEnum::kPreparingToDonate,
                                                      CoordinatorStateEnum::kCloning,
                                                      CoordinatorStateEnum::kApplying,
                                                      CoordinatorStateEnum::kBlockingWrites,
                                                      CoordinatorStateEnum::kCommitting};

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
    makeRecipientsFinishedCloningWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kApplying);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kApplying);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kApplying);
    coordinator->onOkayToEnterCritical();

    stateTransitionsGuard.wait(CoordinatorStateEnum::kBlockingWrites);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kBlockingWrites);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kBlockingWrites);
    makeRecipientsBeInStrictConsistencyWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kCommitting);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kCommitting);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCommitting);

    // Call abort after the coordinator has entered the committing state.
    // This should be ignored because the resharding operation is past the point of no return.
    coordinator->abort({resharding::kUserAbortReason, resharding::AbortType::kAbortSkipQuiesce});

    makeDonorsProceedToDoneWithAssert(opCtx);
    makeRecipientsProceedToDoneWithAssert(opCtx);

    ASSERT_OK(coordinator->getCompletionFuture().getNoThrow());
}

TEST_F(ReshardingCoordinatorServiceTest, TransientErrorAfterCoordinatorDocRemovedDoesNotFassert) {
    const std::vector<CoordinatorStateEnum> states = {CoordinatorStateEnum::kPreparingToDonate,
                                                      CoordinatorStateEnum::kCloning,
                                                      CoordinatorStateEnum::kApplying,
                                                      CoordinatorStateEnum::kBlockingWrites,
                                                      CoordinatorStateEnum::kCommitting};

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
    makeRecipientsFinishedCloningWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kApplying);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kApplying);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kApplying);
    coordinator->onOkayToEnterCritical();

    stateTransitionsGuard.wait(CoordinatorStateEnum::kBlockingWrites);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kBlockingWrites);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kBlockingWrites);
    makeRecipientsBeInStrictConsistencyWithAssert(opCtx);

    stateTransitionsGuard.wait(CoordinatorStateEnum::kCommitting);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kCommitting);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kCommitting);

    auto pauseBeforeRemovingDoc =
        globalFailPointRegistry().find("reshardingPauseCoordinatorBeforeRemovingStateDoc");
    auto timesEntered = pauseBeforeRemovingDoc->setMode(FailPoint::alwaysOn, 0);

    makeDonorsProceedToDoneWithAssert(opCtx);
    makeRecipientsProceedToDoneWithAssert(opCtx);

    // Wait for the coordinator to reach the pause point (it has finished awaiting participants
    // and is about to remove the coordinator doc).
    pauseBeforeRemovingDoc->waitForTimesEntered(timesEntered + 1);

    // Arm the WC failure failpoint so the next transaction (the one that removes the coordinator
    // document) will commit successfully but report a transient error. This causes
    // WithAutomaticRetry to retry the entire post-commit block.
    globalFailPointRegistry()
        .find("shardingCatalogManagerWithTransactionFailWCAfterCommit")
        ->setMode(FailPoint::nTimes, 1);

    // Unpause: the coordinator removes the doc, gets the transient error, retries, and the
    // idempotent check at the top of the retry block detects the doc is gone and short-circuits.
    pauseBeforeRemovingDoc->setMode(FailPoint::off, 0);

    // The coordinator must complete successfully.
    ASSERT_OK(coordinator->getCompletionFuture().getNoThrow());
}

TEST_F(ReshardingCoordinatorServiceTest, NoRefreshBlockingWritesWithFeatureFlag) {
    RAIIServerParameterControllerForTest noRefreshFeatureFlagController(
        "featureFlagReshardingNoRefreshApplyingAndBlockingWrites", true);
    RAIIServerParameterControllerForTest initNoRefreshFeatureFlagController(
        "featureFlagReshardingInitNoRefresh", true);
    externalState()->throwUnrecoverableErrorIn(CoordinatorStateEnum::kBlockingWrites,
                                               kTellAllDonorsToRefresh);

    runReshardingToCompletion();
}

TEST_F(ReshardingCoordinatorServiceTest, NoRefreshApplyingWithFeatureFlag) {
    RAIIServerParameterControllerForTest noRefreshFeatureFlagController(
        "featureFlagReshardingNoRefreshApplyingAndBlockingWrites", true);
    RAIIServerParameterControllerForTest initNoRefreshFeatureFlagController(
        "featureFlagReshardingInitNoRefresh", true);
    externalState()->throwUnrecoverableErrorIn(CoordinatorStateEnum::kApplying,
                                               kTellAllDonorsToRefresh);

    runReshardingToCompletion();
}

TEST_F(ReshardingCoordinatorServiceTest, SkipsParticipantWaitOnAbort) {
    PauseDuringStateTransitions stateTransitionsGuard{controller(),
                                                      CoordinatorStateEnum::kPreparingToDonate};

    auto opCtx = operationContext();
    auto coordinator = initializeAndGetCoordinator();

    // Wait until kPreparingToDonate is committed to ensure coordinator sends abort commands to
    // participants.
    stateTransitionsGuard.wait(CoordinatorStateEnum::kPreparingToDonate);
    stateTransitionsGuard.unset(CoordinatorStateEnum::kPreparingToDonate);
    waitUntilCommittedCoordinatorDocReach(opCtx, CoordinatorStateEnum::kPreparingToDonate);

    {
        auto coordDoc = getCoordinatorDoc(opCtx);
        for (const auto& donor : coordDoc.getDonorShards()) {
            ASSERT_EQ(donor.getMutableState().getState(), DonorStateEnum::kUnused);
        }
        for (const auto& recipient : coordDoc.getRecipientShards()) {
            ASSERT_EQ(recipient.getMutableState().getState(), RecipientStateEnum::kUnused);
        }
    }

    // Abort while all participants are still in kUnused, which previously would cause a hang on
    // abort. See SERVER-92857.
    coordinator->abort({resharding::kUserAbortReason, resharding::AbortType::kAbortSkipQuiesce});

    ASSERT_EQ(coordinator->getCompletionFuture().getNoThrow(),
              ErrorCodes::ReshardCollectionAborted);
}

TEST_F(ReshardingCoordinatorServiceTest,
       ReshardingFailsWhenSearchIndexCheckThrowsUnrecoverableError) {
    externalState()->pushSearchIndexError(ErrorCodes::InternalError);
    auto opCtx = operationContext();
    auto coordinator = initializeAndGetCoordinator();
    ASSERT_THROWS_CODE(
        coordinator->getCompletionFuture().get(opCtx), DBException, ErrorCodes::InternalError);
}

TEST_F(ReshardingCoordinatorServiceTest, ReshardingSucceedsAfterSearchIndexCheckRetryableError) {
    externalState()->pushSearchIndexError(ErrorCodes::HostUnreachable);
    runReshardingToCompletion();
}

TEST_F(ReshardingCoordinatorServiceTest, ReshardingFailsWithIllegalOperationWhenSearchIndexExists) {
    externalState()->pushSearchIndexResult(true);
    auto opCtx = operationContext();
    auto coordinator = initializeAndGetCoordinator();
    ASSERT_THROWS_CODE(
        coordinator->getCompletionFuture().get(opCtx), DBException, ErrorCodes::IllegalOperation);
}

}  // namespace
}  // namespace mongo
