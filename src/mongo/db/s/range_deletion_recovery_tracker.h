// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/unordered_map.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/observable_mutex.h"

#include <set>

namespace mongo {

enum class [[MONGO_MOD_PUBLIC]] RecoveryJob {
    kRangeDeleter,
    kLegacyMigration,
    kMoveRangeCoordinator,
};

class RangeDeletionRecoveryTracker {
public:
    using Term = long long;


    // Indicates whether all recovery jobs completed before the term ended.
    enum class Outcome {
        kComplete,    // All recovery jobs completed before the term ended.
        kIncomplete,  // At least one recovery job never completed before the term ended.
        kUnknown,     // This request was for an old term that had its state cleaned up before the
                      // request was made.
    };

    class ActiveTerm {
    public:
        ActiveTerm(RangeDeletionRecoveryTracker* parent, Term term);
        ActiveTerm(ActiveTerm&) = delete;
        ActiveTerm(ActiveTerm&&) = delete;
        ~ActiveTerm();

    private:
        RangeDeletionRecoveryTracker* _parent;
        Term _term;
    };

    RangeDeletionRecoveryTracker();

    [[nodiscard]] std::unique_ptr<ActiveTerm> notifyStartOfTerm(Term term);
    void registerRecoveryJob(Term term, RecoveryJob job);
    void notifyRecoveryJobComplete(Term term, RecoveryJob job);
    SharedSemiFuture<Outcome> getRecoveryFuture(Term term);
    size_t getTrackedTermsCount() const;

private:
    mutable ObservableMutex<std::mutex> _mutex;

    boost::optional<Term> _highestEndedTerm;

    struct TermState {
        std::set<RecoveryJob> recoveryJobs;
        SharedPromise<Outcome> recoveryCompletePromise;
    };
    std::map<Term, TermState> _termStates;

    TermState* getStateForTerm(WithLock, Term term);
    bool isTermTooOld(WithLock, Term term);
    void notifyEndOfTerm(Term term);
    void cleanUpOldTerms(WithLock);
    void ensurePromiseSet(SharedPromise<Outcome>& promise, Outcome outcome);
};

}  // namespace mongo
