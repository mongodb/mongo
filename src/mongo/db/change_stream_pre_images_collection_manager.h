/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/change_stream_pre_images_truncate_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <cstdint>

#include <boost/optional/optional.hpp>

namespace mongo {
/**
 * Manages the lifecycle of the change stream pre-images collection(s). Also is responsible for
 * inserting the pre-images into the pre-images collection.
 */
class ChangeStreamPreImagesCollectionManager {
public:
    struct PurgingJobStats {
        /**
         * Total number of deletion passes completed by the purging job.
         */
        AtomicWord<int64_t> totalPass;

        /**
         * Cumulative number of pre-image documents deleted by the purging job.
         */
        AtomicWord<int64_t> docsDeleted;

        /**
         * Cumulative size in bytes of all deleted documents from all pre-image collections by the
         * purging job.
         */
        AtomicWord<int64_t> bytesDeleted;

        /**
         * Cumulative number of pre-image collections scanned by the purging job. In single-tenant
         * environments this is the same as totalPass as there is 1 pre-image collection per tenant.
         */
        AtomicWord<int64_t> scannedCollections;

        /**
         * Cumulative number of internal pre-image collections scanned by the purging job. Internal
         * collections are the segments of actual pre-images of collections within system.preimages.
         */
        AtomicWord<int64_t> scannedInternalCollections;

        /**
         * Cumulative number of milliseconds elapsed since the first pass by the purging job.
         */
        AtomicWord<int64_t> timeElapsedMillis;

        /**
         * The wall time of the pre-image with the highest 'operationTime' which has been truncated.
         */
        AtomicWord<Date_t> maxStartWallTime;

        /**
         * The maximum timestamp expired pre-images, in the most recent pass, could have to be
         * eligible for truncate.
         */
        AtomicWord<Timestamp> maxTimestampEligibleForTruncate;

        /**
         * Serializes the purging job statistics to the BSON object.
         */
        BSONObj toBSON() const;
    };

    explicit ChangeStreamPreImagesCollectionManager() {}

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
    void performExpiredChangeStreamPreImagesRemovalPass(Client* client);

    const PurgingJobStats& getPurgingJobStats() {
        return _purgingJobStats;
    }

    int64_t getDocsInserted() const {
        return _docsInserted.loadRelaxed();
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
     * Returns the number of pre-image documents removed.
     */
    size_t _deleteExpiredPreImagesWithTruncate(OperationContext* opCtx);

    PurgingJobStats _purgingJobStats;

    AtomicWord<int64_t> _docsInserted;

    /**
     * Manages truncate markers.
     */
    PreImagesTruncateManager _truncateManager;
};
}  // namespace mongo
