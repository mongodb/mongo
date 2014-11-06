/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/query/query_yield.h"

#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_fetcher.h"

namespace mongo {
namespace {

    void yieldOrSleepFor1Microsecond() {
#ifdef _WIN32
        SwitchToThread();
#elif defined(__linux__)
        pthread_yield();
#else
        sleepmicros(1);
#endif
    }

}

    // static
    void QueryYield::yieldAllLocks(OperationContext* txn, int micros, RecordFetcher* fetcher) {
        // Things have to happen here in a specific order:
        //   1) Tell the RecordFetcher to do any setup which needs to happen inside locks
        //   2) Release lock mgr locks
        //   3) Go to sleep
        //   4) Touch the record we're yielding on, if there is one (RecordFetcher::fetch)
        //   5) Reacquire lock mgr locks

        Locker* locker = txn->lockState();

        // If we had the read lock, we yield extra hard so that we don't starve writers.
        bool hadReadLock = locker->hasAnyReadLock();

        Locker::LockSnapshot snapshot;

        if (fetcher) {
            fetcher->setup();
        }

        // Nothing was unlocked, just return, yielding is pointless.
        if (!locker->saveLockStateAndUnlock(&snapshot)) {
            return;
        }

        // Track the number of yields in CurOp.
        txn->getCurOp()->yielded();

        if (hadReadLock) {
            // TODO(kal): Is this still relevant?  Probably not?
            //
            // Quote: This sleep helps reader threads yield to writer threads.  Without this, the underlying
            // reader/writer lock implementations are not sufficiently writer-greedy.

#ifdef _WIN32
            SwitchToThread();
#else
            if (0 == micros) {
                yieldOrSleepFor1Microsecond();
            }
            else {
                sleepmicros(1);
            }
#endif
        }
        else {
            if (-1 == micros) {
                sleepmicros(1);
                //sleepmicros(Client::recommendedYieldMicros());
            }
            else if (0 == micros) {
                yieldOrSleepFor1Microsecond();
            }
            else if (micros > 0) {
                sleepmicros(micros);
            }
        }

        if (fetcher) {
            fetcher->fetch();
        }

        locker->restoreLockState(snapshot);
    }

} // namespace mongo
