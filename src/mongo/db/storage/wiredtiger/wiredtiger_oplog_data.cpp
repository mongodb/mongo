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

#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_data.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_truncate_markers.h"

namespace mongo {

WiredTigerOplogData::WiredTigerOplogData(const WiredTigerRecordStore::Oplog::Params& params)
    : _maxSize(params.oplogMaxSize) {
    invariant(_maxSize.load());
}

void WiredTigerOplogData::getTruncateStats(BSONObjBuilder& builder) const {
    if (_truncateMarkers) {
        _truncateMarkers->getOplogTruncateMarkersStats(builder);
    }
    builder.append("totalTimeTruncatingMicros", _totalTimeTruncating.load());
    builder.append("truncateCount", _truncateCount.load());
}

std::shared_ptr<WiredTigerOplogTruncateMarkers> WiredTigerOplogData::getTruncateMarkers() const {
    return _truncateMarkers;
}

void WiredTigerOplogData::setTruncateMarkers(
    std::shared_ptr<WiredTigerOplogTruncateMarkers> markers) {
    _truncateMarkers = std::move(markers);
}

int64_t WiredTigerOplogData::getMaxSize() const {
    return _maxSize.load();
}

AtomicWord<Timestamp>& WiredTigerOplogData::getFirstRecordTimestamp() {
    return _firstRecordTimestamp;
}

Status WiredTigerOplogData::updateSize(int64_t newSize) {
    invariant(_maxSize.load());

    if (_maxSize.load() == newSize) {
        return Status::OK();
    }

    _maxSize.store(newSize);

    invariant(_truncateMarkers);
    _truncateMarkers->adjust(newSize);
    return Status::OK();
}


void WiredTigerOplogData::trackTruncateCompletion(int64_t micros) {
    _totalTimeTruncating.fetchAndAdd(micros);
    _truncateCount.fetchAndAdd(1);
}


}  // namespace mongo
