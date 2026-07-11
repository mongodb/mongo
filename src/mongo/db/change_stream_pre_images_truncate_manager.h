// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/change_stream_pre_images_truncate_markers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

namespace mongo {
/**
 * Manages the truncation of expired pre-images for the pre-images collection.
 */
class PreImagesTruncateManager {
public:
    /**
     * Statistics for initial marker creation.
     */
    struct MarkerCreationStats {
        /**
         * Total number of marker creation passes completed.
         */
        Atomic<int64_t> totalPass;

        /**
         * Cumulative number of pre-image collections scanned for the marker creation.
         */
        Atomic<int64_t> scannedInternalCollections;

        /**
         * Cumulative number of milliseconds elapsed during the marker creation.
         */
        Atomic<int64_t> timeElapsedMillis;

        /**
         * Serializes the marker creation statistics to the BSON object.
         */
        BSONObj toBSON() const;
    };

    /*
     * Truncates expired pre-images spanning the pre-images collection.
     */
    PreImagesTruncateStats truncateExpiredPreImages(OperationContext* opCtx,
                                                    bool useReplicatedTruncates);

    /**
     * Updates truncate markers to account for a newly inserted 'preImage' into the pre-images
     * collection. If no truncate markers have been created for the 'tenantId's pre-images
     * collection, this is a no-op.
     */
    void updateMarkersOnInsert(OperationContext* opCtx,
                               const ChangeStreamPreImage& preImage,
                               int64_t bytesInserted);

    /**
     * Erase any existing in-memory truncate markers, so that they will be rebuilt from scratch next
     * time.
     */
    void flushTruncateMarkers();

    /**
     * Returns a reference to the initial marker creation statistics. Does not need to be
     * thread-safe as the underlying metrics in the statistics are atomics.
     */
    const MarkerCreationStats& getMarkerCreationStats() const {
        return _markerCreationStats;
    }

    /**
     * Returns true if the '_truncateMarkers' instance variable is populated, false otherwise.
     */
    bool areTruncateMarkersPopulated_forTest() const;

    /**
     * Test wrapper for calling '_getInitializedMarkersForPreImagesCollection()' from out of unit
     * tests without making the method part of the public API. See that method's description for
     * more details.
     */
    std::shared_ptr<PreImagesTruncateMarkers> getInitializedMarkersForPreImagesCollection_forTest(
        OperationContext* opCtx) {
        return _getInitializedMarkersForPreImagesCollection(opCtx);
    }

private:
    /**
     * Tries to retrieve truncate markers for the pre-images collection - or initialize the truncate
     * markers if they don't yet exist.
     *
     * Returns a shared_ptr to truncate markers for the pre-images collection. Returns a nullptr if
     * the pre-images collection doesn't exist yet.
     */
    std::shared_ptr<PreImagesTruncateMarkers> _getInitializedMarkersForPreImagesCollection(
        OperationContext* opCtx);

    /**
     * Returns a shared pointer to 'PreImagesTenantMarkers', provided the truncate markers were
     * successfully installed. Otherwise, returns a null pointer.
     */
    std::shared_ptr<PreImagesTruncateMarkers> _createAndInstallMarkers(OperationContext* opCtx);

    /**
     * Returns a shared pointer to the currently installed 'PreImagesTenantMarkers' instance, if
     * any. Can return a nullptr.
     * The shared pointer is retrieved under the RWMutex in shared mode, so access to this function
     * is thread-safe.
     */
    std::shared_ptr<PreImagesTruncateMarkers> _getTruncateMarkers() const;

    /**
     * Install a new value in the 'PreImagesTenantMarkers' instance. The new value can be a nullptr.
     * any.
     * The shared pointer is installed under the RWMutex in exclusive mode, so access to method is
     * thread-safe.
     */
    void _setTruncateMarkers(std::shared_ptr<PreImagesTruncateMarkers> truncateMarkers);

    /**
     * Read-write mutex for accessing '_truncateMarkers'. All loads and stores of the
     * '_truncateMarkers' instance in this class must acquire this mutex in the appropriate mode.
     */
    mutable RWMutex _mutex;

    /**
     * Truncate markers for the pre-images collection. The member is populated lazily and can
     * contain a nullptr. This member is protected by '_mutex' and should never be accessed
     * directly, but only via calls to ' _getTruncateMarkers()' and ' _setTruncateMarkers()'.
     */
    std::shared_ptr<PreImagesTruncateMarkers> _truncateMarkers;

    /**
     * Statistics for initial marker creation. Not protected by '_mutex'.
     */
    MarkerCreationStats _markerCreationStats;
};
}  // namespace mongo
