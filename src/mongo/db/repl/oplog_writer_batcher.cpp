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

#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/duration.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

namespace {

// The default batch limits used for steady state replication.
const OplogWriterBatcher::BatchLimits defaultBatchLimits{
    BSONObjMaxInternalSize,      // 16MB + margin
    BSONObjMaxInternalSize * 2,  // 32MB + margin
    5 * 1000                     // default number of replBatchLimitOperations
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
    boost::optional<long long> termWhenExhausted;
    auto now = opCtx->fastClockSource().now();
    auto delaySecsLatestTimestamp = _calculateSecondaryDelaySecsLatestTimestamp(opCtx, now);
    auto delayMillis = Milliseconds(oplogBatchDelayMillis);
    Date_t waitForDataDeadline = now + maxWaitTime;
    // We expect oplogBatchDelayMillis to be tens of milliseconds so we cap it at
    // maxWaitTime to avoid unexpected behavior. Therefore, secondaryDelaySecs won't be affected by
    // oplogBatchDelayMillis because it will be at least 1s if set so it will override
    // oplogBatchDelayMillis.
    Date_t batchDeadline = delayMillis > Milliseconds(0)
        ? now + std::min(delayMillis, duration_cast<Milliseconds>(maxWaitTime))
        : Date_t();

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
            // Once the total bytes is between 16MB and 32MB, or the total ops reach the
            // limit, we return it as a writer batch. This may not be optimistic on size
            // but we can avoid waiting the next batch coming before deciding whether we
            // can return. This also means the maxCount is just a soft limit because the
            // total ops can exceed it.
            if (totalBytes > batchLimits.minBytes || totalOps >= batchLimits.maxCount) {
                break;
            }
        }

        if (!batches.empty()) {
            if (batchDeadline != Date_t() && totalBytes <= batchLimits.minBytes) {
                waitForDataDeadline = batchDeadline;
                LOGV2_DEBUG(8663100,
                            3,
                            "Waiting for batch to fill",
                            "deadline"_attr = waitForDataDeadline,
                            "delayMillis"_attr = delayMillis,
                            "totalBytes"_attr = totalBytes);
            } else {
                break;
            }
        }

        if (!_waitForData(opCtx, waitForDataDeadline)) {
            break;
        }
    }

    // We can't wait for any data from the buffer, return an empty batch.
    if (batches.empty()) {
        OplogWriterBatch batch;
        if (auto term = _isBufferExhausted(opCtx)) {
            LOGV2(8938402, "Oplog writer buffer has been drained", "term"_attr = term);
            batch.setTermWhenExhausted(*term);
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
        auto currentEntryTime = Date_t::fromDurationSinceEpoch(
            Seconds(lastEntry.getField(OplogEntry::kTimestampFieldName).timestamp().getSecs()));

        // Find the latest oplog entry that satisfies the delaySecsLatestTimestamp.
        auto oplogBatchVector = batch->getBatch();
        auto latestEntryBeforeDelaySecsIndex = static_cast<int>(batch->count()) - 1;
        size_t originalBatchByteSize = batch->byteSize();
        size_t nextBatchByteSize = originalBatchByteSize;
        while (currentEntryTime > *delaySecsLatestTimestamp &&
               latestEntryBeforeDelaySecsIndex > 0) {
            latestEntryBeforeDelaySecsIndex -= 1;
            auto currentEntry = oplogBatchVector[latestEntryBeforeDelaySecsIndex];

            currentEntryTime = Date_t::fromDurationSinceEpoch(Seconds(
                currentEntry.getField(OplogEntry::kTimestampFieldName).timestamp().getSecs()));

            nextBatchByteSize -= currentEntry.objsize();
        }

        // If the entire batch doesn't satisfy the delaySecsLatestTimestamp, then we will stash this
        // batch for next iteration.
        if (latestEntryBeforeDelaySecsIndex == 0 && currentEntryTime > *delaySecsLatestTimestamp) {
            _stashedBatch = std::move(*batch);
            return false;
        }

        // Set batch to the first part of the batch that satisfies the delaySecsLatestTimestamp.
        auto batchVectorToReturn =
            std::vector<BSONObj>(oplogBatchVector.begin(),
                                 oplogBatchVector.begin() + latestEntryBeforeDelaySecsIndex + 1);
        *batch = OplogWriterBatch(batchVectorToReturn, nextBatchByteSize);

        // Set _stashedBatch to the rest of the batch that the secondary still needs to wait for. If
        // the entire batch satisfies the delay, set _stashedBatch to boost::none so that next time
        // it pops from the oplog buffer.
        auto stashedBatchVector = std::vector<BSONObj>(
            oplogBatchVector.begin() + latestEntryBeforeDelaySecsIndex + 1, oplogBatchVector.end());
        if (stashedBatchVector.empty()) {
            _stashedBatch = boost::none;
        } else {
            _stashedBatch =
                OplogWriterBatch(stashedBatchVector, originalBatchByteSize - nextBatchByteSize);
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

bool OplogWriterBatcher::_waitForData(OperationContext* opCtx, Date_t waitDeadline) {
    // If there is a stashedBatch, meaning we only have this batch and it is not passing
    // secondaryDelaySecs yet, so we wait 1s here and return an empty batch to the caller of this
    // batcher.
    if (_stashedBatch) {
        sleepsecs(1);
        return false;
    }

    try {
        if (_oplogBuffer->waitForDataUntil(waitDeadline, opCtx)) {
            return true;
        }
    } catch (const ExceptionFor<ErrorCategory::CancellationError>& e) {
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
boost::optional<Date_t> OplogWriterBatcher::_calculateSecondaryDelaySecsLatestTimestamp(
    OperationContext* opCtx, Date_t now) {
    auto secondaryDelaySecs = ReplicationCoordinator::get(opCtx)->getSecondaryDelaySecs();
    if (secondaryDelaySecs <= Seconds(0)) {
        return {};
    }
    return now - secondaryDelaySecs;
}

boost::optional<long long> OplogWriterBatcher::_isBufferExhausted(OperationContext* opCtx) {
    // Store the current term. It's checked in signalWriterDrainComplete() to detect if
    // the node has stepped down and stepped back up again. See the declaration of
    // signalWriterDrainComplete() for more details.
    auto replCoord = ReplicationCoordinator::get(opCtx);
    auto termWhenExhausted = replCoord->getTerm();
    auto syncState = replCoord->getOplogSyncState();

    // Draining state guarantees the producer has already been fully stopped and no more
    // operations will be pushed in to the oplog buffer until the OplogSyncState changes.
    auto isDraining = syncState == ReplicationCoordinator::OplogSyncState::WriterDraining;

    if (isDraining && _oplogBuffer->isEmpty()) {
        return termWhenExhausted;
    }

    return {};
}

}  // namespace repl
}  // namespace mongo
