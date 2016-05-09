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

#include "mongo/platform/basic.h"

#include "mongo/db/query/query_yield.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {
MONGO_FP_DECLARE(setYieldAllLocksHang);
MONGO_FP_DECLARE(setYieldAllLocksWait);
}  // namespace

// static
void QueryYield::yieldAllLocks(OperationContext* txn,
                               RecordFetcher* fetcher,
                               const std::string& planExecNS) {
    // Things have to happen here in a specific order:
    //   1) Tell the RecordFetcher to do any setup which needs to happen inside locks
    //   2) Release lock mgr locks
    //   3) Go to sleep
    //   4) Touch the record we're yielding on, if there is one (RecordFetcher::fetch)
    //   5) Reacquire lock mgr locks

    Locker* locker = txn->lockState();

    Locker::LockSnapshot snapshot;

    if (fetcher) {
        fetcher->setup();
    }

    // Nothing was unlocked, just return, yielding is pointless.
    if (!locker->saveLockStateAndUnlock(&snapshot)) {
        return;
    }

    // Top-level locks are freed, release any potential low-level (storage engine-specific
    // locks). If we are yielding, we are at a safe place to do so.
    txn->recoveryUnit()->abandonSnapshot();

    MONGO_FAIL_POINT_PAUSE_WHILE_SET(setYieldAllLocksHang);

    MONGO_FAIL_POINT_BLOCK(setYieldAllLocksWait, customWait) {
        const BSONObj& data = customWait.getData();
        BSONElement customWaitNS = data["namespace"];
        if (!customWaitNS || planExecNS == customWaitNS.str()) {
            sleepFor(Milliseconds(data["waitForMillis"].numberInt()));
        }
    }

    // Track the number of yields in CurOp.
    CurOp::get(txn)->yielded();

    if (fetcher) {
        fetcher->fetch();
    }

    locker->restoreLockState(snapshot);
}

}  // namespace mongo
