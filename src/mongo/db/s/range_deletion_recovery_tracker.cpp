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

#include "mongo/db/s/range_deletion_recovery_tracker.h"

#include "mongo/logv2/log.h"
#include "mongo/util/observable_mutex_registry.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingRangeDeleter

namespace mongo {

using Outcome = RangeDeletionRecoveryTracker::Outcome;
using Term = RangeDeletionRecoveryTracker::Term;
using ActiveTerm = RangeDeletionRecoveryTracker::ActiveTerm;

ActiveTerm::ActiveTerm(RangeDeletionRecoveryTracker* parent, Term term)
    : _parent{parent}, _term{term} {}

ActiveTerm::~ActiveTerm() {
    _parent->notifyEndOfTerm(_term);
}

std::unique_ptr<ActiveTerm> RangeDeletionRecoveryTracker::notifyStartOfTerm(Term term) {
    return std::make_unique<ActiveTerm>(this, term);
}

RangeDeletionRecoveryTracker::RangeDeletionRecoveryTracker() {
    ObservableMutexRegistry::get().add("RangeDeletionRecoveryTracker::_mutex", _mutex);
}

void RangeDeletionRecoveryTracker::registerRecoveryJob(Term term) {
    stdx::lock_guard guard(_mutex);
    auto state = getStateForTerm(guard, term);
    if (!state) {
        return;
    }
    tassert(11420101,
            "Recovery job already completed",
            !state->recoveryCompletePromise.getFuture().isReady());
    auto& count = state->remainingJobCount;
    count = count.value_or(0) + 1;
}

void RangeDeletionRecoveryTracker::notifyRecoveryJobComplete(Term term) {
    stdx::lock_guard guard(_mutex);
    auto state = getStateForTerm(guard, term);
    if (!state) {
        return;
    }
    auto& [count, promise] = *state;
    if (!isRemainingJobCountValid(count)) {
        return;
    }
    (*count)--;
    if (*count == 0) {
        ensurePromiseSet(promise, Outcome::kComplete);
    }
}

void RangeDeletionRecoveryTracker::notifyEndOfTerm(Term term) {
    stdx::lock_guard guard(_mutex);
    if (_highestEndedTerm.has_value()) {
        _highestEndedTerm = std::max(*_highestEndedTerm, term);
    } else {
        _highestEndedTerm = term;
    }
    cleanUpOldTerms(guard);
}

SharedSemiFuture<Outcome> RangeDeletionRecoveryTracker::getRecoveryFuture(Term term) {
    stdx::lock_guard guard(_mutex);
    auto state = getStateForTerm(guard, term);
    if (!state) {
        return {Outcome::kUnknown};
    }
    return state->recoveryCompletePromise.getFuture();
}

size_t RangeDeletionRecoveryTracker::getTrackedTermsCount() const {
    stdx::lock_guard guard(_mutex);
    return _termStates.size();
}

RangeDeletionRecoveryTracker::TermState* RangeDeletionRecoveryTracker::getStateForTerm(
    WithLock lock, Term term) {
    auto it = _termStates.find(term);
    if (it == _termStates.end()) {
        if (isTermTooOld(lock, term)) {
            return nullptr;
        }
        it = _termStates.emplace_hint(
            it, std::piecewise_construct, std::forward_as_tuple(term), std::forward_as_tuple());
    }
    return &it->second;
}

bool RangeDeletionRecoveryTracker::isTermTooOld(WithLock, Term term) {
    return _highestEndedTerm.has_value() && *_highestEndedTerm >= term;
}

bool RangeDeletionRecoveryTracker::isRemainingJobCountValid(const boost::optional<int8_t>& count) {
    try {
        tassert(1079600,
                "More jobs notified as complete than registered as started",
                count.has_value() && *count > 0);
        return true;
    } catch (const AssertionException&) {
        return false;
    }
}

void RangeDeletionRecoveryTracker::cleanUpOldTerms(WithLock) {
    invariant(_highestEndedTerm.has_value());
    auto it = _termStates.begin();
    while (it != _termStates.end()) {
        auto& [term, state] = *it;
        if (term > *_highestEndedTerm) {
            break;
        }
        ensurePromiseSet(state.recoveryCompletePromise, Outcome::kIncomplete);
        it = _termStates.erase(it);
    }
}

void RangeDeletionRecoveryTracker::ensurePromiseSet(SharedPromise<Outcome>& promise,
                                                    Outcome outcome) {
    if (promise.getFuture().isReady()) {
        return;
    }
    promise.emplaceValue(outcome);
}

}  // namespace mongo
