// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/change_stream_pre_images_truncate_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <cstdint>

namespace mongo {
/**
 * Manages the lifecycle of the change stream pre-images collection(s). Also is responsible for
 * inserting the pre-images into the pre-images collection.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ChangeStreamPreImagesCollectionManager {
public:
    struct PurgingJobStats {
        /**
         * Total number of deletion passes completed by the purging job.
         */
        Atomic<int64_t> totalPass;

        /**
         * Cumulative number of pre-image documents deleted by the purging job.
         * This number is an estimate based on the collection count estimates present in the
         * 'CollectionTruncateMarker's, and initially is only as accurate as the initial size/count
         * information for the collection is.
         */
        Atomic<int64_t> docsDeleted;

        /**
         * Cumulative size in bytes of all deleted documents from all pre-image collections by the
         * purging job.
         * This number is an estimate based on the collection size estimates present in the
         * 'CollectionTruncateMarker's, and initially is only as accurate as the initial size/count
         * information for the collection is.
         */
        Atomic<int64_t> bytesDeleted;

        /**
         * Cumulative number of pre-image collections scanned by the purging job. As
         * system.preimages is the only collection that is scanned during purging, this counter will
         * be incremented by one on every purge job invocation.
         */
        Atomic<int64_t> scannedCollections;

        /**
         * Cumulative number of internal pre-image collections scanned by the purging job. Internal
         * collections are the segments of actual pre-images of collections within system.preimages.
         */
        Atomic<int64_t> scannedInternalCollections;

        /**
         * Cumulative number of milliseconds elapsed since the first pass by the purging job.
         */
        Atomic<int64_t> timeElapsedMillis;

        /**
         * The wall time of the pre-image with the highest 'operationTime' which has been truncated.
         */
        Atomic<Date_t> maxStartWallTime;

        /**
         * The maximum timestamp expired pre-images, in the most recent pass, could have to be
         * eligible for truncate.
         */
        Atomic<Timestamp> maxTimestampEligibleForTruncate;

        /**
         * Serializes the purging job statistics to the BSON object.
         */
        BSONObj toBSON() const;
    };

    ChangeStreamPreImagesCollectionManager();

    ~ChangeStreamPreImagesCollectionManager() = default;

    /**
     * Gets the instance of the class using the service context.
     */
    static ChangeStreamPreImagesCollectionManager& get(ServiceContext* service);

    /**
     * Gets the instance of the class using the operation context.
     */
    static ChangeStreamPreImagesCollectionManager& get(OperationContext* opCtx);

    /**
     * Creates the pre-images collection, clustered by the primary key '_id'.
     */
    void createPreImagesCollection(OperationContext* opCtx);

    /**
     * Inserts the document into the pre-images collection.
     */
    void insertPreImage(OperationContext* opCtx, const ChangeStreamPreImage& preImage);

    /**
     * Scans the system pre-images collection and deletes the expired pre-images from it.
     */
    void performExpiredChangeStreamPreImagesRemovalPass(Client* client,
                                                        bool useReplicatedTruncates);

    const PurgingJobStats& getPurgingJobStats() const {
        return _purgingJobStats;
    }

    int64_t getDocsInserted() const {
        return _docsInserted.loadRelaxed();
    }

    void flushTruncateMarkers();

    /**
     * Returns a reference to the initial marker creation statistics of the truncate manager.
     * Does not need to be thread-safe as the underlying metrics in the statistics are atomics.
     */
    const PreImagesTruncateManager::MarkerCreationStats& getMarkerCreationStats() const {
        return _truncateManager.getMarkerCreationStats();
    }

private:
    /**
     * Scans the 'config.system.preimages' collection and deletes the expired pre-images from it.
     *
     * Pre-images are ordered by collection UUID, ie. if UUID of collection A is ordered before UUID
     * of collection B, then pre-images of collection A will be stored before pre-images of
     * collection B.
     *
     * Pre-images are considered expired based on expiration parameter. In case when expiration
     * parameter is not set a pre-image is considered expired if its timestamp is smaller than the
     * timestamp of the earliest oplog entry. In case when expiration parameter is specified, aside
     * from timestamp check a check on the wall clock time of the pre-image recording
     * ('operationTime') is performed. If the difference between 'currentTimeForTimeBasedExpiration'
     * and 'operationTime' is larger than expiration parameter, the pre-image is considered expired.
     * One of those two conditions must be true for a pre-image to be eligible for deletion.
     *
     *                               +-------------------------+
     *                               | config.system.preimages |
     *                               +------------+------------+
     *                                            |
     *             +--------------------+---------+---------+-----------------------+
     *             |                    |                   |                       |
     * +-----------+-------+ +----------+--------+ +--------+----------+ +----------+--------+
     * |  collA.preImageA  | |  collA.preImageB  | |  collB.preImageC  | |  collB.preImageD  |
     * +-----------+-------+ +----------+--------+ +---------+---------+ +----------+--------+
     * |   timestamp: 1    | |   timestamp: 10   | |   timestamp: 5    | |   timestamp: 9    |
     * |   applyIndex: 0   | |   applyIndex: 0   | |   applyIndex: 0   | |   applyIndex: 1   |
     * +-------------------+ +-------------------+ +-------------------+ +-------------------+
     */

    /**
     * Removes expired pre-images through truncation.
     *
     * Lazily initializes truncate markers if they don't already exist, then utilizes the truncate
     * markers to remove expired pre-images from the collection.
     *
     * Returns the estimated number of pre-image documents removed.
     */
    size_t _deleteExpiredPreImagesWithTruncate(OperationContext* opCtx,
                                               bool useReplicatedTruncates);

    PurgingJobStats _purgingJobStats;

    Atomic<int64_t> _docsInserted;

    /**
     * Manages truncate markers.
     */
    PreImagesTruncateManager _truncateManager;
};
}  // namespace mongo
