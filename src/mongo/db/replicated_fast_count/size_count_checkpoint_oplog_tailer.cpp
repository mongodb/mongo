/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/replicated_fast_count/size_count_checkpoint_oplog_tailer.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::replicated_fast_count {
namespace {

using TailerState = SizeCountCheckpointOplogTailer::TailerState;

RecordId makeStartRecordId(Timestamp startAfter) {
    return massertStatusOK(record_id_helpers::keyForOptime(startAfter, KeyFormat::Long));
}

AutoGetCollection acquireOplogForRead(OperationContext* opCtx) {
    AutoGetCollection coll(opCtx, NamespaceString::kRsOplogNamespace, MODE_IS);
    tassert(12101803, "oplog collection not found", coll);
    return coll;
}

std::shared_ptr<CappedInsertNotifier> acquireInsertNotifier(OperationContext* opCtx) {
    AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
    const auto& oplogColl = oplogRead.getCollection();
    tassert(12101805, "oplog collection not found", oplogColl);
    return oplogColl->getRecordStore()->capped()->getInsertNotifier();
}

void ensureCursor(OperationContext* opCtx,
                  RecoveryUnit* ru,
                  const CollectionPtr& oplogColl,
                  TailerState& state) {
    if (!state.cursor) {
        // Starting with a new cursor.
        state.cursor = oplogColl->getRecordStore()->getCursor(opCtx, *ru);
        return;
    }
    // The cursor must have started in a saved state. Time to restore.
    if (!state.cursor->restore(*ru, /*tolerateCappedRepositioning=*/false)) {
        // Aside from after an initial bootstrap, the cursor should always successfully restore
        // given it was previously exhausted to EOF.
        // If it cannot restore, reset the cursor. Restore is always followed by a seekExact, which
        // maintains correctness that the last digested oplog entry still exists on the oplog.
        LOGV2_WARNING(12101813,
                      "Unexpectedly could not restore cursor for tailing oplog size and count. "
                      "last Scanned Oplog recordId",
                      "lastBufferedRid"_attr = state.lastBufferedRid);
        state.cursor.reset();
        state.cursor = oplogColl->getRecordStore()->getCursor(opCtx, *ru);
    }
}

// Resumes scanning for new oplog entries after 'lastBufferedRid'. Verifies `lastBufferedRid` is
// still present in the oplog to ensure all size count deltas are accounted.
bool runOneIteration(OperationContext* opCtx,
                     TailerState& state,
                     SizeCountCheckpointBuffer& buffer) {
    auto oplogRead = acquireOplogForRead(opCtx);
    const CollectionPtr& coll = *oplogRead;

    auto* ru = shard_role_details::getRecoveryUnit(opCtx);
    ensureCursor(opCtx, ru, coll, state);
    invariant(state.cursor);
    tassert(12101812,
            str::stream() << "Unable to find oplog start point for next size count checkpoint"
                          << ", lastBufferedRid: " << state.lastBufferedRid.toStringHumanReadable(),
            state.cursor->seekExact(state.lastBufferedRid));

    const boost::optional<RecordId> lastSeenRid = buffer.scanToNoHolesEOF(*state.cursor);

    const bool madeProgress = lastSeenRid.has_value();
    if (madeProgress) {
        state.lastBufferedRid = *lastSeenRid;
    }

    state.cursor->save();
    ru->abandonSnapshot();
    return madeProgress;
}

}  // namespace
void SizeCountCheckpointOplogTailer::run(OperationContext* opCtx,
                                         Timestamp startAfter,
                                         SizeCountCheckpointBuffer& buffer) {
    std::shared_ptr<CappedInsertNotifier> notifier;
    boost::optional<TailerState> state = boost::none;

    while (!state) {
        try {
            if (!notifier) {
                notifier = acquireInsertNotifier(opCtx);
            }
            opCtx->checkForInterrupt();
            auto waitVersion = notifier->getVersion();
            state = _bootstrap(opCtx, startAfter, buffer);
            if (!state) {
                // The oplog is still empty.
                notifier->waitUntil(opCtx, waitVersion, Date_t::now() + Seconds(1));
            }
        } catch (const DBException& ex) {
            if (ex.code() == ErrorCodes::InterruptedDueToReplStateChange) {
                LOGV2_DEBUG(12917800,
                            2,
                            "SizeCountCheckpointOplogTailer interrupted due to replication state",
                            "error"_attr = ex.toStatus());
                return;
            } else {
                // The `state` is necessarily none here, so nothing to discard.
                LOGV2_WARNING(12917801,
                              "Exception handled in SizeCountCheckpointOplogTailer::run()",
                              "error"_attr = ex.toStatus());
            }
        }
    }

    while (true) {
        try {
            opCtx->checkForInterrupt();
            auto waitVersion = notifier->getVersion();
            bool madeProgress = runOneIteration(opCtx, *state, buffer);
            if (!madeProgress) {
                state->cursor.reset();
            }
            notifier->waitUntil(opCtx, waitVersion, Date_t::max());
        } catch (const DBException& ex) {
            if (ex.code() == ErrorCodes::InterruptedDueToReplStateChange) {
                LOGV2_DEBUG(12917802,
                            2,
                            "SizeCountCheckpointOplogTailer interrupted due to replication state",
                            "error"_attr = ex.toStatus());
                return;
            } else {
                LOGV2_WARNING(12917803,
                              "Exception handled in SizeCountCheckpointOplogTailer::run()",
                              "error"_attr = ex.toStatus());
                state->cursor.reset();
            }
        }
    }
}

boost::optional<TailerState> SizeCountCheckpointOplogTailer::_bootstrap(
    OperationContext* opCtx, Timestamp startAfter, SizeCountCheckpointBuffer& buffer) {
    auto oplogRead = acquireOplogForRead(opCtx);
    const CollectionPtr& coll = *oplogRead;
    auto* ru = shard_role_details::getRecoveryUnit(opCtx);
    auto cursor = coll->getRecordStore()->getCursor(opCtx, *ru);
    invariant(cursor);

    RecordId lastBufferedRid;
    if (startAfter == Timestamp::min()) {
        // Flags the special case where the tailer should start at the beginning of the oplog.
        auto first = cursor->next();
        if (!first) {
            // Oplog is empty.
            return boost::none;
        }

        // Since the tailer is starting at the beginning of the oplog, ensure the first record is
        // accounted for in the buffer since there's no true oplog entry to start after.
        buffer.accumulate(*first);

        lastBufferedRid = first->id;
    } else {
        // Resume from the last oplog entry included in the most recent size/count checkpoint.
        //
        // TODO SERVER-129451: Enforce that this entry is still present in the oplog before
        // resuming. Until then, we temporarily allow startup to proceed even if it has been
        // truncated, which can cause size and count to be inaccurate after oplog rollover.
        lastBufferedRid = makeStartRecordId(startAfter);
        const auto seekResult =
            cursor->seek(lastBufferedRid, SeekableRecordCursor::BoundInclusion::kInclude);
        if (!seekResult) {
            // No new oplog entries since the previous size count checkpoint.
            return boost::none;
        }
        if (seekResult->id != lastBufferedRid) {
            LOGV2_WARNING(12880600,
                          "Expected already-processed oplog entry at timestamp to exist, "
                          "seeking to next available entry",
                          "startAfter"_attr = startAfter,
                          "expectedRid"_attr = lastBufferedRid,
                          "foundRid"_attr = seekResult->id);
        }
        lastBufferedRid = seekResult->id;
    }

    cursor->save();
    ru->abandonSnapshot();

    return TailerState{.cursor = std::move(cursor), .lastBufferedRid = std::move(lastBufferedRid)};
}

bool SizeCountCheckpointOplogTailer::runOneIteration_ForTest(OperationContext* opCtx,
                                                             boost::optional<TailerState>& state,
                                                             SizeCountCheckpointBuffer& buffer) {
    if (!state) {
        return false;
    }
    return runOneIteration(opCtx, *state, buffer);
}
}  // namespace mongo::replicated_fast_count
