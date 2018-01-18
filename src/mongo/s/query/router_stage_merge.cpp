/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/query/router_stage_merge.h"

#include "mongo/db/query/find_common.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

RouterStageMerge::RouterStageMerge(OperationContext* opCtx,
                                   executor::TaskExecutor* executor,
                                   ClusterClientCursorParams* params)
    : RouterExecStage(opCtx), _executor(executor), _params(params), _arm(opCtx, executor, params) {}

StatusWith<ClusterQueryResult> RouterStageMerge::next(ExecContext execCtx) {
    // Non-tailable and tailable non-awaitData cursors always block until ready(). AwaitData
    // cursors wait for ready() only until a specified time limit is exceeded.
    return (_params->tailableMode == TailableMode::kTailableAndAwaitData
                ? awaitNextWithTimeout(execCtx)
                : blockForNextNoTimeout(execCtx));
}

StatusWith<ClusterQueryResult> RouterStageMerge::blockForNextNoTimeout(ExecContext execCtx) {
    invariant(_params->tailableMode != TailableMode::kTailableAndAwaitData);
    while (!_arm.ready()) {
        auto nextEventStatus = _arm.nextEvent();
        if (!nextEventStatus.isOK()) {
            return nextEventStatus.getStatus();
        }
        auto event = nextEventStatus.getValue();

        // Block until there are further results to return.
        _executor->waitForEvent(event);
    }

    return _arm.nextReady();
}

StatusWith<ClusterQueryResult> RouterStageMerge::awaitNextWithTimeout(ExecContext execCtx) {
    invariant(_params->tailableMode == TailableMode::kTailableAndAwaitData);
    // If we are in kInitialFind or kGetMoreWithAtLeastOneResultInBatch context and the ARM is not
    // ready, we don't block. Fall straight through to the return statement.
    while (!_arm.ready() && execCtx == ExecContext::kGetMoreNoResultsYet) {
        auto nextEventStatus = getNextEvent();
        if (!nextEventStatus.isOK()) {
            return nextEventStatus.getStatus();
        }
        auto event = nextEventStatus.getValue();

        // Block until there are further results to return, or our time limit is exceeded.
        auto waitStatus = _executor->waitForEvent(
            getOpCtx(), event, awaitDataState(getOpCtx()).waitForInsertsDeadline);

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

StatusWith<EventHandle> RouterStageMerge::getNextEvent() {
    // If we abandoned a previous event due to a mongoS-side timeout, wait for it first.
    if (_leftoverEventFromLastTimeout) {
        invariant(_params->tailableMode == TailableMode::kTailableAndAwaitData);
        auto event = _leftoverEventFromLastTimeout;
        _leftoverEventFromLastTimeout = EventHandle();
        return event;
    }

    return _arm.nextEvent();
}

void RouterStageMerge::kill(OperationContext* opCtx) {
    auto killEvent = _arm.kill(opCtx);
    if (!killEvent) {
        // Mongos is shutting down.
        return;
    }
    _executor->waitForEvent(killEvent);
}

bool RouterStageMerge::remotesExhausted() {
    return _arm.remotesExhausted();
}

Status RouterStageMerge::doSetAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    return _arm.setAwaitDataTimeout(awaitDataTimeout);
}

void RouterStageMerge::addNewShardCursors(
    std::vector<ClusterClientCursorParams::RemoteCursor>&& newShards) {
    _arm.addNewShardCursors(newShards);
    std::move(newShards.begin(), newShards.end(), std::back_inserter(_params->remotes));
}

}  // namespace mongo
