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

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_yield_policy.h"

#include "mongo/db/concurrency/yield.h"

namespace mongo {

    // Yield every 128 cycles or 10ms.  These values were inherited from v2.6, which just copied
    // them from v2.4.
    PlanYieldPolicy::PlanYieldPolicy()
        : _elapsedTracker(128, 10),
          _planYielding(NULL) { }

    PlanYieldPolicy::~PlanYieldPolicy() {
        if (NULL != _planYielding) {
            // We were destructed mid-yield.  Since we're being used to yield a runner, we have
            // to deregister the runner.
            if (_planYielding->collection()) {
                _planYielding->collection()->cursorCache()->deregisterExecutor(_planYielding);
            }
        }
    }

    /**
     * Yield the provided runner, registering and deregistering it appropriately.  Deal with
     * deletion during a yield by setting _runnerYielding to ensure deregistration.
     *
     * Provided runner MUST be YIELD_MANUAL.
     */
    bool PlanYieldPolicy::yieldAndCheckIfOK(PlanExecutor* plan) {
        invariant(plan);
        invariant(plan->collection());

        // If micros > 0, we should yield.
        plan->saveState();

        // If we're destructed during yield this will be used to deregister ourselves.
        // This happens when we're not in a ClientCursor and somebody kills all cursors
        // on the ns we're operating on.
        _planYielding = plan;

        // Register with the thing that may kill() the 'plan'.
        plan->collection()->cursorCache()->registerExecutor(plan);

        // Note that this call checks for interrupt, and thus can throw if interrupt flag is set.
        Yield::yieldAllLocks(plan->getOpCtx(), 1);

        // If the plan was killed, runner->collection() will return NULL, and we can't/don't
        // deregister it.
        if (plan->collection()) {
            plan->collection()->cursorCache()->deregisterExecutor(plan);
        }

        _planYielding = NULL;

        _elapsedTracker.resetLastTime();

        return plan->restoreState(plan->getOpCtx());
    }

} // namespace mongo

