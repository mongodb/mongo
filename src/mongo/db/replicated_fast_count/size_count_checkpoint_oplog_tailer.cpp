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

struct ScanResultAndLastSeenRid {
    OplogScanResult scanResult;
    boost::optional<RecordId> lastSeenRid;
};

RecordId makeStartRecordId(Timestamp startAfter) {
    return massertStatusOK(record_id_helpers::keyForOptime(startAfter, KeyFormat::Long));
}

// A forward cursor on the oplog is guaranteed to only scan until the no holes point. This is
// necessary to guarantee that all new oplog entries are parsed in order for correct size and count
// accumulation.
ScanResultAndLastSeenRid scanUntilEOF(SeekableRecordCursor& cursor, const UUID& oplogUuid) {
    StreamingOplogDeltaAccumulator acc({
        .isCheckpoint = true,
        .oplogUuid = oplogUuid,
    });

    boost::optional<RecordId> lastSeenRecordId;
    while (auto rec = cursor.next()) {
        acc.consumeRecord(*rec);
        lastSeenRecordId = rec->id;
    }

    return {acc.finish(), std::move(lastSeenRecordId)};
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

    auto out = scanUntilEOF(*state.cursor, coll->uuid());

    const bool madeProgress = out.lastSeenRid.has_value();
    if (madeProgress) {
        buffer.mergeVisibleScan(std::move(out.scanResult));
        state.lastBufferedRid = *out.lastSeenRid;
    }

    state.cursor->save();
    ru->abandonSnapshot();
    return madeProgress;
}

}  // namespace
void SizeCountCheckpointOplogTailer::run(OperationContext* opCtx,
                                         Timestamp startAfter,
                                         SizeCountCheckpointBuffer& buffer) {
    auto notifier = acquireInsertNotifier(opCtx);
    auto state = _bootstrap(opCtx, startAfter, buffer);

    // If the oplog starts out empty.
    while (!state) {
        opCtx->checkForInterrupt();
        auto waitVersion = notifier->getVersion();
        state = _bootstrap(opCtx, startAfter, buffer);
        // Don't wait forever, as this shouldn't be the state for long durations.
        notifier->waitUntil(opCtx, waitVersion, Date_t::now() + Seconds(1));
    }

    invariant(state.has_value());

    while (true) {
        opCtx->checkForInterrupt();
        auto waitVersion = notifier->getVersion();
        bool madeProgress = runOneIteration(opCtx, *state, buffer);
        if (!madeProgress) {
            state->cursor.reset();
        }
        notifier->waitUntil(opCtx, waitVersion, Date_t::max());
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
        // accounted for in the buffer since there's no true Oplog entry to start after.
        StreamingOplogDeltaAccumulator acc({
            .isCheckpoint = true,
            .oplogUuid = coll->uuid(),
        });
        acc.consumeRecord(*first);
        buffer.mergeVisibleScan(acc.finish());

        lastBufferedRid = first->id;
    } else {
        // There's a real last-scanned oplog entry. Ensure it hasn't fallen off the oplog so the
        // tailer can startup safely.
        lastBufferedRid = makeStartRecordId(startAfter);
        tassert(12101811,
                str::stream() << "Expected already-processed oplog entry at timestamp "
                              << startAfter << " to exist",
                cursor->seekExact(lastBufferedRid));
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
