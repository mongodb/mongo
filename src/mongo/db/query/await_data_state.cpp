// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/await_data_state.h"

#include <functional>
#include <mutex>
#include <utility>

namespace mongo {
namespace {
// Written by compression workers, unlike AwaitDataState which only the operation thread touches.
// 'ready' is atomic so the check in shouldWaitForInserts() doesn't lock; 'mutex' orders it against
// waker registration.
struct PipelineWakeState {
    std::mutex mutex;
    AtomicWord<bool> ready{false};
    AtomicWord<bool> eligible{false};
    std::function<void()> waker;
};

const OperationContext::Decoration<PipelineWakeState> pipelineWakeState =
    OperationContext::declareDecoration<PipelineWakeState>();
}  // namespace

const OperationContext::Decoration<AwaitDataState> awaitDataState =
    OperationContext::declareDecoration<AwaitDataState>();

void setAwaitDataDeadline(OperationContext* opCtx, Date_t deadline) {
    awaitDataState(opCtx).waitForInsertsDeadline = deadline;
}

bool awaitDataPipelineReadyToSend(OperationContext* opCtx) {
    return pipelineWakeState(opCtx).ready.load();
}

void setAwaitDataPipelineWakeEligible(OperationContext* opCtx) {
    pipelineWakeState(opCtx).eligible.store(true);
}

bool awaitDataPipelineWakeEligible(OperationContext* opCtx) {
    return pipelineWakeState(opCtx).eligible.load();
}

void setAwaitDataPipelineWaker(OperationContext* opCtx, std::function<void()> waker) {
    auto& state = pipelineWakeState(opCtx);
    std::function<void()> previous;
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        previous = std::exchange(state.waker, std::move(waker));
    }
    // 'previous' is destroyed here, outside the lock.
}

void signalAwaitDataPipelineReady(OperationContext* opCtx) {
    auto& state = pipelineWakeState(opCtx);
    std::function<void()> waker;
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.ready.store(true);
        waker = state.waker;
    }
    // Call outside the lock. The copy owns what it uses, so a concurrent clear is fine.
    if (waker) {
        waker();
    }
}

}  // namespace mongo
