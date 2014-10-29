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
#include "mongo/db/storage/record_fetcher.h"

namespace mongo {

    // Yield every 128 cycles or 10ms.  These values were inherited from v2.6, which just copied
    // them from v2.4.
    PlanYieldPolicy::PlanYieldPolicy(PlanExecutor* exec)
        : _elapsedTracker(128, 10),
          _planYielding(exec) { }

    bool PlanYieldPolicy::shouldYield() {
        invariant(!_planYielding->getOpCtx()->lockState()->inAWriteUnitOfWork());
        return _elapsedTracker.intervalHasElapsed();
    }

    bool PlanYieldPolicy::yield(RecordFetcher* fetcher) {
        invariant(_planYielding);

        OperationContext* opCtx = _planYielding->getOpCtx();
        invariant(opCtx);

        // All YIELD_AUTO plans will get here eventually when the elapsed tracker triggers that it's
        // time to yield. Whether or not we will actually yield (doc-level locking systems won't),
        // we need to check if this operation has been interrupted. Throws if the interrupt flag is
        // set.
        opCtx->checkForInterrupt();

        if (supportsDocLocking()) {
            // Doc-level locking is supported, so no need to release locks.
            return true;
        }

        // No need to yield if the collection is NULL.
        if (NULL == _planYielding->collection()) {
            return true;
        }

        _planYielding->saveState();

        // Release and reacquire locks.
        Yield::yieldAllLocks(opCtx, 1, fetcher);

        _elapsedTracker.resetLastTime();

        return _planYielding->restoreState(opCtx);
    }

} // namespace mongo
