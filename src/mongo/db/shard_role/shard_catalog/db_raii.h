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

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/modules.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/time_support.h"

#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/container/flat_set.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * RAII-style class which can update the diagnostic state on the operation's CurOp object and record
 * the operation via Top upon destruction. Can be configured to only update the Top counters if
 * desired.
 */
class MONGO_MOD_PUBLIC AutoStatsTracker {
    AutoStatsTracker(const AutoStatsTracker&) = delete;
    AutoStatsTracker& operator=(const AutoStatsTracker&) = delete;

public:
    /**
     * Describes which diagnostics to update during the lifetime of this object.
     */
    enum class LogMode {
        kUpdateTop,    // Increments the Top counter for this operation type and this namespace
                       // upon destruction.
        kUpdateCurOp,  // Adjusts the state on the CurOp object associated with the
                       // OperationContext. Updates the namespace to be 'nss', starts a timer
                       // for the operation (if it hasn't already started), and figures out and
                       // records the profiling level of the operation.
        kUpdateTopAndCurOp,  // Performs the operations of both the LogModes specified above.
    };

    /**
     * If 'logMode' is 'kUpdateCurOp' or 'kUpdateTopAndCurOp', sets up and records state on the
     * CurOp object attached to 'opCtx', as described above.
     */
    AutoStatsTracker(OperationContext* opCtx,
                     const NamespaceString& nss,
                     Top::LockType lockType,
                     LogMode logMode,
                     int dbProfilingLevel,
                     Date_t deadline = Date_t::max(),
                     boost::optional<std::vector<NamespaceStringOrUUID>::const_iterator>
                         secondaryNssVectorBegin = boost::none,
                     boost::optional<std::vector<NamespaceStringOrUUID>::const_iterator>
                         secondaryNssVectorEnd = boost::none);

    /**
     * Records stats about the current operation via Top, if 'logMode' is 'kUpdateTop' or
     * 'kUpdateTopAndCurOp'.
     */
    ~AutoStatsTracker();

private:
    OperationContext* _opCtx;
    Top::LockType _lockType;
    const LogMode _logMode;
    boost::container::flat_set<NamespaceString,
                               std::less<NamespaceString>,
                               absl::InlinedVector<NamespaceString, 1>>
        _nssSet;
};

/**
 * Acquires the global MODE_IS lock and establishes a consistent CollectionCatalog and storage
 * snapshot.
 */
class MONGO_MOD_NEEDS_REPLACEMENT AutoReadLockFree {
public:
    AutoReadLockFree(OperationContext* opCtx, Date_t deadline = Date_t::max());

private:
    // Sets a flag on the opCtx to inform subsequent code that the operation is running lock-free.
    LockFreeReadsBlock _lockFreeReadsBlock;

    Lock::GlobalLock _globalLock;
};

/**
 * Establishes a consistent CollectionCatalog with a storage snapshot. Also verifies Database
 * sharding state for the provided Db. Takes MODE_IS global lock.
 *
 * Does not take readConcern into account. Any Collection returned by the stashed catalog will not
 * refresh the storage snapshot on yield.
 *
 * Should only be used to read catalog metadata for a particular Db and not for reading from
 * Collection(s).
 */
class MONGO_MOD_PRIVATE AutoGetDbForReadLockFree {
public:
    AutoGetDbForReadLockFree(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             Date_t deadline = Date_t::max());

private:
    // Sets a flag on the opCtx to inform subsequent code that the operation is running lock-free.
    LockFreeReadsBlock _lockFreeReadsBlock;

    Lock::GlobalLock _globalLock;
};

/**
 * Creates either an AutoGetDb or AutoGetDbForReadLockFree depending on whether a lock-free read is
 * supported in the situation per the results of supportsLockFreeRead().
 */
class MONGO_MOD_NEEDS_REPLACEMENT AutoGetDbForReadMaybeLockFree {
public:
    AutoGetDbForReadMaybeLockFree(OperationContext* opCtx,
                                  const DatabaseName& dbName,
                                  Date_t deadline = Date_t::max());

private:
    boost::optional<AutoGetDb> _autoGet;
    boost::optional<AutoGetDbForReadLockFree> _autoGetLockFree;
};

/**
 * When in scope, enforces prepare conflicts in the storage engine. Reads and writes in this scope
 * will block on accessing an already updated document which is in prepared state. And they will
 * unblock after the prepared transaction that performed the update commits/aborts.
 */
class MONGO_MOD_PUBLIC EnforcePrepareConflictsBlock {
public:
    explicit EnforcePrepareConflictsBlock(OperationContext* opCtx)
        : _opCtx(opCtx),
          _originalValue(shard_role_details::getRecoveryUnit(opCtx)->getPrepareConflictBehavior()) {
        // It is illegal to call setPrepareConflictBehavior() while any storage transaction is
        // active. setPrepareConflictBehavior() invariants that there is no active storage
        // transaction.
        shard_role_details::getRecoveryUnit(_opCtx)->setPrepareConflictBehavior(
            PrepareConflictBehavior::kEnforce);
    }

    ~EnforcePrepareConflictsBlock() {
        // If we are still holding locks, we might still have open storage transactions. However, we
        // did not start with any active transactions when we first entered the scope. And
        // transactions started within this scope cannot be reused outside of the scope. So we need
        // to call abandonSnapshot() to close any open transactions on destruction. Any reads or
        // writes should have already completed as we are exiting the scope. Therefore, this call is
        // safe.
        if (shard_role_details::getLocker(_opCtx)->isLocked()) {
            shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        }
        // It is illegal to call setPrepareConflictBehavior() while any storage transaction is
        // active. There should not be any active transaction if we are not holding locks. If locks
        // are still being held, the above abandonSnapshot() call should have already closed all
        // storage transactions.
        shard_role_details::getRecoveryUnit(_opCtx)->setPrepareConflictBehavior(_originalValue);
    }

private:
    OperationContext* _opCtx;
    PrepareConflictBehavior _originalValue;
};

}  // namespace mongo
