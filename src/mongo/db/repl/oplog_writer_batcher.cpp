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
const auto kMinWriterBatchSize = 16 * 1024 * 1024;  // 16MB
const auto kMaxWriterBatchSize = 32 * 1024 * 1024;  // 32MB
}  // namespace

OplogWriterBatcher::OplogWriterBatcher(OplogBuffer* oplogBuffer) : _oplogBuffer(oplogBuffer) {}

OplogWriterBatcher::~OplogWriterBatcher() {}

OplogBatchBSONObj OplogWriterBatcher::getNextBatch(OperationContext* opCtx, Seconds maxWaitTime) {
    std::vector<OplogBatchBSONObj> batches;
    OplogBatchBSONObj batch;
    size_t totalBytes = 0;
    size_t totalOps = 0;

    while (true) {
        while (_oplogBuffer->tryPopBatch(opCtx, &batch)) {
            auto batchSize = batch.getByteSize();
            invariant(batchSize <= kMinWriterBatchSize);

            totalBytes += batchSize;
            totalOps += batch.size();
            batches.push_back(std::move(batch));

            // Once the total bytes is between 16MB and 32MB, we return it as a writer batch. This
            // may not be optimistic on size but we can avoid waiting the next batch coming before
            // deciding whether we can return.
            if (totalBytes > kMinWriterBatchSize) {
                invariant(totalBytes <= kMaxWriterBatchSize);
                break;
            }
        }

        if (totalBytes != 0 || !_waitForData(opCtx, maxWaitTime)) {
            break;
        }
    }

    // We can't wait for any data from the buffer, return an empty batch.
    if (batches.empty()) {
        return OplogBatchBSONObj();
    }

    auto finalBatch = _mergeBatches(batches, totalBytes, totalOps);
    _waitSecondaryDelaySecsIfNecessary(opCtx, finalBatch);
    return finalBatch;
}

OplogBatchBSONObj OplogWriterBatcher::_mergeBatches(std::vector<OplogBatchBSONObj>& batches,
                                                    size_t totalBytes,
                                                    size_t totalOps) {
    invariant(!batches.empty());
    // Merge all oplog entries
    std::vector<BSONObj> ops;
    ops.reserve(totalOps);
    for (auto& batch : batches) {
        auto& objs = batch.getBatch();
        std::move(objs.begin(), objs.end(), std::back_inserter(ops));
    }
    return OplogBatchBSONObj(std::move(ops), totalBytes);
}

bool OplogWriterBatcher::_waitForData(OperationContext* opCtx, Seconds maxWaitTime) {
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

void OplogWriterBatcher::_waitSecondaryDelaySecsIfNecessary(OperationContext* opCtx,
                                                            const OplogBatchBSONObj& batch) {
    invariant(!batch.empty());
    auto& lastEntry = batch.back();
    auto entryTime = Date_t::fromDurationSinceEpoch(
        Seconds(lastEntry.getField(OplogEntry::kTimestampFieldName).timestamp().getSecs()));
    boost::optional<Date_t> secondaryDelaySecsLatestTimestamp = boost::none;
    while ((secondaryDelaySecsLatestTimestamp = _calculateSecondaryDelaySecsLatestTimestamp())) {
        // See if the last entry has passed secondaryDelaySecs, which means all entries in
        // this batch has passed secondaryDelaySecs. This could cause earlier entries in the
        // same batch got delayed longer but that only happens in a rare case and only in
        // one batch.
        if (entryTime <= *secondaryDelaySecsLatestTimestamp) {
            return;
        }

        try {
            opCtx->sleepFor(Seconds(1));
        } catch (const ExceptionForCat<ErrorCategory::CancellationError>& ex) {
            LOGV2(8602601,
                  "Interrupted when waiting for secondaryDelaySecs, return a batch "
                  "before secondaryDelaySecs is met.",
                  "error"_attr = ex.toString());
        }
    }
}

}  // namespace repl
}  // namespace mongo
