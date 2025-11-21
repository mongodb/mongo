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

#pragma once

#include "mongo/stdx/unordered_map.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/observable_mutex.h"

namespace mongo {

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

    RangeDeletionRecoveryTracker();

    void registerRecoveryJob(Term term);
    void notifyRecoveryJobComplete(Term term);
    void notifyEndOfTerm(Term term);
    SharedSemiFuture<Outcome> getRecoveryFuture(Term term);
    size_t getTrackedTermsCount() const;

private:
    mutable ObservableMutex<stdx::mutex> _mutex;

    boost::optional<Term> _highestEndedTerm;

    struct TermState {
        boost::optional<int8_t> remainingJobCount;
        SharedPromise<Outcome> recoveryCompletePromise;
    };
    std::map<Term, TermState> _termStates;

    TermState* getStateForTerm(WithLock, Term term);
    bool isTermTooOld(WithLock, Term term);
    bool isRemainingJobCountValid(const boost::optional<int8_t>& count);
    void cleanUpOldTerms(WithLock);
    void ensurePromiseSet(SharedPromise<Outcome>& promise, Outcome outcome);
};

}  // namespace mongo
