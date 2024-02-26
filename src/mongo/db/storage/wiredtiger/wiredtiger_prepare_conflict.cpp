/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <mutex>
#include <string>
#include <vector>

#include <wiredtiger.h>

#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/lock_stats.h"
#include "mongo/db/curop.h"
#include "mongo/db/prepare_conflict_tracker.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {
std::once_flag logPrepareWithTimestampOnce;
}

MONGO_FAIL_POINT_DEFINE(WTSkipPrepareConflictRetries);

MONGO_FAIL_POINT_DEFINE(WTPrintPrepareConflictLog);

void wiredTigerPrepareConflictLog(int attempts) {
    LOGV2_DEBUG(22379,
                1,
                "Caught WT_PREPARE_CONFLICT, attempt {attempts}. Waiting for unit of work to "
                "commit or abort.",
                "attempts"_attr = attempts);
}

void wiredTigerPrepareConflictFailPointLog() {
    LOGV2(22380, "WTPrintPrepareConflictLog fail point enabled.");
}

void wiredTigerPrepareConflictOplogResourceLog() {
    std::call_once(logPrepareWithTimestampOnce, [] {
        LOGV2(5739901, "Hit a prepare conflict while holding a resource on the oplog");
        printStackTrace();
    });
}

int wiredTigerPrepareConflictRetrySlow(OperationContext* opCtx, std::function<int()> func) {
    if (shard_role_details::getRecoveryUnit(opCtx)->isTimestamped()) {
        // This transaction is holding a resource in the form of an oplog slot. Committed
        // transactions that get a later oplog slot will be unable to replicate until this resource
        // is released (in the form of this transaction committing or aborting). For this case, we
        // choose to abort our transaction and retry instead of blocking. It's possible that we can
        // be blocking on a prepared update that requires replication to make progress, creating a
        // stall in the MDB cluster.
        wiredTigerPrepareConflictOplogResourceLog();
        throwWriteConflictException("Holding a resource (oplog slot).");
    }

    auto recoveryUnit = WiredTigerRecoveryUnit::get(opCtx);
    int attempts = 1;
    // If we return from this function, we have either returned successfully or we've returned an
    // error other than WT_PREPARE_CONFLICT. Reset PrepareConflictTracker accordingly.
    ON_BLOCK_EXIT([opCtx] { PrepareConflictTracker::get(opCtx).endPrepareConflict(opCtx); });
    PrepareConflictTracker::get(opCtx).beginPrepareConflict(opCtx);

    auto client = opCtx->getClient();
    if (client->isFromSystemConnection()) {
        // System (internal) connections that hit a prepare conflict should be killable to prevent
        // deadlocks with prepared transactions on replica set step up and step down.
        stdx::lock_guard<Client> lk(*client);
        invariant(client->canKillSystemOperationInStepdown(lk));
    }

    // It is contradictory to be running into a prepare conflict when we are ignoring interruptions,
    // particularly when running code inside an
    // OperationContext::runWithoutInterruptionExceptAtGlobalShutdown block. Operations executed in
    // this way are expected to be set to ignore prepare conflicts.
    invariant(!opCtx->isIgnoringInterrupts());

    if (MONGO_unlikely(WTPrintPrepareConflictLog.shouldFail())) {
        wiredTigerPrepareConflictFailPointLog();
    }

    CurOp::get(opCtx)->debug().additiveMetrics.incrementPrepareReadConflicts(1);
    wiredTigerPrepareConflictLog(attempts);

    const auto lockerInfo = shard_role_details::getLocker(opCtx)->getLockerInfo(boost::none);
    for (const auto& lock : lockerInfo.locks) {
        const auto type = lock.resourceId.getType();
        // If a user operation on secondaries acquires a lock in MODE_S and then blocks on a prepare
        // conflict with a prepared transaction, deadlock will occur at the commit time of the
        // prepared transaction when it attempts to reacquire (since locks were yielded on
        // secondaries) an IX lock that conflicts with the MODE_S lock held by the user operation.
        // User operations that acquire MODE_X locks and block on prepare conflicts could lead to
        // the same problem. However, user operations on secondaries should never hold MODE_X locks.
        // Since prepared transactions will not reacquire RESOURCE_MUTEX / RESOURCE_METADATA /
        // RESOURCE_DDL_* locks at commit time, these lock types are safe. Therefore, invariant here
        // that we do not get a prepare conflict while holding a global, database, or collection
        // MODE_S lock (or MODE_X lock for completeness).
        if (type == RESOURCE_GLOBAL || type == RESOURCE_DATABASE || type == RESOURCE_COLLECTION)
            invariant(lock.mode != MODE_S && lock.mode != MODE_X,
                      str::stream() << lock.resourceId.toString() << " in " << modeName(lock.mode));
    }

    if (MONGO_unlikely(WTSkipPrepareConflictRetries.shouldFail())) {
        // Callers of wiredTigerPrepareConflictRetry() should eventually call wtRCToStatus() via
        // invariantWTOK() and have the WT_ROLLBACK error bubble up as a WriteConflictException.
        // Enabling the "skipWriteConflictRetries" failpoint in conjunction with the
        // "WTSkipPrepareConflictRetries" failpoint prevents the higher layers from retrying the
        // entire operation.
        return WT_ROLLBACK;
    }

    while (true) {
        attempts++;
        auto lastCount = recoveryUnit->getSessionCache()->getPrepareCommitOrAbortCount();
        int ret = WT_READ_CHECK(func());

        if (ret != WT_PREPARE_CONFLICT)
            return ret;

        CurOp::get(opCtx)->debug().additiveMetrics.incrementPrepareReadConflicts(1);
        wiredTigerPrepareConflictLog(attempts);

        // Wait on the session cache to signal that a unit of work has been committed or aborted.
        recoveryUnit->getSessionCache()->waitUntilPreparedUnitOfWorkCommitsOrAborts(*opCtx,
                                                                                    lastCount);
    }
}


}  // namespace mongo
