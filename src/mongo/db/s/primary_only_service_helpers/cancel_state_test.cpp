// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/primary_only_service_helpers/cancel_state.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace primary_only_service_helpers {
namespace {

class CancelStateTest : public unittest::Test {
protected:
    CancelStateTest() : _stepdownToken{_stepdownSource.token()}, _cancelState{_stepdownToken} {}

    CancelState& getCancelState() {
        return _cancelState;
    }

    void stepdown() {
        _stepdownSource.cancel();
    }

private:
    CancellationSource _stepdownSource;
    CancellationToken _stepdownToken;
    CancelState _cancelState;
};

TEST_F(CancelStateTest, NotSteppingDown) {
    ASSERT_FALSE(getCancelState().isSteppingDown());
}

TEST_F(CancelStateTest, IsSteppingDownIndicatesStepdown) {
    stepdown();
    ASSERT_TRUE(getCancelState().isSteppingDown());
}

TEST_F(CancelStateTest, IsSteppingDownUnaffectedByAbort) {
    getCancelState().abort();
    ASSERT_FALSE(getCancelState().isSteppingDown());
}

TEST_F(CancelStateTest, NotAbortedOrSteppingDown) {
    ASSERT_FALSE(getCancelState().isAbortedOrSteppingDown());
}

TEST_F(CancelStateTest, IsAbortedOrSteppingDownIndicatesAbort) {
    getCancelState().abort();
    ASSERT_TRUE(getCancelState().isAbortedOrSteppingDown());
}

TEST_F(CancelStateTest, IsAbortedOrSteppingDownIndicatesStepdown) {
    stepdown();
    ASSERT_TRUE(getCancelState().isAbortedOrSteppingDown());
}

class CancelStateDetachedTest : public unittest::Test {
protected:
    CancelState& getCancelState() {
        return _cancelState;
    }

    void stepdown() {
        _stepdownSource.cancel();
    }

    void attach() {
        _cancelState.attachStepdownToken(_stepdownSource.token());
    }

private:
    CancellationSource _stepdownSource;
    CancelState _cancelState;
};

TEST_F(CancelStateDetachedTest, NotSteppingDownWithoutToken) {
    ASSERT_FALSE(getCancelState().isSteppingDown());
}

TEST_F(CancelStateDetachedTest, NotAbortedOrSteppingDownWithoutToken) {
    ASSERT_FALSE(getCancelState().isAbortedOrSteppingDown());
}

TEST_F(CancelStateDetachedTest, AbortWorksWithoutToken) {
    getCancelState().abort();
    ASSERT_TRUE(getCancelState().isAbortedOrSteppingDown());
    ASSERT_FALSE(getCancelState().isSteppingDown());
}

TEST_F(CancelStateDetachedTest, StepdownBeforeAttach) {
    auto token = getCancelState().getStepdownToken();
    ASSERT_FALSE(token.isCanceled());

    stepdown();
    ASSERT_FALSE(token.isCanceled());

    attach();
    ASSERT_TRUE(token.isCanceled());
    ASSERT_TRUE(getCancelState().isSteppingDown());
    ASSERT_TRUE(getCancelState().isAbortedOrSteppingDown());
}

TEST_F(CancelStateDetachedTest, StepdownAfterAttach) {
    auto token = getCancelState().getStepdownToken();
    ASSERT_FALSE(token.isCanceled());

    attach();
    ASSERT_FALSE(token.isCanceled());

    stepdown();
    ASSERT_TRUE(token.isCanceled());
    ASSERT_TRUE(getCancelState().isSteppingDown());
    ASSERT_TRUE(getCancelState().isAbortedOrSteppingDown());
}

}  // namespace
}  // namespace primary_only_service_helpers
}  // namespace mongo
