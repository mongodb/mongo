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


#include "mongo/platform/basic.h"

#include "mongo/db/storage/snapshot_helper.h"

#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {
bool canReadAtLastApplied(OperationContext* opCtx) {
    // Local and available are the only ReadConcern levels that allow their ReadSource to be
    // overridden to read at lastApplied. They read without a timestamp by default, but this check
    // allows user secondary reads from conflicting with oplog batch application by reading at a
    // consistent point in time.
    // Internal operations use DBDirectClient as a loopback to perform local operations, and they
    // expect the same level of consistency guarantees as any user operation. For that reason,
    // DBDirectClient should be able to change the owning operation's ReadSource in order to serve
    // consistent data.
    const auto readConcernLevel = repl::ReadConcernArgs::get(opCtx).getLevel();
    if ((opCtx->getClient()->isFromUserConnection() || opCtx->getClient()->isInDirectClient()) &&
        (readConcernLevel == repl::ReadConcernLevel::kLocalReadConcern ||
         readConcernLevel == repl::ReadConcernLevel::kAvailableReadConcern)) {
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
        if (reason) {
            *reason = "conflicts with batch application";
        }
        return false;
    }

    // If we are already holding the PBWM lock, do not change ReadSource. Snapshots acquired by an
    // operation after a yield/restore must see all writes in the pre-yield snapshot. Once a
    // snapshot is reading without a timestamp, we choose to continue acquiring snapshots without a
    // timestamp. This is done in lieu of determining a timestamp far enough in the future that's
    // guaranteed to observe all previous writes. This may occur when multiple collection locks are
    // held concurrently, which is often the case when DBDirectClient is used.
    if (opCtx->lockState()->isLockHeldForMode(resourceIdParallelBatchWriterMode, MODE_IS)) {
        if (reason) {
            *reason = "PBWM lock is held";
        }
        LOGV2_DEBUG(20577, 1, "not reading at lastApplied because the PBWM lock is held");
        return false;
    }

    // If this node can accept writes (i.e. primary), then no conflicting replication batches are
    // being applied and we can read from the default snapshot. If we are in a replication state
    // (like secondary or primary catch-up) where we are not accepting writes, we should read at
    // lastApplied.
    if (repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx, "admin")) {
        if (reason) {
            *reason = "primary";
        }
        return false;
    }

    // If we are not secondary, then we should not attempt to read at lastApplied because it may not
    // be available or valid. Any operations reading outside of the primary or secondary states must
    // be internal. We give these operations the benefit of the doubt rather than attempting to read
    // at a lastApplied timestamp that is not valid.
    if (!repl::ReplicationCoordinator::get(opCtx)->isInPrimaryOrSecondaryState(opCtx)) {
        if (reason) {
            *reason = "not primary or secondary";
        }
        return false;
    }

    // Non-replicated collections do not need to read at lastApplied, as those collections are not
    // written by the replication system.  However, the oplog is special, as it *is* written by the
    // replication system.
    if (!nss.isReplicated() && !nss.isOplog()) {
        if (reason) {
            *reason = "unreplicated collection";
        }
        return false;
    }

    // Linearizable read concern should never be read at lastApplied, they must always read from
    // latest and are only allowed on primaries.
    if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
        repl::ReadConcernLevel::kLinearizableReadConcern) {
        if (reason) {
            *reason = "linearizable read concern";
        }
        return false;
    }

    return true;
}

}  // namespace

namespace SnapshotHelper {

bool changeReadSourceIfNeeded(OperationContext* opCtx, const NamespaceString& nss) {
    std::string reason;
    // Write to the reason string if debug logging is enabled. This avoids writing this string every
    // time we check if we should read at last applied. This string itself is only used in logging
    // with the same debug level as this check.
    std::string* reasonWriter =
        logv2::shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(2)) ? &reason
                                                                                      : nullptr;
    bool readAtLastApplied = shouldReadAtLastApplied(opCtx, nss, reasonWriter);

    if (!canReadAtLastApplied(opCtx)) {
        return readAtLastApplied;
    }

    const auto originalReadSource = opCtx->recoveryUnit()->getTimestampReadSource();
    if (opCtx->recoveryUnit()->isReadSourcePinned()) {
        LOGV2_DEBUG(5863601,
                    2,
                    "Not changing readSource as it is pinned",
                    "current"_attr = RecoveryUnit::toString(originalReadSource),
                    "rejected"_attr = readAtLastApplied
                        ? RecoveryUnit::toString(RecoveryUnit::ReadSource::kLastApplied)
                        : RecoveryUnit::toString(RecoveryUnit::ReadSource::kNoTimestamp));
        return false;
    }

