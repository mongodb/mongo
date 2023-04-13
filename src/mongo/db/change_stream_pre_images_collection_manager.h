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

#include <boost/optional/optional.hpp>

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/background.h"

namespace mongo {
namespace change_stream_pre_image_helpers {
boost::optional<Date_t> getPreImageExpirationTime(OperationContext* opCtx, Date_t currentTime);

Timestamp getPreImageTimestamp(const RecordId& rid);

RecordId toRecordId(ChangeStreamPreImageId id);
}  // namespace change_stream_pre_image_helpers

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
         * Maximum wall time from the first document of each pre-image collection.
         */
        AtomicWord<Date_t> maxStartWallTime;

        /**
         * Serializes the purging job statistics to the BSON object.
         */
        BSONObj toBSON() const;
    };

    /**
     * Creates the pre-images collection, clustered by the primary key '_id'. The collection is
     * created for the specific tenant if the 'tenantId' is specified.
     */
    static void createPreImagesCollection(OperationContext* opCtx,
                                          boost::optional<TenantId> tenantId);

    /**
     * Drops the pre-images collection. The collection is dropped for the specific tenant if
     * the 'tenantId' is specified.
     */
    static void dropPreImagesCollection(OperationContext* opCtx,
                                        boost::optional<TenantId> tenantId);

    /**
     * Inserts the document into the pre-images collection. The document is inserted into the
     * tenant's pre-images collection if the 'tenantId' is specified.
     */
    static void insertPreImage(OperationContext* opCtx,
                               boost::optional<TenantId> tenantId,
                               const ChangeStreamPreImage& preImage);

    /**
     * Scans the system pre-images collection and deletes the expired pre-images from it.
     */
    static void performExpiredChangeStreamPreImagesRemovalPass(Client* client);

    static const PurgingJobStats& getPurgingJobStats();
};
}  // namespace mongo
