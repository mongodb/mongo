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

#include "mongo/db/replicated_fast_count/size_count_checkpoint_buffer.h"

namespace mongo::replicated_fast_count {

SizeCountCheckpointBuffer::SizeCountCheckpointBuffer(UUID oplogUuid)
    : _accumulatorOptions{.isCheckpoint = true, .oplogUuid = oplogUuid} {
    _pending.emplace(_accumulatorOptions);
}

boost::optional<OplogScanResult> SizeCountCheckpointBuffer::checkoutForFlush() {
    std::lock_guard lk(_mutex);

    if (_inFlight) {
        // A previous flush has not been acknowledged yet. Retry the same batch.
        return _inFlight;
    }

    // Cut the _pending accumulator into a flushable batch and reset _pending.
    OplogScanResult result = _pending->finish();
    _pending.emplace(_accumulatorOptions);

    if (!result.lastTimestamp) {
        // finish() leaves lastTimestamp unset when no size/count entries were accumulated. We
        // return early so we do not advance the checkpoint for untracked entries.
        return boost::none;
    }

    _inFlight = std::move(result);
    return _inFlight;
}

boost::optional<RecordId> SizeCountCheckpointBuffer::scanToNoHolesEOF(
    SeekableRecordCursor& cursor) {
    std::lock_guard lk(_mutex);

    boost::optional<RecordId> lastSeenRid;
    while (const auto rec = cursor.next()) {
        _pending->consumeRecord(*rec);
        lastSeenRid = rec->id;
    }
    return lastSeenRid;
}

void SizeCountCheckpointBuffer::accumulate(const Record& rec) {
    std::lock_guard lk(_mutex);
    _pending->consumeRecord(rec);
}

void SizeCountCheckpointBuffer::acknowledgeFlushSuccess() {
    std::lock_guard lk(_mutex);
    _inFlight.reset();
}

bool SizeCountCheckpointBuffer::hasInFlightWork() const {
    std::lock_guard lk(_mutex);
    return _inFlight.has_value();
}

bool SizeCountCheckpointBuffer::hasPendingWork() const {
    std::lock_guard lk(_mutex);
    return _pending->hasPendingWork();
}

}  // namespace mongo::replicated_fast_count