    // We may only change to kLastApplied if we were reading without a timestamp (or if kLastApplied
    // is already set)
    if (originalReadSource != RecoveryUnit::ReadSource::kNoTimestamp &&
        originalReadSource != RecoveryUnit::ReadSource::kLastApplied) {
        return readAtLastApplied;
    }

    // Helper to set read source to the recovery unit and remember our current setting
    auto currentReadSource = originalReadSource;
    auto setReadSource = [&](RecoveryUnit::ReadSource readSource) {
        opCtx->recoveryUnit()->setTimestampReadSource(readSource);
        currentReadSource = readSource;
    };

    // Set read source based on current setting and readAtLastApplied decision.
    if (originalReadSource == RecoveryUnit::ReadSource::kLastApplied && !readAtLastApplied) {
        setReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
    } else if (readAtLastApplied) {
        // Shifting from reading without a timestamp to reading with a timestamp can be
        // dangerous because writes will appear to vanish.
        //
        // If a query recovers from a yield and the node is no longer primary, it must start
        // reading at the lastApplied point because reading without a timestamp is not safe.
        //
        // An operation that yields a timestamped snapshot must restore a snapshot with at least
        // as large of a timestamp, or with proper consideration of rollback scenarios, no
        // timestamp. Given readers do not survive rollbacks, it's okay to go from reading with
        // a timestamp to reading without one. More writes will become visible.
        //
        // If we already had kLastApplied as our read source then this call will refresh the
        // timestamp.
        setReadSource(RecoveryUnit::ReadSource::kLastApplied);

        // We need to make sure the decision if we need to read at last applied is not changing
        // concurrently with setting the read source with its read timestamp to the recovery unit.
        //
        // When the timestamp is being selected we might have transitioned into PRIMARY that is
        // accepting writes. The lastApplied timestamp can have oplog holes behind it, in PRIMARY
        // mode, making it unsafe as a read timestamp as concurrent writes could commit at earlier
        // timestamps.
        //
        // This is handled by re-verifying the conditions if we need to read at last applied after
        // determining the timestamp but before opening the storage snapshot. If the conditions do
        // not match what we recorded at the beginning of the operation, we set the read source back
        // to kNoTimestamp and read without a timestamp.
        //
        // The above mainly applies for Lock-free reads that is not holding the RSTL which protects
        // against state changes.
        reason.clear();
        if (!shouldReadAtLastApplied(opCtx, nss, reasonWriter)) {
            // State changed concurrently with setting the read source and we should no longer read
            // at lastApplied.
            setReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
            readAtLastApplied = false;
        }
    }

    // All done, log if we made a change to the read source
    if (originalReadSource == RecoveryUnit::ReadSource::kNoTimestamp &&
        currentReadSource == RecoveryUnit::ReadSource::kLastApplied) {
        LOGV2_DEBUG(4452901,
                    2,
                    "Changed ReadSource to kLastApplied",
                    logAttrs(nss),
                    "ts"_attr = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx));
    } else if (originalReadSource == RecoveryUnit::ReadSource::kLastApplied &&
               currentReadSource == RecoveryUnit::ReadSource::kLastApplied) {
        LOGV2_DEBUG(6730500,
                    2,
                    "ReadSource kLastApplied updated timestamp",
                    logAttrs(nss),
                    "ts"_attr = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx));
    } else if (originalReadSource == RecoveryUnit::ReadSource::kLastApplied &&
               currentReadSource == RecoveryUnit::ReadSource::kNoTimestamp) {
        LOGV2_DEBUG(4452902,
                    2,
                    "Changed ReadSource to kNoTimestamp",
                    logAttrs(nss),
                    "reason"_attr = reason);
    }

    // Return if we need to read at last applied to the caller in case further checks need to be
    // performed.
    return readAtLastApplied;
}

bool collectionChangesConflictWithRead(boost::optional<Timestamp> collectionMin,
                                       boost::optional<Timestamp> readTimestamp) {
    if (!collectionMin) {
        return false;
    }

    // If we do not have a point in time to conflict with collectionMin, return.
    if (!readTimestamp || readTimestamp->isNull()) {
        return false;
    }

    // If the last change to the collection was before or at the read timestamp, then the storage
    // snapshot will match the collection in-memory state. Return true only if there would be an
    // inconsistency: a collection with a newer min timestamp would not match an older storage
    // snapshot.
    return *collectionMin > readTimestamp;
}
}  // namespace SnapshotHelper
}  // namespace mongo
