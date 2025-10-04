/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_coordinator_dao.h"

#include "mongo/db/s/resharding/resharding_coordinator_service_util.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/make_array_type.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace resharding {

struct SpyingDaoStorageClientState {
    BSONObj lastRequest;
    ReshardingCoordinatorDocument document;
};

class SpyingDaoStorageClient : public DaoStorageClient {
public:
    SpyingDaoStorageClient(std::shared_ptr<SpyingDaoStorageClientState> state) : _state{state} {}

    void alterState(OperationContext* opCtx, const BatchedCommandRequest& request) override {
        _state->lastRequest = request.toBSON();
    }

    ReshardingCoordinatorDocument readState(OperationContext* opCtx,
                                            const UUID& reshardingUUID) override {
        return _state->document;
    }

private:
    std::shared_ptr<SpyingDaoStorageClientState> _state;
};

class SpyingDaoStorageClientFactory : public DaoStorageClientFactory {
public:
    SpyingDaoStorageClientFactory(std::shared_ptr<SpyingDaoStorageClientState> state)
        : _state{state} {}

    std::unique_ptr<DaoStorageClient> createDaoStorageClient(
        boost::optional<TxnNumber> txnNumber) override {
        return std::make_unique<SpyingDaoStorageClient>(_state);
    }

    static std::tuple<std::unique_ptr<SpyingDaoStorageClientFactory>,
                      std::shared_ptr<SpyingDaoStorageClientState>>
    create() {
        auto state = std::make_shared<SpyingDaoStorageClientState>();
        auto factory = std::make_unique<SpyingDaoStorageClientFactory>(state);
        return {std::move(factory), state};
    }

private:
    std::shared_ptr<SpyingDaoStorageClientState> _state;
};

struct PhaseTransitionTestCase {
    CoordinatorStateEnum initialPhase;
    std::function<void()> transitionFn;
    boost::optional<BSONObj> set = boost::none;
    boost::optional<BSONObj> unset = boost::none;
};

class ReshardingCoordinatorDaoFixture : public unittest::Test {
protected:
    void setUp() override {
        auto [clientFactory, state] = SpyingDaoStorageClientFactory::create();
        _state = std::move(state);
        _dao = std::make_unique<ReshardingCoordinatorDao>(_uuid, std::move(clientFactory));
    }

    BSONObj wrapUpdate(boost::optional<BSONObj> set, boost::optional<BSONObj> unset) {
        BSONObjBuilder update;
        if (set) {
            update.append("$set", *set);
        }
        if (unset) {
            update.append("$unset", *unset);
        }
        return BSON_ARRAY(BSON("q" << BSON("_id" << _uuid) << "u" << update.obj() << "multi"
                                   << false << "upsert" << false));
    }

    void runPhaseTransitionTest(PhaseTransitionTestCase testCase) {
        _state->document.setState(testCase.initialPhase);
        testCase.transitionFn();

        const auto& lastRequest = _state->lastRequest;
        ASSERT_EQUALS(lastRequest.getStringField("update"),
                      NamespaceString::kConfigReshardingOperationsNamespace.coll());
        auto updates = lastRequest.getObjectField("updates");
        ASSERT_BSONOBJ_EQ_UNORDERED(updates, wrapUpdate(testCase.set, testCase.unset));
    }

    UUID _uuid{UUID::gen()};
    OperationContext* _opCtx = nullptr;
    std::unique_ptr<ClockSourceMock> _clock = std::make_unique<ClockSourceMock>();
    std::shared_ptr<SpyingDaoStorageClientState> _state;
    std::unique_ptr<ReshardingCoordinatorDao> _dao;
};

TEST_F(ReshardingCoordinatorDaoFixture, GetPhase) {
    _state->document.setState(CoordinatorStateEnum::kCloning);
    auto phase = _dao->getPhase(_opCtx);
    ASSERT_EQUALS(phase, CoordinatorStateEnum::kCloning);

    // Test with a different phase.
    _state->document.setState(CoordinatorStateEnum::kApplying);
    phase = _dao->getPhase(_opCtx);
    ASSERT_EQUALS(phase, CoordinatorStateEnum::kApplying);
}

