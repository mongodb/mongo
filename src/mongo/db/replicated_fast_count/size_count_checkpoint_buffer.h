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
     */
    explicit SizeCountCheckpointBuffer(UUID oplogUuid);

    /**
     * Consumer side. Cuts everything accumulated since the last successful flush into an in-flight
     * batch and returns it, or returns the existing in-flight batch if a prior flush has not yet
     * been acknowledged (retry). Returns boost::none when there is no real work to flush.
     *
     * Acquires the buffer mutex, so it blocks on any in-progress scans.
     */
    boost::optional<OplogScanResult> checkoutForFlush();

    /**
     * Producer side. Scans `cursor` forward to the no-holes EOF, accumulating every record into the
     * long-lived pending accumulator. Returns the RecordId of the last record seen, or boost::none
     * if no records were scanned.
     *
     * Holds the buffer mutex for the duration of the scan so that a concurrent `checkoutForFlush()`
     * cannot observe a partially-applied scan.
     */
    boost::optional<RecordId> scanToNoHolesEOF(SeekableRecordCursor& cursor);

    /**
     * Producer side. Accumulates a single, already-fetched record into the pending accumulator.
     * Useful when a record must be consumed without a cursor.
     */
    void accumulate(const Record& rec);

    /**
     * Acknowledges that the _inFlight result has been persisted and is safe to clear.
     */
    void acknowledgeFlushSuccess();

    /**
     * There is an outstanding result that hasn't been flushed yet.
     */
    bool hasInFlightWork() const;

    /**
     * Returns whether there are new scan results to be retrieved via checkoutForFlush().
     */
    bool hasPendingWork() const;

private:
    // The options used to build every pending accumulator.
    const StreamingOplogDeltaAccumulator::Options _accumulatorOptions;

    mutable std::mutex _mutex;
    // Accumulated size/count deltas buffered but not checked out. See checkoutForFlush().
    boost::optional<StreamingOplogDeltaAccumulator> _pending;
    // Accumulated size/count deltas currently being flushed.
    boost::optional<OplogScanResult> _inFlight;
};

}  // namespace mongo::replicated_fast_count
