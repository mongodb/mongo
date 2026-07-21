// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_buffer.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::replicated_fast_count {

SizeCountCheckpointBuffer::SizeCountCheckpointBuffer(UUID oplogUuid,
                                                     boost::optional<RecordId> lastBufferedRid)
    : _accumulatorOptions{.isCheckpoint = true, .oplogUuid = oplogUuid},
      _lastBufferedRid(lastBufferedRid) {
    _pending.emplace(_accumulatorOptions);
}

boost::optional<OplogScanResult> SizeCountCheckpointBuffer::checkoutForFlush() {
    if (_inFlight) {
        // A previous flush has not been acknowledged yet. Retry the same batch.
        return _inFlight;
    }

    OplogScanResult result;
    {
        std::lock_guard lk(_mutex);
        // Cut the _pending accumulator into a flushable batch and reset _pending.
        result = _pending->finish();
        _pending.emplace(_accumulatorOptions);
    }

    if (!result.lastTimestamp) {
        // finish() leaves lastTimestamp unset when no size/count entries were accumulated. We
        // return early so we do not advance the checkpoint for untracked entries.
        return boost::none;
    }

    _inFlight = std::move(result);
    return _inFlight;
}

void SizeCountCheckpointBuffer::scanToNoHolesEOF(SeekableRecordCursor& cursor) {
    std::lock_guard lk(_mutex);
    boost::optional<Record> record;
    // The boost::none state means we have not buffered any oplog entries yet.
    if (_lastBufferedRid.has_value()) {
        // Size-based oplog truncation should never truncate the last buffered record ID.
        // Time-based oplog truncation can truncate the last buffered record ID, so we relax this
        // assertion until SERVER-131791.
        // TODO(SERVER-131842): Assert this unconditionally.
        if (gFeatureFlagSizeBasedOplogTruncationForDisagg.isEnabled()) {
            tassert(
                12101812,
                str::stream() << "Unable to find oplog start point for next size count checkpoint"
                              << ", lastBufferedRid: "
                              << _lastBufferedRid.value().toStringHumanReadable(),
                cursor.seekExact(_lastBufferedRid.value()));
            record = cursor.next();
        } else {
            record = cursor.seek(_lastBufferedRid.value(),
                                 SeekableRecordCursor::BoundInclusion::kExclude);
        }
    } else {
        record = cursor.next();
    }

    // We advance lastBufferedRid on each iteration so that if cursor.next() throws a
    // WriteConflictException, the caller can resume scanning after the last consumed record.
    while (record) {
        _pending->consumeRecord(*record);
        _lastBufferedRid = record->id;
        record = cursor.next();
    }
}

void SizeCountCheckpointBuffer::acknowledgeFlushSuccess() {
    _inFlight.reset();
}
}  // namespace mongo::replicated_fast_count
