// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_streaming_oplog_delta_accumulator.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/uuid.h"

#include <mutex>

#include <boost/optional/optional.hpp>

namespace mongo::replicated_fast_count {

/**
 * The handoff channel between the oplog tailer (single producer) and the checkpoint flusher
 * (single consumer). The producer accumulates size/count deltas across oplog scans into a
 * long-lived accumulator. The oplog tailer cuts a batch when it sees a no-op watermark entry.
 * Thread-safe.
 */
class SizeCountCheckpointBuffer {
public:
    /**
     * `oplogUuid` is the UUID of the oplog collection being tailed. We assume the oplog UUID does
     * not change during the lifetime of the buffer.
     *
     * The first `scanToNoHolesEOF()` call seeks to strictly after `lastBufferedRid`. If
     * `lastBufferedRid` is `boost::none`, the scan starts at the beginning of the oplog.
     */
    SizeCountCheckpointBuffer(UUID oplogUuid, boost::optional<RecordId> lastBufferedRid);

    /**
     * Consumer side. Returns the in-flight batch, or `boost::none` when no batch has been cut.
     *
     * Acquires the buffer mutex, so it blocks on any in-progress scans.
     */
    boost::optional<OplogScanResult> checkoutForFlush();

    /**
     * Returns true when the buffer is ready to accumulate a watermark in `scanToNoHolesEOF()`.
     */
    bool readyForWatermark() const;

    /**
     * Consumer side. Returns the batch immediately if it is ready, or blocks indefinitely until
     * signaled that the next batch has been cut.
     */
    OplogScanResult awaitCheckoutForFlush(OperationContext* opCtx);

    /**
     * Producer side. Seeks `cursor` forward to immediately after the last buffered record, then
     * scans `cursor` to the no-holes EOF, accumulating every record into the long-lived pending
     * accumulator.
     *
     * When the scan consumes a no-op watermark entry, it moves the pending batch, including the
     * watermark, into the in-flight batch and notifies the `awaitCheckoutForFlush()` waiter, if one
     * exists.
     *
     * Holds the buffer mutex for the duration of the scan so that a concurrent
     * `awaitCheckoutForFlush()` cannot observe a partially-applied scan.
     *
     * If `cursor.next()` throws a WriteConflictException mid-scan, the next call to
     * `scanToNoHolesEOF()` resumes strictly after the last accumulated record.
     */
    void scanToNoHolesEOF(SeekableRecordCursor& cursor);

    /**
     * Acknowledges that the _inFlight result is safe to clear.
     */
    void acknowledgeFlush();

private:
    /**
     * Handles a watermark seen while tailing the oplog by finalizing the `_pending` batch and
     * moving it into `_inFlight`. If `_inFlight` already exists, this is a no-op.
     */
    void _handleWatermark();

    // The options used to build every pending accumulator.
    const StreamingOplogDeltaAccumulator::Options _accumulatorOptions;

    // The record ID of the last record consumed into _pending. The boost::none state means we have
    // not buffered any oplog entries yet.
    boost::optional<RecordId> _lastBufferedRid = boost::none;

    // Set while the last buffered record ID has been observed to be lost and the anomaly reported,
    // so the same loss is not re-reported on every scan. Cleared once scanning has recovered,
    // re-arming the report for the next loss.
    bool _reportedLostLastBufferedRid = false;

    mutable std::mutex _mutex;
    // Accumulated size/count deltas currently being flushed. Written by the producer when the
    // scan cuts a batch at a watermark, and read/cleared by the consumer. All access is
    // synchronized under _mutex.
    boost::optional<OplogScanResult> _inFlight;
    // Notified whenever the scan cuts a batch into _inFlight. See awaitCheckoutForFlush().
    stdx::condition_variable _batchReady;
    // Accumulated size/count deltas buffered but not yet cut.
    boost::optional<StreamingOplogDeltaAccumulator> _pending;
};

}  // namespace mongo::replicated_fast_count
