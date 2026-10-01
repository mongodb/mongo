// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_buffer.h"

#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::replicated_fast_count {
namespace {

/**
 * Returns true if the raw oplog BSON is a no-op watermark entry, i.e. a noop ('n') carrying
 * `o.msg == kWatermarkMsg`.
 */
bool isWatermarkNoop(const BSONObj& raw) {
    const BSONElement opElem = raw.getField("op");
    if (opElem.type() != BSONType::string || opElem.valueStringData() != "n") {
        return false;
    }
    const BSONElement oElem = raw.getField("o");
    if (!oElem.isABSONObj()) {
        return false;
    }
    const BSONObj o = oElem.Obj();
    const BSONElement msgElem = o.getField("msg");
    return msgElem.type() == BSONType::string && msgElem.valueStringData() == kWatermarkMsg;
}

}  // namespace

SizeCountCheckpointBuffer::SizeCountCheckpointBuffer(UUID oplogUuid,
                                                     boost::optional<RecordId> lastBufferedRid)
    : _accumulatorOptions{.isCheckpoint = true, .oplogUuid = oplogUuid},
      _lastBufferedRid(lastBufferedRid) {
    _pending.emplace(_accumulatorOptions);
}

boost::optional<OplogScanResult> SizeCountCheckpointBuffer::checkoutForFlush() {
    std::lock_guard lk(_mutex);
    return _inFlight;
}

bool SizeCountCheckpointBuffer::readyForWatermark() const {
    std::lock_guard lk(_mutex);
    return !_inFlight.has_value() && _pending->hasPendingWork();
}

OplogScanResult SizeCountCheckpointBuffer::awaitCheckoutForFlush(OperationContext* opCtx) {
    const Date_t start = Date_t::now();
    std::unique_lock lk(_mutex);
    opCtx->waitForConditionOrInterrupt(_batchReady, lk, [&] { return _inFlight.has_value(); });
    recordWatermarkAwaitTime(Date_t::now() - start);
    return *_inFlight;
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
                if (isWatermarkNoop(record->data.toBson())) {
                    _handleWatermark();
                }
            }
        }
    }

    // We advance lastBufferedRid on each iteration so that if cursor.next() throws a
    // WriteConflictException, the caller can resume scanning after the last consumed record.
    while (boost::optional<Record> record = cursor.next()) {
        _pending->consumeRecord(*record);
        _lastBufferedRid = record->id;
        // TODO(SERVER-135085): Update this loop to not swallow multiple watermarks. We want one
        // checkpoint per watermark, so we need to stop accumulating in _pending past this point.
        if (isWatermarkNoop(record->data.toBson())) {
            _handleWatermark();
        }
    }
}

void SizeCountCheckpointBuffer::acknowledgeFlush() {
    std::lock_guard lk(_mutex);
    _inFlight.reset();
}

void SizeCountCheckpointBuffer::_handleWatermark() {
    incrementTailerWatermarksSeenCount();
    if (!_inFlight.has_value()) {
        // Cut the accumulated batch, the watermark included, so its timestamp is the
        // batch's lastTimestamp.
        _inFlight = _pending->finish();
        _pending.emplace(_accumulatorOptions);
        _batchReady.notify_one();
    }
}
}  // namespace mongo::replicated_fast_count
