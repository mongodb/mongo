// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/record_id.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_streaming_oplog_delta_accumulator.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/uuid.h"

#include <mutex>

#include <boost/optional/optional.hpp>

namespace mongo::replicated_fast_count {

/**
 * The handoff channel between the oplog tailer (single producer) and the checkpoint flusher
 * (single consumer). The producer accumulates size/count deltas across oplog scans into a
 * long-lived accumulator. The consumer cuts that accumulation into an in-flight batch to flush.
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
     * Consumer side. Cuts everything accumulated since the last successful flush into an in-flight
     * batch and returns it, or returns the existing in-flight batch if a prior flush has not yet
     * been acknowledged (retry). Returns boost::none when there is no real work to flush.
     *
     * Acquires the buffer mutex, so it blocks on any in-progress scans.
     */
    boost::optional<OplogScanResult> checkoutForFlush();

    /**
     * Producer side. Seeks `cursor` forward to immediately after the last buffered record, then
     * scans `cursor` to the no-holes EOF, accumulating every record into the long-lived pending
     * accumulator.
     *
     * Holds the buffer mutex for the duration of the scan so that a concurrent `checkoutForFlush()`
     * cannot observe a partially-applied scan.
     *
     * If `cursor.next()` throws a WriteConflictException mid-scan, the next call to
     * `scanToNoHolesEOF()` resumes strictly after the last accumulated record.
     */
    void scanToNoHolesEOF(SeekableRecordCursor& cursor);

    /**
     * Acknowledges that the _inFlight result has been persisted and is safe to clear.
     */
    void acknowledgeFlushSuccess();

private:
    // The options used to build every pending accumulator.
    const StreamingOplogDeltaAccumulator::Options _accumulatorOptions;

    // Accumulated size/count deltas currently being flushed.
    boost::optional<OplogScanResult> _inFlight;
    // The record ID of the last record consumed into _pending. The boost::none state means we have
    // not buffered any oplog entries yet.
    boost::optional<RecordId> _lastBufferedRid = boost::none;

    mutable std::mutex _mutex;
    // Accumulated size/count deltas buffered but not checked out. See checkoutForFlush().
    boost::optional<StreamingOplogDeltaAccumulator> _pending;
};

}  // namespace mongo::replicated_fast_count
