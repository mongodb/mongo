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

#include "mongo/db/s/resharding/resharding_participant_cancel_state.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ReshardingParticipantCancelStateTest, DefaultConstruction) {
    ReshardingParticipantCancelState state;
    ASSERT_FALSE(state.isSteppingDown());
    ASSERT_FALSE(state.isAbortedOrSteppingDown());
    ASSERT_FALSE(state.isEffectiveTokenCancelled());
}

TEST(ReshardingParticipantCancelStateTest, CreateChangeStreamsMonitorAbortSourceIsIdempotent) {
    ReshardingParticipantCancelState state;
    state.createChangeStreamsMonitorAbortSource();
    state.createChangeStreamsMonitorAbortSource();

    // No crash means idempotency is satisfied.
    ASSERT_FALSE(state.isEffectiveTokenCancelled());
}

TEST(ReshardingParticipantCancelStateTest, AbortCascadesToCSMToken) {
    ReshardingParticipantCancelState state;
    state.createChangeStreamsMonitorAbortSource();
    ASSERT_FALSE(state.isEffectiveTokenCancelled());

    state.abort();
    ASSERT_TRUE(state.isAbortedOrSteppingDown());
    ASSERT_TRUE(state.isEffectiveTokenCancelled());
}

TEST(ReshardingParticipantCancelStateTest, StepdownTokenPassedToConstructorCascadesToCSMToken) {
    CancellationSource stepdownSource;
    ReshardingParticipantCancelState state(stepdownSource.token());
    state.createChangeStreamsMonitorAbortSource();
    ASSERT_FALSE(state.isEffectiveTokenCancelled());

    stepdownSource.cancel();
    ASSERT_TRUE(state.isSteppingDown());
    ASSERT_TRUE(state.isAbortedOrSteppingDown());
    ASSERT_TRUE(state.isEffectiveTokenCancelled());
}

TEST(ReshardingParticipantCancelStateTest, StepdownCascadesToCSMToken) {
    CancellationSource stepdownSource;
    ReshardingParticipantCancelState state;
    state.attachStepdownToken(stepdownSource.token());
    state.createChangeStreamsMonitorAbortSource();
    ASSERT_FALSE(state.isEffectiveTokenCancelled());

    stepdownSource.cancel();
    ASSERT_TRUE(state.isSteppingDown());
    ASSERT_TRUE(state.isAbortedOrSteppingDown());
    ASSERT_TRUE(state.isEffectiveTokenCancelled());
}

TEST(ReshardingParticipantCancelStateTest, CancelChangeStreamsMonitorCancelsCSMToken) {
    ReshardingParticipantCancelState state;
    state.createChangeStreamsMonitorAbortSource();
    ASSERT_FALSE(state.isEffectiveTokenCancelled());

    state.cancelChangeStreamsMonitor();
    ASSERT_TRUE(state.isEffectiveTokenCancelled());

    // The abortOrStepdown token should not be cancelled.
    ASSERT_FALSE(state.isAbortedOrSteppingDown());
    ASSERT_FALSE(state.isSteppingDown());
}

DEATH_TEST(ReshardingParticipantCancelStateDeathTest,
           CancelChangeStreamsMonitorWithoutSourceTasserts,
           "No change streams monitor abort source was created") {
    ReshardingParticipantCancelState state;
    state.cancelChangeStreamsMonitor();
}

}  // namespace
}  // namespace mongo
