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
    // The boost::none state means we have not buffered any oplog entries yet.
    if (_lastBufferedRid.has_value()) {
        const bool lastBufferedRidFound = cursor.seekExact(_lastBufferedRid.value()).has_value();
        if (!lastBufferedRidFound) {
            if (!_reportedLostLastBufferedRid) {
                // Avoid looping with the same assertion.
                _reportedLostLastBufferedRid = true;
                tasserted(
                    12101812,
                    fmt::format(
                        "Unable to find oplog start point for next size count "
                        "checkpoint at lastBufferedRid: {}. A repair procedure will be needed.",
                        _lastBufferedRid.value().toStringHumanReadable()));
            }

            if (boost::optional<Record> record = cursor.seek(
                    _lastBufferedRid.value(), SeekableRecordCursor::BoundInclusion::kExclude)) {
                _pending->consumeRecord(*record);
                _lastBufferedRid = record->id;
                // Unset the flag so the next loss of the last buffered record is a new anomaly and
                // gets its own tassert.
                _reportedLostLastBufferedRid = false;
            }
        }
    }

    // We advance lastBufferedRid on each iteration so that if cursor.next() throws a
    // WriteConflictException, the caller can resume scanning after the last consumed record.
    while (boost::optional<Record> record = cursor.next()) {
        _pending->consumeRecord(*record);
        _lastBufferedRid = record->id;
    }
}

void SizeCountCheckpointBuffer::acknowledgeFlushSuccess() {
    _inFlight.reset();
}
}  // namespace mongo::replicated_fast_count
