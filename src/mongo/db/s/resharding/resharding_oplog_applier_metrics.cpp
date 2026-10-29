// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_oplog_applier_metrics.h"

#include <boost/optional/optional.hpp>

namespace mongo {

ReshardingOplogApplierMetrics::ReshardingOplogApplierMetrics(
    ShardId donorShardId,
    ReshardingMetrics* metrics,
    boost::optional<ReshardingOplogApplierProgress> progressDoc)
    : _donorShardId(donorShardId), _metrics(metrics) {
    if (progressDoc) {
        _insertsApplied.store(progressDoc->getInsertsApplied());
        _updatesApplied.store(progressDoc->getUpdatesApplied());
        _deletesApplied.store(progressDoc->getDeletesApplied());
        _writesToStashCollections.store(progressDoc->getWritesToStashCollections());
    }
}

void ReshardingOplogApplierMetrics::onInsertApplied() {
    _insertsApplied.fetchAndAdd(1);
    _metrics->onInsertApplied();
}

void ReshardingOplogApplierMetrics::onUpdateApplied() {
    _updatesApplied.fetchAndAdd(1);
    _metrics->onUpdateApplied();
}

void ReshardingOplogApplierMetrics::onDeleteApplied() {
    _deletesApplied.fetchAndAdd(1);
    _metrics->onDeleteApplied();
}

void ReshardingOplogApplierMetrics::onBatchRetrievedDuringOplogApplying(Milliseconds elapsed) {
    _metrics->onBatchRetrievedDuringOplogApplying(elapsed);
}

void ReshardingOplogApplierMetrics::onOplogLocalBatchApplied(Milliseconds elapsed) {
    _metrics->onOplogLocalBatchApplied(elapsed);
}

void ReshardingOplogApplierMetrics::onOplogEntriesApplied(int64_t numEntries) {
    _oplogEntriesApplied.fetchAndAdd(numEntries);
    _metrics->onOplogEntriesApplied(numEntries);
}

void ReshardingOplogApplierMetrics::updateAverageTimeToApplyOplogEntries(Milliseconds timeToApply) {
    _metrics->updateAverageTimeToApplyOplogEntries(_donorShardId, timeToApply);
}

void ReshardingOplogApplierMetrics::onWriteToStashCollections() {
    _writesToStashCollections.fetchAndAdd(1);
    _metrics->onWriteToStashedCollections();
}

int64_t ReshardingOplogApplierMetrics::getInsertsApplied() const {
    return _insertsApplied.load();
}

int64_t ReshardingOplogApplierMetrics::getUpdatesApplied() const {
    return _updatesApplied.load();
}

int64_t ReshardingOplogApplierMetrics::getDeletesApplied() const {
    return _deletesApplied.load();
}

int64_t ReshardingOplogApplierMetrics::getOplogEntriesApplied() const {
    return _oplogEntriesApplied.load();
}

int64_t ReshardingOplogApplierMetrics::getWritesToStashCollections() const {
    return _writesToStashCollections.load();
}

}  // namespace mongo
