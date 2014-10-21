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
#include "mongo/db/global_environment_experiment.h"

namespace mongo {

    // Yield every 128 cycles or 10ms.  These values were inherited from v2.6, which just copied
    // them from v2.4.
    PlanYieldPolicy::PlanYieldPolicy(PlanExecutor* exec)
        : _elapsedTracker(128, 10),
          _planYielding(exec) { }

    bool PlanYieldPolicy::shouldYield() {
        return _elapsedTracker.intervalHasElapsed();
    }

    bool PlanYieldPolicy::yield(bool registerPlan) {
        // This is a no-op if document-level locking is supported. Doc-level locking systems
        // should not need to yield.
        if (supportsDocLocking()) {
            return true;
        }

        // No need to yield if the collection is NULL.
        if (NULL == _planYielding->collection()) {
            return true;
        }

        invariant(_planYielding);

        if (registerPlan) {
            _planYielding->registerExec();
        }

        OperationContext* opCtx = _planYielding->getOpCtx();
        invariant(opCtx);

        _planYielding->saveState();

        // Note that this call checks for interrupt, and thus can throw if interrupt flag is set.
        Yield::yieldAllLocks(opCtx, 1);
        _elapsedTracker.resetLastTime();

        if (registerPlan) {
            _planYielding->deregisterExec();
        }

        return _planYielding->restoreState(opCtx);
    }

} // namespace mongo
