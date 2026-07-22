// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/range_deletion_recovery_tracker.h"

#include "mongo/logv2/log.h"
#include "mongo/util/observable_mutex_registry.h"

#include <algorithm>
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
    ObservableMutexRegistry::get().add("rangeDeletionRecoveryTrackerMutex", _mutex);
}

void RangeDeletionRecoveryTracker::registerRecoveryJob(Term term, RecoveryJob job) {
    std::lock_guard guard(_mutex);
    auto state = getStateForTerm(guard, term);
    if (!state) {
        return;
    }
    tassert(11420101,
            "Recovery job already completed",
            !state->recoveryCompletePromise.getFuture().isReady());
    tassert(11420102, "Recovery job already registered", state->recoveryJobs.insert(job).second);
}

void RangeDeletionRecoveryTracker::notifyRecoveryJobComplete(Term term, RecoveryJob job) {
    std::lock_guard guard(_mutex);
    auto state = getStateForTerm(guard, term);
    if (!state) {
        return;
    }

    if (state->recoveryJobs.erase(job) && state->recoveryJobs.empty()) {
        ensurePromiseSet(state->recoveryCompletePromise, Outcome::kComplete);
    }
}

void RangeDeletionRecoveryTracker::notifyEndOfTerm(Term term) {
    std::lock_guard guard(_mutex);
    if (_highestEndedTerm.has_value()) {
        _highestEndedTerm = std::max(*_highestEndedTerm, term);
    } else {
        _highestEndedTerm = term;
    }
    cleanUpOldTerms(guard);
}

SharedSemiFuture<Outcome> RangeDeletionRecoveryTracker::getRecoveryFuture(Term term) {
    std::lock_guard guard(_mutex);
    auto state = getStateForTerm(guard, term);
    if (!state) {
        return {Outcome::kUnknown};
    }
    return state->recoveryCompletePromise.getFuture();
}

size_t RangeDeletionRecoveryTracker::getTrackedTermsCount() const {
    std::lock_guard guard(_mutex);
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
