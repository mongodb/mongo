// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/modules.h"

#include <deque>
#include <mutex>

#include <boost/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

// Keep "milestones" against the oplog to efficiently remove the old records when the collection
// grows beyond its desired maximum size.
class [[MONGO_MOD_OPEN]] OplogTruncateMarkers : public CollectionTruncateMarkers {
public:
    OplogTruncateMarkers(std::deque<CollectionTruncateMarkers::Marker>&& markers,
                         int64_t partialMarkerRecords,
                         int64_t partialMarkerBytes,
                         int64_t minBytesPerMarker,
                         Microseconds totalTimeSpentBuilding,
                         CollectionTruncateMarkers::MarkersCreationMethod creationMethod,
                         bool initialSamplingFinished,
                         const RecordStore::Oplog& oplog);

    struct InitialSetOfOplogMarkers : public CollectionTruncateMarkers::InitialSetOfMarkers {
        int64_t minBytesPerTruncateMarker;
        bool initialSamplingFinished;
    };

    OplogTruncateMarkers(InitialSetOfOplogMarkers&& initialMarkers,
                         const RecordStore::Oplog& oplog);

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
     * The time after which oplog is retained by policy.
     */
    static Date_t newestExpiredWallTime(OperationContext* opCtx);

    /**
     * Waits for excess oplog space to be available for reclamation.
     * Returns true if we can proceed to reclaim space in the oplog.
     * Otherwise, returns false if the containing record store instance is being destroyed
     * or if we reached the deadline for waiting.
     * Throws exception if interrupted.
     * See 'oplogTruncationCheckPeriodSeconds' server parameter.
     */
    bool awaitHasExcessMarkersOrDead(OperationContext* opCtx) override;

    /**
     * Waits for expired oplog entries to be eligible for reclamation by time-based retention only.
     * Returns true if we can proceed to reclaim space in the oplog by time.
     * Otherwise, returns false if the containing record store instance is being destroyed
     * or if we reached the deadline for waiting.
     * Throws exception if interrupted.
     * See 'oplogTruncationCheckPeriodSeconds' server parameter.
     */
    bool awaitHasExpiredOplogOrDead(OperationContext* opCtx, RecordStore& rs);

    // Clears all the markers of the instance whenever the current WUOW commits.
    void clearMarkersOnCommit(OperationContext* opCtx);

    // Updates the metadata about the collection markers after a rollback occurs.
    void updateMarkersAfterCappedTruncateAfter(int64_t recordsRemoved,
                                               int64_t bytesRemoved,
                                               const RecordId& firstRemovedId);

    // Resize oplog size
    void adjust(RecordStore& rs);

    // The start point of where to truncate next. Used by the background reclaim thread to
    // efficiently truncate records with WiredTiger by skipping over tombstones, etc.
    RecordId firstRecord;

    static std::shared_ptr<OplogTruncateMarkers> createEmptyOplogTruncateMarkers(RecordStore& rs);

    /*
     * Initialize truncation marker creation. This may either create all the truncation markers
     * synchronously, or initialize async sampling, depending on what creation method is chosen.
     * estimatedOplogSize should be an estimate of oplog size at steady-state, to use in marker
     * sizing calculations.
     */
    static InitialSetOfOplogMarkers beginMarkerCreation(OperationContext* opCtx,
                                                        RecordStore& rs,
                                                        int64_t estimatedOplogSize);

    static std::shared_ptr<OplogTruncateMarkers> createOplogTruncateMarkers(OperationContext* opCtx,
                                                                            RecordStore& rs);

    /**
     * Return estimated size of oplog to use for truncation marker sizing calculations.
     * The standard implementation uses the oplog's max size.
     */
    static int64_t estimateOplogSize(RecordStore& rs);

protected:
    virtual int64_t getEstimatedOplogSize(RecordStore& rs) const {
        return estimateOplogSize(rs);
    }

    /**
     * Computes and sets minBytesPerMarker based on the oplog's current configuration
     */
    void recomputeMinBytesPerMarker(RecordStore& rs);

    void _notifyNewMarkerCreation() override {
        _reclaimCv.notify_all();
    }

private:
    bool _hasExcessMarkers(OperationContext* opCtx) const final;

    std::mutex _reclaimMutex;
    stdx::condition_variable _reclaimCv;

    // True if '_rs' has been destroyed, e.g. due to repairDatabase being called on the collection's
    // database, and false otherwise.
    bool _isDead = false;

    const RecordStore::Oplog& _oplog;
};

}  // namespace mongo
