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

#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"

namespace mongo {

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

    /**
     * Whether the instance is going to get destroyed.
     */
    bool isDead();

    /**
     * Mark this instance as serving a non-existent RecordStore. This is the case if either the
     * RecordStore has been deleted or we're shutting down. Doing this will mark the instance as
     * ready for destruction.
     */
    void kill();

    /**
     * Waits for excess oplog space to be available for reclamation.
     * Returns true if we can proceed to reclaim space in the oplog.
     * Otherwise, returns false if the containing record store instance is being destroyed
     * or if we reached the deadline for waiting.
     * Throws exception if interrupted.
     * See 'oplogTruncationCheckPeriodSeconds' server parameter.
     */
    bool awaitHasExcessMarkersOrDead(OperationContext* opCtx) override;

    // Clears all the markers of the instance whenever the current WUOW commits.
    void clearMarkersOnCommit(OperationContext* opCtx);

    // Updates the metadata about the collection markers after a rollback occurs.
    void updateMarkersAfterCappedTruncateAfter(int64_t recordsRemoved,
                                               int64_t bytesRemoved,
                                               const RecordId& firstRemovedId);

    void getOplogTruncateMarkersStats(BSONObjBuilder& builder) const;

    // Resize oplog size
    void adjust(OperationContext* opCtx, int64_t maxSize);

    // The start point of where to truncate next. Used by the background reclaim thread to
    // efficiently truncate records with WiredTiger by skipping over tombstones, etc.
    RecordId firstRecord;

    static std::shared_ptr<OplogTruncateMarkers> createEmptyOplogTruncateMarkers(
        WiredTigerRecordStore* rs);

    static std::shared_ptr<OplogTruncateMarkers> sampleAndUpdate(OperationContext* opCtx,
                                                                 WiredTigerRecordStore* rs,
                                                                 const NamespaceString& ns);

    static std::shared_ptr<OplogTruncateMarkers> createOplogTruncateMarkers(
        OperationContext* opCtx, WiredTigerRecordStore* rs, const NamespaceString& ns);

    CollectionTruncateMarkers::MarkersCreationMethod getMarkersCreationMethod() const {
        return _creationMethod;
    }

    Microseconds getCreationProcessingTime() const {
        return _totalTimeProcessing;
    }

private:
    bool _hasExcessMarkers(OperationContext* opCtx) const final;

    void _notifyNewMarkerCreation() final {
        _reclaimCv.notify_all();
    }

    Mutex _reclaimMutex = MONGO_MAKE_LATCH("OplogTruncateMarkers::_reclaimMutex");
    stdx::condition_variable _reclaimCv;

    // True if '_rs' has been destroyed, e.g. due to repairDatabase being called on the collection's
    // database, and false otherwise.
    bool _isDead = false;

    WiredTigerRecordStore* _rs;

    Microseconds _totalTimeProcessing;  // Amount of time spent scanning and/or sampling the
                                        // oplog during start up, if any.

    CollectionTruncateMarkers::MarkersCreationMethod _creationMethod;
};

}  // namespace mongo
