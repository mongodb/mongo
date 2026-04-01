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

#pragma once

#include "mongo/db/change_stream_pre_images_truncate_markers_sampling_strategy/sampling_strategy.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstdint>

#include <boost/optional/optional.hpp>

namespace mongo {
/**
 * Statistics for a truncate pass over the system's pre-images collection.
 */
struct PreImagesTruncateStats {
    // The estimated number of bytes deleted in the truncate pass.
    // This number is an estimate based on the collection size estimates present in the collection
    // truncate markers, and it is only as accurate as the size/count information in the
    // 'CollectionTruncateMarker's is.
    int64_t bytesDeleted{0};

    // The estimated number of documents deleted in the truncate pass.
    // This number is an estimate based on the collection count estimates present in the collection
    // truncate markers, and it is only as accurate as the size/count information in the
    // 'CollectionTruncateMarker's is.
    int64_t docsDeleted{0};

    // The number of 'nsUUID's scanned in the truncate pass.
    int64_t scannedInternalCollections{0};

    // Instantaneous maximum timestamp eligible for truncation. Expired documents will only be
    // truncated when their timestamp is less than or equal to it.
    Timestamp maxTimestampEligibleForTruncate;

    // The maximum wall time from the pre-images truncated across the collection.
    Date_t maxStartWallTime{};
};

/**
 * Manages truncate markers specific to the system's pre-images collection.
 */
class PreImagesTruncateMarkers {
    PreImagesTruncateMarkers(const PreImagesTruncateMarkers&) = delete;
    PreImagesTruncateMarkers& operator=(const PreImagesTruncateMarkers&) = delete;

public:
    /**
     * Creates a 'PreImagesTruncateMarkers' instance populated with truncate markers that span the
     * system's pre-images collection. However, these markers are not safe to use until
     * 'refreshMarkers' is called to update the highest seen recordId and wall time for each
     * nsUUID. Otherwise, pre-images would not be truncated until new inserts come in for each
     * nsUUID.
     *
     * Note: Pre-images inserted concurrently with creation might not be covered by the resulting
     * truncate markers.
     *
     * The 'preImagesCollection' is required to exist. Otherwise the constructor will trigger an
     * invariant failure. The 'OperationContext' parameter is only used for the initial population
     * of the truncate markers, but is not used afterwards.
     */
    PreImagesTruncateMarkers(OperationContext* opCtx,
                             const CollectionAcquisition& preImagesCollection);

    /**
     * Opens a fresh snapshot and ensures the all pre-images visible in the snapshot are
     * covered by truncate markers.
     *
     * Required to ensure truncate markers are viable for truncation.
     */
    void refreshMarkers(OperationContext* opCtx);

    /**
     * Performs the truncation of the change stream pre-images collection based on the in-memory
     * state of the 'CollectionTruncateMarkers' in '_markersMap'.
     */
    PreImagesTruncateStats truncateExpiredPreImages(OperationContext* opCtx,
                                                    bool useReplicatedTruncates);

    /**
     * Updates or creates the 'PreImagesTruncateMarkersPerNsUUID' to account for a
     * newly inserted pre-image generated from the user's collection with UUID 'nsUUID'.
     *
     * 'numRecords' should always be 1 except for during initialization.
     *
     * Callers are responsible for calling this only once the data inserted is committed.
     */
    void updateOnInsert(const RecordId& recordId,
                        const UUID& nsUUID,
                        Date_t wallTime,
                        int64_t bytesInserted,
                        int64_t numRecords = 1);

    UUID getPreImagesCollectionUUID() const {
        return _preImagesCollectionUUID;
    }

    /**
     * Returns the number of sampled user collection ranges inside the pre-images collection.
     */
    int64_t getNumberOfSampledCollections() const;

    const pre_image_marker_initialization_internal::MarkersMap& getMarkersMap_forTest() const {
        return _markersMap;
    }

private:
    const UUID _preImagesCollectionUUID;

    /**
     * The pre-images collection spans pre-images generated across all pre-image enabled
     * collections. The pre-images collection is sorted so that all pre-images from the same
     * 'nsUUID' are stored consecutively. There is a separate set of truncate markers for each
     * 'nsUUID'.
     *
     * Maps pre-images of a given 'nsUUID' to their truncate markers.
     */
    pre_image_marker_initialization_internal::MarkersMap _markersMap;
};

}  // namespace mongo
