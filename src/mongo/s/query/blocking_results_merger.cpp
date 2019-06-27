/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/find_common.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/s/query/blocking_results_merger.h"

namespace mongo {

BlockingResultsMerger::BlockingResultsMerger(OperationContext* opCtx,
                                             AsyncResultsMergerParams&& armParams,
                                             std::shared_ptr<executor::TaskExecutor> executor,
                                             std::unique_ptr<ResourceYielder> resourceYielder)
    : _tailableMode(armParams.getTailableMode().value_or(TailableModeEnum::kNormal)),
      _executor(executor),
      _arm(opCtx, std::move(executor), std::move(armParams)),
      _resourceYielder(std::move(resourceYielder)) {}

StatusWith<stdx::cv_status> BlockingResultsMerger::doWaiting(
    OperationContext* opCtx, const std::function<StatusWith<stdx::cv_status>()>& waitFn) noexcept {

    if (_resourceYielder) {
        try {
            // The BRM interface returns Statuses. Be sure we respect that here.
            _resourceYielder->yield(opCtx);
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    boost::optional<StatusWith<stdx::cv_status>> result;
    try {
        // This shouldn't throw, but we cannot enforce that.
        result = waitFn();
    } catch (const DBException&) {
        MONGO_UNREACHABLE;
    }

    if (_resourceYielder) {
        try {
            _resourceYielder->unyield(opCtx);
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    return *result;
}

StatusWith<ClusterQueryResult> BlockingResultsMerger::awaitNextWithTimeout(
    OperationContext* opCtx, RouterExecStage::ExecContext execCtx) {
    invariant(_tailableMode == TailableModeEnum::kTailableAndAwaitData);
    // If we are in kInitialFind or kGetMoreWithAtLeastOneResultInBatch context and the ARM is not
    // ready, we don't block. Fall straight through to the return statement.
    while (!_arm.ready() && execCtx == RouterExecStage::ExecContext::kGetMoreNoResultsYet) {
        auto nextEventStatus = getNextEvent();
        if (!nextEventStatus.isOK()) {
            return nextEventStatus.getStatus();
        }
        auto event = nextEventStatus.getValue();

        const auto waitStatus = doWaiting(opCtx, [this, opCtx, &event]() {
            return _executor->waitForEvent(
                opCtx, event, awaitDataState(opCtx).waitForInsertsDeadline);
        });

        if (!waitStatus.isOK()) {
            return waitStatus.getStatus();
        }
        // Swallow timeout errors for tailable awaitData cursors, stash the event that we were
        // waiting on, and return EOF.
        if (waitStatus == stdx::cv_status::timeout) {
            _leftoverEventFromLastTimeout = std::move(event);
            return ClusterQueryResult{};
        }
    }

    // We reach this point either if the ARM is ready, or if the ARM is !ready and we are in
    // kInitialFind or kGetMoreWithAtLeastOneResultInBatch ExecContext. In the latter case, we
    // return EOF immediately rather than blocking for further results.
    return _arm.ready() ? _arm.nextReady() : ClusterQueryResult{};
}

StatusWith<ClusterQueryResult> BlockingResultsMerger::blockUntilNext(OperationContext* opCtx) {
    while (!_arm.ready()) {
        auto nextEventStatus = _arm.nextEvent();
        if (!nextEventStatus.isOK()) {
            return nextEventStatus.getStatus();
        }
        auto event = nextEventStatus.getValue();

        // Block until there are further results to return.
        auto status = doWaiting(
            opCtx, [this, opCtx, &event]() { return _executor->waitForEvent(opCtx, event); });

        if (!status.isOK()) {
            return status.getStatus();
        }

        // We have not provided a deadline, so if the wait returns without interruption, we do not
        // expect to have timed out.
        invariant(status.getValue() == stdx::cv_status::no_timeout);
    }

    return _arm.nextReady();
}
StatusWith<ClusterQueryResult> BlockingResultsMerger::next(OperationContext* opCtx,
                                                           RouterExecStage::ExecContext execCtx) {
    // Non-tailable and tailable non-awaitData cursors always block until ready(). AwaitData
    // cursors wait for ready() only until a specified time limit is exceeded.
    return (_tailableMode == TailableModeEnum::kTailableAndAwaitData
                ? awaitNextWithTimeout(opCtx, execCtx)
                : blockUntilNext(opCtx));
}

StatusWith<executor::TaskExecutor::EventHandle> BlockingResultsMerger::getNextEvent() {
    // If we abandoned a previous event due to a mongoS-side timeout, wait for it first.
    if (_leftoverEventFromLastTimeout) {
        invariant(_tailableMode == TailableModeEnum::kTailableAndAwaitData);
        // If we have an outstanding event from last time, then we might have to manually schedule
        // some getMores for the cursors. If a remote response came back while we were between
        // getMores (from the user to mongos), the response may have been an empty batch, and the
        // ARM would not be able to ask for the next batch immediately since it is not attached to
        // an OperationContext. Now that we have a valid OperationContext, we schedule the getMores
        // ourselves.
        Status getMoreStatus = _arm.scheduleGetMores();
        if (!getMoreStatus.isOK()) {
            return getMoreStatus;
        }

        // Return the leftover event and clear '_leftoverEventFromLastTimeout'.
        auto event = _leftoverEventFromLastTimeout;
        _leftoverEventFromLastTimeout = executor::TaskExecutor::EventHandle();
        return event;
    }

    return _arm.nextEvent();
}

void BlockingResultsMerger::kill(OperationContext* opCtx) {
    auto killEvent = _arm.kill(opCtx);
    if (!killEvent) {
        // We are shutting down.
        return;
    }
    _executor->waitForEvent(killEvent);
}

}  // namespace mongo
