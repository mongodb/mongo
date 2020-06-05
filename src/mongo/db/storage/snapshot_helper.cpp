/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/snapshot_helper.h"

#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace SnapshotHelper {
bool canSwitchReadSource(OperationContext* opCtx) {

    // Most readConcerns have behavior controlled at higher levels. Local and available are the only
    // ReadConcerns that should consider changing, since they read without a timestamp by default.
    const auto readConcernLevel = repl::ReadConcernArgs::get(opCtx).getLevel();
    if (readConcernLevel == repl::ReadConcernLevel::kLocalReadConcern ||
        readConcernLevel == repl::ReadConcernLevel::kAvailableReadConcern) {
        return true;
    }

    return false;
}

bool shouldReadAtLastApplied(OperationContext* opCtx,
                             const NamespaceString& nss,
                             std::string* reason) {

    // If this is true, then the operation opted-in to the PBWM lock, implying that it cannot change
    // its ReadSource. It's important to note that it is possible for this to be false, but still be
    // holding the PBWM lock, explained below.
    if (opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()) {
        *reason = "conflicts with batch application";
        return false;
    }

    // If we are already holding the PBWM lock, do not change ReadSource. Snapshots acquired by an
    // operation after a yield/restore must see all writes in the pre-yield snapshot. Once a
    // snapshot is reading without a timestamp, we choose to continue acquiring snapshots without a
    // timestamp. This is done in lieu of determining a timestamp far enough in the future that's
    // guaranteed to observe all previous writes. This may occur when multiple collection locks are
    // held concurrently, which is often the case when DBDirectClient is used.
    if (opCtx->lockState()->isLockHeldForMode(resourceIdParallelBatchWriterMode, MODE_IS)) {
        *reason = "PBWM lock is held";
        LOGV2_DEBUG(20577, 1, "not reading at lastApplied because the PBWM lock is held");
        return false;
    }

    // If we are in a replication state (like secondary or primary catch-up) where we are not
    // accepting writes, we should read at lastApplied. If this node can accept writes, then no
    // conflicting replication batches are being applied and we can read from the default snapshot.
    if (repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx, "admin")) {
        *reason = "primary";
        return false;
    }

    // Non-replicated collections do not need to read at lastApplied, as those collections are not
    // written by the replication system.  However, the oplog is special, as it *is* written by the
    // replication system.
    if (!nss.isReplicated() && !nss.isOplog()) {
        *reason = "unreplicated collection";
        return false;
    }

    return true;
}
boost::optional<RecoveryUnit::ReadSource> getNewReadSource(OperationContext* opCtx,
                                                           const NamespaceString& nss) {
    const bool canSwitch = canSwitchReadSource(opCtx);
    if (!canSwitch) {
        return boost::none;
    }

    const auto existing = opCtx->recoveryUnit()->getTimestampReadSource();
    std::string reason;
    const bool readAtLastApplied = shouldReadAtLastApplied(opCtx, nss, &reason);
    if (existing == RecoveryUnit::ReadSource::kUnset) {
        // Shifting from reading without a timestamp to reading with a timestamp can be dangerous
        // because writes will appear to vanish. This case is intended for new reads on secondaries
        // and query yield recovery after state transitions from primary to secondary.

        // If a query recovers from a yield and the node is no longer primary, it must start reading
        // at the lastApplied point because reading without a timestamp is not safe.
        if (readAtLastApplied) {
            LOGV2_DEBUG(4452901, 2, "Changing ReadSource to kLastApplied", logAttrs(nss));
            return RecoveryUnit::ReadSource::kLastApplied;
        }
    } else if (existing == RecoveryUnit::ReadSource::kLastApplied) {
        // For some reason, we can no longer read at lastApplied.
        // An operation that yields a timestamped snapshot must restore a snapshot with at least as
        // large of a timestamp, or with proper consideration of rollback scenarios, no timestamp.
        // Given readers do not survive rollbacks, it's okay to go from reading with a timestamp to
        // reading without one. More writes will become visible.
        if (!readAtLastApplied) {
            LOGV2_DEBUG(
                4452902, 2, "Changing ReadSource to kUnset", logAttrs(nss), "reason"_attr = reason);
            // This shift to kUnset assumes that callers will not make future attempts to manipulate
            // their ReadSources after performing reads at an un-timetamped snapshot. The only
            // exception is callers of this function that may need to change from kUnset to
            // kLastApplied in the event of a catalog conflict or query yield.
            return RecoveryUnit::ReadSource::kUnset;
        }
    }
    return boost::none;
}

bool collectionChangesConflictWithRead(boost::optional<Timestamp> collectionMin,
                                       boost::optional<Timestamp> readTimestamp) {
    // This is the timestamp of the most recent catalog changes to this collection. If this is
    // greater than any point in time read timestamps, we should either wait or return an error.
    if (!collectionMin) {
        return false;
    }

    // If we do not have a point in time to conflict with collectionMin, return.
    if (!readTimestamp || readTimestamp->isNull()) {
        return false;
    }

    // Return if there are no conflicting catalog changes with the readTimestamp.
    return *collectionMin > readTimestamp;
}
}  // namespace SnapshotHelper
}  // namespace mongo