TEST_F(ReshardingCoordinatorDaoFixture, TransitionToPreparingToDonatePhaseSucceeds) {
    DonorShardContext donorContext;
    donorContext.setState(DonorStateEnum::kUnused);
    std::vector<DonorShardEntry> donorShards = {{ShardId("donorShard1"), donorContext},
                                                {ShardId("donorShard2"), donorContext}};

    RecipientShardContext recipientContext;
    recipientContext.setState(RecipientStateEnum::kUnused);
    std::vector<RecipientShardEntry> recipientShards = {
        {ShardId("recipientShard1"), recipientContext},
        {ShardId("recipientShard2"), recipientContext}};

    std::vector<ChunkType> initialChunks;
    auto shardsAndChunks =
        ParticipantShardsAndChunks({donorShards, recipientShards, initialChunks});

    BSONArrayBuilder donorShardsArrayBuilder;
    for (const auto& shard : donorShards) {
        donorShardsArrayBuilder.append(shard.toBSON());
    }

    BSONArrayBuilder recipientShardsArrayBuilder;
    for (const auto& shard : recipientShards) {
        recipientShardsArrayBuilder.append(shard.toBSON());
    }

    runPhaseTransitionTest(PhaseTransitionTestCase{
        .initialPhase = CoordinatorStateEnum::kInitializing,
        .transitionFn =
            [&]() { _dao->transitionToPreparingToDonatePhase(_opCtx, shardsAndChunks); },
        .set = BSON("state" << "preparing-to-donate"
                            << "donorShards" << donorShardsArrayBuilder.arr() << "recipientShards"
                            << recipientShardsArrayBuilder.arr()),
        .unset = BSON("presetReshardedChunks" << ""
                                              << "zones"
                                              << "")});
}

DEATH_TEST_F(ReshardingCoordinatorDaoFixture,
             TransitionToPreparingToDonateFailsFromInvalidPreviousPhase,
             "invariant") {
    DonorShardContext donorContext;
    donorContext.setState(DonorStateEnum::kUnused);
    std::vector<DonorShardEntry> donorShards = {{ShardId("donorShard1"), donorContext},
                                                {ShardId("donorShard2"), donorContext}};

    RecipientShardContext recipientContext;
    recipientContext.setState(RecipientStateEnum::kUnused);
    std::vector<RecipientShardEntry> recipientShards = {
        {ShardId("recipientShard1"), recipientContext},
        {ShardId("recipientShard2"), recipientContext}};

    std::vector<ChunkType> initialChunks;
    auto shardsAndChunks =
        ParticipantShardsAndChunks({donorShards, recipientShards, initialChunks});

    runPhaseTransitionTest(PhaseTransitionTestCase{
        .initialPhase = CoordinatorStateEnum::kPreparingToDonate, .transitionFn = [&]() {
            _dao->transitionToPreparingToDonatePhase(_opCtx, shardsAndChunks);
        }});
}

TEST_F(ReshardingCoordinatorDaoFixture, TransitionToCloningPhaseSucceeds) {
    auto bytesToCopy = 100;
    auto documentsToCopy = 5;
    ReshardingApproxCopySize approxCopySize;
    approxCopySize.setApproxBytesToCopy(bytesToCopy);
    approxCopySize.setApproxDocumentsToCopy(documentsToCopy);

    Timestamp cloneTimestamp(10, 50);
    auto cloneStartTime = _clock->now();

    runPhaseTransitionTest(PhaseTransitionTestCase{
        .initialPhase = CoordinatorStateEnum::kPreparingToDonate,
        .transitionFn =
            [&]() {
                _dao->transitionToCloningPhase(
                    _opCtx, cloneStartTime, cloneTimestamp, approxCopySize);
            },
        .set = BSON("state" << "cloning"
                            << "cloneTimestamp" << cloneTimestamp << "approxBytesToCopy"
                            << bytesToCopy << "approxDocumentsToCopy" << documentsToCopy
                            << "metrics.documentCopy.start" << cloneStartTime)});
}

DEATH_TEST_F(ReshardingCoordinatorDaoFixture,
             TransitionToCloningPhasePreviousStateInvariant,
             "invariant") {
    ReshardingApproxCopySize approxCopySize;
    approxCopySize.setApproxBytesToCopy(100);
    approxCopySize.setApproxDocumentsToCopy(5);

    Timestamp cloneTimestamp(10, 50);
    auto cloneStartTime = _clock->now();

    runPhaseTransitionTest(PhaseTransitionTestCase{
        .initialPhase = CoordinatorStateEnum::kCloning, .transitionFn = [&]() {
            _dao->transitionToCloningPhase(_opCtx, cloneStartTime, cloneTimestamp, approxCopySize);
        }});
}

TEST_F(ReshardingCoordinatorDaoFixture, TransitionToApplyingPhaseSucceeds) {
    auto applyStartTime = _clock->now();

    runPhaseTransitionTest(PhaseTransitionTestCase{
        .initialPhase = CoordinatorStateEnum::kCloning,
        .transitionFn = [&]() { _dao->transitionToApplyingPhase(_opCtx, applyStartTime); },
        .set = BSON("state" << "applying"
                            << "metrics.documentCopy.stop" << applyStartTime
                            << "metrics.oplogApplication.start" << applyStartTime)});
}

DEATH_TEST_F(ReshardingCoordinatorDaoFixture,
             TransitionToApplyingFailsFromInvalidPreviousPhase,
             "invariant") {
    auto applyStartTime = _clock->now();

    runPhaseTransitionTest(PhaseTransitionTestCase{
        .initialPhase = CoordinatorStateEnum::kApplying, .transitionFn = [&]() {
            _dao->transitionToApplyingPhase(_opCtx, applyStartTime);
        }});
}

