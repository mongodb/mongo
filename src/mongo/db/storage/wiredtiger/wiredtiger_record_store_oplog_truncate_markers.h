/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/optional.hpp>

#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"

namespace mongo {

class OperationContext;
class RecordId;

// Keep "milestones" against the oplog to efficiently remove the old records when the collection
// grows beyond its desired maximum size.
class WiredTigerRecordStore::OplogTruncateMarkers final : public CollectionTruncateMarkers {
public:
    OplogTruncateMarkers(std::deque<CollectionTruncateMarkers::Marker> markers,
                         int64_t partialMarkerRecords,
                         int64_t partialMarkerBytes,
                         int64_t minBytesPerMarker,
                         Microseconds totalTimeSpentBuilding,
                         CollectionTruncateMarkers::MarkersCreationMethod creationMethod,
                         WiredTigerRecordStore* rs);

    void getOplogTruncateMarkersStats(BSONObjBuilder& builder) const {
        builder.append("totalTimeProcessingMicros", _totalTimeProcessing.count());
        builder.append("processingMethod", _processBySampling ? "sampling" : "scanning");
        if (auto oplogMinRetentionHours = storageGlobalParams.oplogMinRetentionHours.load()) {
            builder.append("oplogMinRetentionHours", oplogMinRetentionHours);
        }
    }

    // Resize oplog size
    void adjust(OperationContext* opCtx, int64_t maxSize);

    // The start point of where to truncate next. Used by the background reclaim thread to
    // efficiently truncate records with WiredTiger by skipping over tombstones, etc.
    RecordId firstRecord;

    static WiredTigerRecordStore::OplogTruncateMarkers createOplogTruncateMarkers(
        OperationContext* opCtx, WiredTigerRecordStore* rs, const NamespaceString& ns);
    //
    // The following methods are public only for use in tests.
    //

    bool processedBySampling() const {
        return _processBySampling;
    }

private:
    virtual bool _hasExcessMarkers(OperationContext* opCtx) const final;

    WiredTigerRecordStore* _rs;

    Microseconds _totalTimeProcessing;  // Amount of time spent scanning and/or sampling the
                                        // oplog during start up, if any.
    bool _processBySampling;            // Whether the oplog was sampled or scanned.
};

}  // namespace mongo
