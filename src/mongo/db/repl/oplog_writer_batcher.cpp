/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_writer_batcher.h"

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/duration.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

namespace {

// The default batch limits used for steady state replication.
const OplogWriterBatcher::BatchLimits defaultBatchLimits{
    BSONObjMaxInternalSize,                          // 16MB + margin
    BSONObjMaxInternalSize + BSONObjMaxInternalSize  // 32MB + margin
};

}  // namespace

OplogWriterBatcher::OplogWriterBatcher(OplogBuffer* oplogBuffer) : _oplogBuffer(oplogBuffer) {}

OplogWriterBatcher::~OplogWriterBatcher() {}

OplogWriterBatch OplogWriterBatcher::getNextBatch(OperationContext* opCtx, Seconds maxWaitTime) {
    return getNextBatch(opCtx, maxWaitTime, defaultBatchLimits);
}

OplogWriterBatch OplogWriterBatcher::getNextBatch(OperationContext* opCtx,
                                                  Seconds maxWaitTime,
                                                  const BatchLimits& batchLimits) {
    std::vector<OplogWriterBatch> batches;
    OplogWriterBatch batch;
    size_t totalBytes = 0;
    size_t totalOps = 0;
    bool exhausted = false;
    auto delaySecsLatestTimestamp = _calculateSecondaryDelaySecsLatestTimestamp();

    while (true) {
        while (_pollFromBuffer(opCtx, &batch, delaySecsLatestTimestamp)) {
            auto batchSize = batch.byteSize();
            if (batchSize > BSONObjMaxInternalSize) {
                LOGV2_WARNING(8569805,
                              "Received an oplog batch larger than maximum",
                              "batchSizeBytes"_attr = batchSize,
                              "maxBatchSizeBytes"_attr = BSONObjMaxInternalSize);
            }
            totalBytes += batchSize;
            totalOps += batch.count();
            batches.push_back(std::move(batch));
            // Once the total bytes is between 16MB and 32MB, we return it as a writer batch. This
            // may not be optimistic on size but we can avoid waiting the next batch coming before
            // deciding whether we can return.
            if (totalBytes > batchLimits.minBytes) {
                break;
            }
        }

        if (!batches.empty()) {
            break;
        }

        if (!_waitForData(opCtx, maxWaitTime)) {
            exhausted = _oplogBuffer->inDrainModeAndEmpty();
            break;
        }
    }

    // We can't wait for any data from the buffer, return an empty batch.
    if (batches.empty()) {
        OplogWriterBatch batch;
        if (exhausted) {
            batch.setExhausted();
        }
        return batch;
    }

    if (batches.size() == 1) {
        return std::move(batches.front());
    }

    return _mergeBatches(batches, totalBytes, totalOps);
}

bool OplogWriterBatcher::_pollFromBuffer(OperationContext* opCtx,
                                         OplogWriterBatch* batch,
                                         boost::optional<Date_t>& delaySecsLatestTimestamp) {
    if (_stashedBatch) {
        *batch = std::move(*_stashedBatch);
        _stashedBatch = boost::none;
    } else if (!_oplogBuffer->tryPopBatch(opCtx, batch)) {
        return false;
    }

    if (delaySecsLatestTimestamp) {
        auto& lastEntry = batch->back();
        auto entryTime = Date_t::fromDurationSinceEpoch(
            Seconds(lastEntry.getField(OplogEntry::kTimestampFieldName).timestamp().getSecs()));
        // See if the last entry has passed secondaryDelaySecs, which means all entries in
        // this batch has passed secondaryDelaySecs. This could cause earlier entries in the
        // same batch got delayed longer but that only happens in a rare case and only in
        // one batch.
        if (entryTime > *delaySecsLatestTimestamp) {
            _stashedBatch = std::move(*batch);
            return false;
        }
    }

    return true;
}


OplogWriterBatch OplogWriterBatcher::_mergeBatches(std::vector<OplogWriterBatch>& batches,
                                                   size_t totalBytes,
                                                   size_t totalOps) {
    // Merge all batches into one.
    invariant(batches.size() > 1);

    std::vector<BSONObj> ops;
    ops.reserve(totalOps);

    for (auto& batch : batches) {
        auto& objs = batch.getBatch();
        std::move(objs.begin(), objs.end(), std::back_inserter(ops));
    }

    return OplogWriterBatch(std::move(ops), totalBytes);
}

bool OplogWriterBatcher::_waitForData(OperationContext* opCtx, Seconds maxWaitTime) {
    // If there is a stashedBatch, meaning we only have this batch and it is not passing
    // secondaryDelaySecs yet, so we wait 1s here and return an empty batch to the caller of this
    // batcher.
    if (_stashedBatch) {
        sleepsecs(1);
        return false;
    }

    try {
        if (_oplogBuffer->waitForDataFor(duration_cast<Milliseconds>(maxWaitTime), opCtx)) {
            return true;
        }
    } catch (const ExceptionForCat<ErrorCategory::CancellationError>& e) {
        LOGV2(8569501,
              "Interrupted when waiting for data, return what we have now",
              "error"_attr = e);
    }

    return false;
}

/**
 * If secondaryDelaySecs is enabled, this function calculates the most recent timestamp of any oplog
 * entries that can be be returned in a batch.
 */
boost::optional<Date_t> OplogWriterBatcher::_calculateSecondaryDelaySecsLatestTimestamp() {
    auto service = cc().getServiceContext();
    auto replCoord = ReplicationCoordinator::get(service);
    auto secondaryDelaySecs = replCoord->getSecondaryDelaySecs();
    if (secondaryDelaySecs <= Seconds(0)) {
        return {};
    }
    auto fastClockSource = service->getFastClockSource();
    return fastClockSource->now() - secondaryDelaySecs;
}

}  // namespace repl
}  // namespace mongo
