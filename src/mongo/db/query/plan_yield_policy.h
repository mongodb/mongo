/**
 *    Copyright (C) 2013 10gen Inc.
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

#pragma once

#include "mongo/db/catalog/collection.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

class ClockSource;
class RecordFetcher;

class PlanYieldPolicy {
public:
    /**
     * If policy == WRITE_CONFLICT_RETRY_ONLY, shouldYield will only return true after
     * forceYield has been called, and yield will only abandonSnapshot without releasing any
     * locks.
     */
    PlanYieldPolicy(PlanExecutor* exec, PlanExecutor::YieldPolicy policy);
    /**
     * Only used in dbtests since we don't have access to a PlanExecutor. Since we don't have
     * access to the PlanExecutor to grab a ClockSource from, we pass in a ClockSource directly
     * in the constructor instead.
     */
    PlanYieldPolicy(PlanExecutor::YieldPolicy policy, ClockSource* cs);

    /**
     * Used by YIELD_AUTO plan executors in order to check whether it is time to yield.
     * PlanExecutors give up their locks periodically in order to be fair to other
     * threads.
     */
    bool shouldYield();

    /**
     * Resets the yield timer so that we wait for a while before yielding again.
     */
    void resetTimer();

    /**
     * Used to cause a plan executor to give up locks and go to sleep. The PlanExecutor
     * must *not* be in saved state. Handles calls to save/restore state internally.
     *
     * If 'fetcher' is non-NULL, then we are yielding because the storage engine told us
     * that we will page fault on this record. We use 'fetcher' to retrieve the record
     * after we give up our locks.
     *
     * Returns true if the executor was restored successfully and is still alive. Returns false
     * if the executor got killed during yield.
     */
    bool yield(RecordFetcher* fetcher = NULL);

    /**
     * All calls to shouldYield will return true until the next call to yield.
     */
    void forceYield() {
        dassert(allowedToYield());
        _forceYield = true;
    }

    bool allowedToYield() const {
        return _policy != PlanExecutor::YIELD_MANUAL;
    }

    void setPolicy(PlanExecutor::YieldPolicy policy) {
        _policy = policy;
    }

private:
    PlanExecutor::YieldPolicy _policy;

    bool _forceYield;
    ElapsedTracker _elapsedTracker;

    // The plan executor which this yield policy is responsible for yielding. Must
    // not outlive the plan executor.
    PlanExecutor* const _planYielding;
};

}  // namespace mongo