TEST_F(ReshardingCoordinatorDaoFixture, TransitionToBlockingWritesPhaseSucceeds) {
    auto now = _clock->now();
    auto criticalSectionExpiresAt = now + Seconds(5);

    runPhaseTransitionTest(PhaseTransitionTestCase{
        .initialPhase = CoordinatorStateEnum::kApplying,
        .transitionFn =
            [&]() { _dao->transitionToBlockingWritesPhase(_opCtx, now, criticalSectionExpiresAt); },
        .set = BSON("state" << "blocking-writes"
                            << "criticalSectionExpiresAt" << criticalSectionExpiresAt
                            << "metrics.oplogApplication.stop" << now)});
}

DEATH_TEST_F(ReshardingCoordinatorDaoFixture,
             TransitionToBlockingWritesPhasePreviousStateInvariant,
             "invariant") {
    auto now = _clock->now();
    auto criticalSectionExpiresAt = now + Seconds(5);

    runPhaseTransitionTest(PhaseTransitionTestCase{
        .initialPhase = CoordinatorStateEnum::kAborting, .transitionFn = [&]() {
            _dao->transitionToBlockingWritesPhase(_opCtx, now, criticalSectionExpiresAt);
        }});
}

TEST_F(ReshardingCoordinatorDaoFixture, TransitionToAbortPhaseSucceeds) {
    auto now = _clock->now();
    Status abortReason{ErrorCodes::InternalError, "Something went horribly wrong"};

    runPhaseTransitionTest(PhaseTransitionTestCase{
        .initialPhase = CoordinatorStateEnum::kPreparingToDonate,
        .transitionFn = [&]() { _dao->transitionToAbortingPhase(_opCtx, now, abortReason); },
        .set =
            BSON("state" << "aborting"
                         << "abortReason"
                         << resharding::serializeAndTruncateReshardingErrorIfNeeded(abortReason))});
}

TEST_F(ReshardingCoordinatorDaoFixture, TransitionToAbortPhaseTruncatesLongErrors) {
    auto now = _clock->now();
    std::string longMessage(6000, 'x');
    Status abortReason{ErrorCodes::InternalError, longMessage};

    runPhaseTransitionTest(PhaseTransitionTestCase{
        .initialPhase = CoordinatorStateEnum::kPreparingToDonate,
        .transitionFn = [&]() { _dao->transitionToAbortingPhase(_opCtx, now, abortReason); },
        .set =
            BSON("state" << "aborting"
                         << "abortReason"
                         << resharding::serializeAndTruncateReshardingErrorIfNeeded(abortReason))});
}

TEST_F(ReshardingCoordinatorDaoFixture, TransitionToAbortPhaseEndsCloningMetrics) {
    auto now = _clock->now();
    Status abortReason{ErrorCodes::InternalError, "Something went horribly wrong"};

    runPhaseTransitionTest(PhaseTransitionTestCase{
        .initialPhase = CoordinatorStateEnum::kCloning,
        .transitionFn = [&]() { _dao->transitionToAbortingPhase(_opCtx, now, abortReason); },
        .set = BSON("state" << "aborting"
                            << "abortReason"
                            << resharding::serializeAndTruncateReshardingErrorIfNeeded(abortReason)
                            << "metrics.documentCopy.stop" << now)});
}

TEST_F(ReshardingCoordinatorDaoFixture, TransitionToAbortPhaseEndsApplyingMetrics) {
    auto now = _clock->now();
    Status abortReason{ErrorCodes::InternalError, "Something went horribly wrong"};

    runPhaseTransitionTest(PhaseTransitionTestCase{
        .initialPhase = CoordinatorStateEnum::kApplying,
        .transitionFn = [&]() { _dao->transitionToAbortingPhase(_opCtx, now, abortReason); },
        .set = BSON("state" << "aborting"
                            << "abortReason"
                            << resharding::serializeAndTruncateReshardingErrorIfNeeded(abortReason)
                            << "metrics.oplogApplication.stop" << now)});
}

DEATH_TEST_F(ReshardingCoordinatorDaoFixture, TransitionToAbortCannotHaveOkReason, "invariant") {
    auto now = _clock->now();
    Status abortReason = Status::OK();

    runPhaseTransitionTest(PhaseTransitionTestCase{
        .initialPhase = CoordinatorStateEnum::kApplying, .transitionFn = [&]() {
            _dao->transitionToAbortingPhase(_opCtx, now, abortReason);
        }});
}

DEATH_TEST_F(ReshardingCoordinatorDaoFixture,
             TransitionToAbortPhasePreviousStateInvariant,
             "invariant") {
    auto now = _clock->now();
    Status abortReason{ErrorCodes::InternalError, "Something went horribly wrong"};

    runPhaseTransitionTest(PhaseTransitionTestCase{
        .initialPhase = CoordinatorStateEnum::kCommitting, .transitionFn = [&]() {
            _dao->transitionToAbortingPhase(_opCtx, now, abortReason);
        }});
}

}  // namespace resharding
}  // namespace mongo
