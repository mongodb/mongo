// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/timestamp.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2_test_helpers.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace {

using namespace test;

class DSV2StageInvalidStateTransitionsTest : public ChangeStreamStageTestNoSetup {};
using DSV2StageInvalidStateTransitionsTestDeathTest = DSV2StageInvalidStateTransitionsTest;

// Invalid state transition tests.
// -------------------------------

// Tests that a previous exception must have been registered when running the state machine when the
// start state is kFinal.
DEATH_TEST_REGEX_F(DSV2StageInvalidStateTransitionsTestDeathTest,
                   StateMachineFailsOnStateFinal,
                   "Tripwire assertion.*10657532") {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get());
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    docSource->setState_forTest(V2Stage::State::kFinal, false /* validateStateTransition */);
    ASSERT_THROWS_CODE(docSource->runGetNextStateMachine_forTest(), AssertionException, 10657532);
}

// Tests that an exception is thrown when the state is set to the existing state using 'setState()'
// / 'setState_forTest()'.
DEATH_TEST_REGEX_F(DSV2StageInvalidStateTransitionsTestDeathTest,
                   CheckRepeatedState,
                   "Tripwire assertion.*10657503") {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kStrict));
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), nullptr);

    // Check state invalid transition from Final to kWaiting to kWaiting.
    auto state = V2Stage::State::kWaiting;
    docSource->setState_forTest(state, false /* validateStateTransition */);

    ASSERT_THROWS_CODE(docSource->setState_forTest(state, true /* validateStateTransition */),
                       AssertionException,
                       10657503);
}

// Tests that an exception is thrown when trying to change the state from the end state kFinal to
// another state.
DEATH_TEST_REGEX_F(DSV2StageInvalidStateTransitionsTestDeathTest,
                   CheckStateTransitionBackFromFinalState,
                   "Tripwire assertion.*10657504") {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kStrict));
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), nullptr);

    docSource->setState_forTest(V2Stage::State::kFinal, false /* validateStateTransition */);

    for (auto state : {V2Stage::State::kWaiting,
                       V2Stage::State::kFetchingInitialization,
                       V2Stage::State::kDowngrading}) {
        ASSERT_THROWS_CODE(docSource->setState_forTest(state, true /* validateStateTransition */),
                           AssertionException,
                           10657504);
    }
}

// Tests that an exception is thrown when trying to set the state back to kUninitialized.
DEATH_TEST_REGEX_F(DSV2StageInvalidStateTransitionsTestDeathTest,
                   CheckStateTransitionBackToUninitialized,
                   "Tripwire assertion.*10657505") {
    getExpCtx()->setChangeStreamSpec(
        buildChangeStreamSpec(Timestamp(42, 0), ChangeStreamReadMode::kStrict));

    auto changeStreamReaderBuilder = std::make_shared<ChangeStreamReaderBuilderMock>(
        [](OperationContext* opCtx, const ChangeStream& changeStream) {
            return std::make_unique<ChangeStreamShardTargeterMock>();
        });
    auto dataToShardsAllocationQueryService =
        std::make_unique<DataToShardsAllocationQueryServiceMock>();

    auto params =
        buildParametersForTest(getExpCtx(),
                               V2StageTestHelpers::kDefaultMinAllocationToShardsPollPeriodSecs,
                               changeStreamReaderBuilder.get(),
                               dataToShardsAllocationQueryService.get());
    auto docSource = make_intrusive<V2Stage>(getExpCtx(), params);

    for (auto state : {V2Stage::State::kWaiting,
                       V2Stage::State::kFetchingInitialization,
                       V2Stage::State::kDowngrading}) {
        docSource->setState_forTest(state, false /* validateStateTransition */);

        ASSERT_THROWS_CODE(docSource->setState_forTest(V2Stage::State::kUninitialized,
                                                       true /* validateStateTransition */),
                           AssertionException,
                           10657505);
    }
}

}  // namespace
}  // namespace mongo

