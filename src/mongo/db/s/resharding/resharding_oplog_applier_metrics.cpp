/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_oplog_applier_metrics.h"

namespace mongo {

ReshardingOplogApplierMetrics::ReshardingOplogApplierMetrics(
    ReshardingMetrics* metrics, boost::optional<ReshardingOplogApplierProgress> progressDoc)
    : _metrics(metrics) {
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
