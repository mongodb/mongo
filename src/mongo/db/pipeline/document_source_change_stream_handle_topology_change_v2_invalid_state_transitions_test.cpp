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

