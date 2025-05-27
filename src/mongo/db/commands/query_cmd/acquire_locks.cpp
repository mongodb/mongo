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

#include "mongo/db/commands/query_cmd/acquire_locks.h"

#include "mongo/db/profile_settings.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {
void applyCursorReadConcern(OperationContext* opCtx, repl::ReadConcernArgs rcArgs) {
    const auto isReplSet = repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet();

    // Select the appropriate read source. If we are in a transaction with read concern majority,
    // this will already be set to kNoTimestamp, so don't set it again.
    if (isReplSet && rcArgs.getLevel() == repl::ReadConcernLevel::kMajorityReadConcern &&
        !opCtx->inMultiDocumentTransaction()) {
        switch (rcArgs.getMajorityReadMechanism()) {
            case repl::ReadConcernArgs::MajorityReadMechanism::kMajoritySnapshot: {
                // Make sure we read from the majority snapshot.
                shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
                    RecoveryUnit::ReadSource::kMajorityCommitted);
                uassertStatusOK(shard_role_details::getRecoveryUnit(opCtx)
                                    ->majorityCommittedSnapshotAvailable());
                break;
            }
            case repl::ReadConcernArgs::MajorityReadMechanism::kSpeculative: {
                // Mark the operation as speculative and select the correct read source.
                repl::SpeculativeMajorityReadInfo::get(opCtx).setIsSpeculativeRead();
                shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
                    RecoveryUnit::ReadSource::kNoOverlap);
                break;
            }
        }
    }

    if (isReplSet && rcArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern &&
        !opCtx->inMultiDocumentTransaction()) {
        auto atClusterTime = rcArgs.getArgsAtClusterTime();
        invariant(atClusterTime && *atClusterTime != LogicalTime::kUninitialized);
        shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
            RecoveryUnit::ReadSource::kProvided, atClusterTime->asTimestamp());
    }

    // For cursor commands that take locks internally, the read concern on the
    // OperationContext may affect the timestamp read source selected by the storage engine.
    // We place the cursor read concern onto the OperationContext so the lock acquisition
    // respects the cursor's read concern.
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        repl::ReadConcernArgs::get(opCtx) = rcArgs;
    }
}
}  // namespace

void applyConcernsAndReadPreference(OperationContext* opCtx, const ClientCursor& cursor) {
    applyCursorReadConcern(opCtx, cursor.getReadConcernArgs());
    opCtx->setWriteConcern(cursor.getWriteConcernOptions());
    ReadPreferenceSetting::get(opCtx) = cursor.getReadPreferenceSetting();
}

CursorLocks::CursorLocks(OperationContext* opCtx,
                         const NamespaceString& nss,
                         ClientCursorPin& cursorPin) {
    extDataSourceScopeGuard = ExternalDataSourceScopeGuard::get(cursorPin.getCursor());

    // Update opCtx of the decorated ExternalDataSourceScopeGuard object so that it can drop
    // virtual collections in the new 'opCtx'.
    ExternalDataSourceScopeGuard::updateOperationContext(cursorPin.getCursor(), opCtx);

    ScopeGuard cursorDeleter([&] {
        cursorPin.deleteUnderlying();
        if (txnResourcesHandler) {
            txnResourcesHandler->dismissRestoredResources();
        }
    });

    // Cursors come in one of two flavors:
    //
    // - Cursors which read from a single collection, such as those generated via the
    //   find command. For these cursors, we hold the appropriate collection lock for the
    //   duration of the getMore using AutoGetCollectionForRead. These cursors have the
    //   'kLockExternally' lock policy.
    //
    // - Cursors which may read from many collections, e.g. those generated via the
    //   aggregate command, or which do not read from a collection at all, e.g. those
    //   generated by the listIndexes command. We don't need to acquire locks to use these
    //   cursors, since they either manage locking themselves or don't access data protected
    //   by collection locks. These cursors have the 'kLocksInternally' lock policy.
    //
    // While we only need to acquire locks for 'kLockExternally' cursors, we need to create
    // an AutoStatsTracker in either case. This is responsible for updating statistics in
    // CurOp and Top. We avoid using AutoGetCollectionForReadCommand because we may need to
    // drop and reacquire locks when the cursor is awaitData, but we don't want to update
    // the stats twice.
    if (cursorPin->getExecutor()->lockPolicy() == PlanExecutor::LockPolicy::kLocksInternally) {
        // Profile whole-db/cluster change stream getMore commands.
        if (!nss.isCollectionlessCursorNamespace() ||
            CurOp::get(opCtx)->debug().isChangeStreamQuery) {
            statsTracker.emplace(opCtx,
                                 nss,
                                 Top::LockType::NotLocked,
                                 AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                 DatabaseProfileSettings::get(opCtx->getServiceContext())
                                     .getDatabaseProfileLevel(nss.dbName()));
        }
    } else {
        invariant(cursorPin->getExecutor()->lockPolicy() ==
                  PlanExecutor::LockPolicy::kLockExternally);

        if (cursorPin->getExecutor()->usesCollectionAcquisitions()) {
            // Restore the acquisitions used in the original call. This takes care of
            // checking that the preconditions for the original acquisition still hold and
            // restores any locks necessary.
            txnResourcesHandler.emplace(opCtx, cursorPin.getCursor());
        } else {
            // Lock the backing collection by using the executor's namespace. Note that it
            // may be different from the cursor's namespace. One such possible scenario is
            // when getMore() is executed against a view. Technically, views are pipelines
            // and under normal circumstances use 'kLocksInternally' policy, so we shouldn't
            // be getting into here in the first place. However, if the pipeline was
            // optimized away and replaced with a query plan, its lock policy would have
            // also been changed to 'kLockExternally'. So, we'll use the executor's
            // namespace to take the lock (which is always the backing collection
            // namespace), but will use the namespace provided in the user request for
            // profiling.
            //
            // Otherwise, these two namespaces will match.
            //
            // Note that some pipelines which were optimized away may require locking
            // multiple namespaces. As such, we pass any secondary namespaces required by
            // the pinned cursor's executor when constructing 'readLock'.
            const auto& secondaryNamespaces = cursorPin->getExecutor()->getSecondaryNamespaces();
            readLock.emplace(opCtx,
                             cursorPin->getExecutor()->nss(),
                             AutoGetCollection::Options{}.secondaryNssOrUUIDs(
                                 secondaryNamespaces.cbegin(), secondaryNamespaces.cend()));
        }

        statsTracker.emplace(opCtx,
                             nss,
                             Top::LockType::ReadLocked,
                             AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                             DatabaseProfileSettings::get(opCtx->getServiceContext())
                                 .getDatabaseProfileLevel(nss.dbName()));

        // Check whether we are allowed to read from this node after acquiring our locks.
        // When using ShardRole, only check when there is at least one acquired collection/view – it
        // is possible that a ShardRole acquisition is empty when the enclosed executor is a trivial
        // EOF plan.
        if (!cursorPin->getExecutor()->usesCollectionAcquisitions() ||
            !shard_role_details::TransactionResources::get(opCtx).isEmpty()) {
            uassertStatusOK(
                repl::ReplicationCoordinator::get(opCtx)->checkCanServeReadsFor(opCtx, nss, true));
        }
    }

    cursorDeleter.dismiss();
}

}  // namespace mongo
