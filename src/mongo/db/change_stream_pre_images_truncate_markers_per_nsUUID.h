// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <deque>
#include <vector>

/**
 * The pre-images collection contains pre-images for every collection 'nsUUID' with pre-images
 * enabled. The pre-images collection is ordered by collection 'nsUUID', so that pre-images
 * belonging to a given collection are grouped together. Additionally, pre-images for a given
 * collection 'nsUUID' are stored in timestamp order, which makes range truncation possible.
 *
 * Implementation of truncate markers for pre-images associated with a single collection 'nsUUID'
 * within a pre-images collection.
 */
namespace mongo {

class PreImagesTruncateMarkersPerNsUUID final
    : public CollectionTruncateMarkersWithPartialExpiration {
public:
    struct InitialSetOfMarkers {
        std::deque<Marker> markers{};
        RecordId highestRecordId{};
        Date_t highestWallTime{};
        int64_t leftoverRecordsCount{0};
        int64_t leftoverRecordsBytes{0};
        Microseconds timeTaken{0};
        MarkersCreationMethod creationMethod{MarkersCreationMethod::EmptyCollection};
    };

    PreImagesTruncateMarkersPerNsUUID(const UUID& nsUUID,
                                      InitialSetOfMarkers initialSetOfMarkers,
                                      int64_t minBytesPerMarker);

    /**
     * Refreshes the highest seen record for the nsUUID. Does nothing if the highest record is
     * already tracked. Useful to call with a newer snapshot than the one used to originally
     * construct the truncate markers to ensure all new inserts are tracked.
     */
    void refreshHighestTrackedRecord(OperationContext* opCtx,
                                     const CollectionAcquisition& preImagesCollection);

    /**
     * Creates an 'InitialSetOfMarkers' from samples of pre-images with 'nsUUID'. The generated
     * markers are best-effort estimates. They do not guarantee to capture an accurate number of
     * records and bytes corresponding to the 'nsUUID' within the pre-images collection. This is
     * because size metrics are only available for an entire pre-images collection, not individual
     * segments corresponding to the provided 'nsUUID'.
     *
     * Caller is responsible for ensuring 'samples' are in ascending order.
     *
     * Guarantee: The last sample is tracked by the resulting initial set, even if the exact size
     * and records count aren't correct.
     */
    static InitialSetOfMarkers createInitialMarkersFromSamples(
        OperationContext* opCtx,
        const UUID& preImagesCollectionUUID,
        const UUID& nsUUID,
        const std::vector<CollectionTruncateMarkers::RecordIdAndWallTime>& samples,
        int64_t estimatedRecordsPerMarker,
        int64_t estimatedBytesPerMarker,
        uint64_t randomSamplesPerMarker);

    /**
     * Returns an accurate 'InitialSetOfMarkers' corresponding to the segment of the pre-images
     * collection generated from 'nsUUID'.
     */
    static InitialSetOfMarkers createInitialMarkersScanning(OperationContext* opCtx,
                                                            const CollectionAcquisition& collPtr,
                                                            const UUID& nsUUID,
                                                            int64_t minBytesPerMarker);

    static CollectionTruncateMarkers::RecordIdAndWallTime getRecordIdAndWallTime(
        const Record& record);

    static Date_t getWallTime(const BSONObj& doc);

    /**
     * Returns whether there are no more markers and no partial marker pending creation.
     */
    bool isEmpty() const {
        return CollectionTruncateMarkers::isEmpty();
    }

    /**
     * Updates the current set of markers to account for the addition of the record with 'recordId'
     * and its size and expiration metadata.  Unlike the inherited
     * 'updateCurrentMarkerAfterInsertOnCommit()' method, the update is invoked immediately when
     * called. Callers are responsible for managing when the call is executed (inside an
     * 'onCommit()' handler or on its own).
     */
    void updateMarkers(int64_t numBytes,
                       const RecordId& recordId,
                       Date_t wallTime,
                       int64_t numRecords);

private:
    bool _hasExcessMarkers(OperationContext* opCtx) const override;

    bool _hasPartialMarkerExpired(OperationContext* opCtx,
                                  const RecordId& highestSeenRecordId,
                                  const Date_t& highestSeenWallTime) const override;

    const UUID _nsUUID;
};

}  // namespace mongo
