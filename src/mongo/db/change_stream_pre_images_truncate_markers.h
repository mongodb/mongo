/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/util/concurrent_shared_values_map.h"

/**
 * There is up to one 'config.system.preimages' collection per tenant. This pre-images
 * collection contains pre-images for every collection 'nsUUID' with pre-images enabled on the
 * tenant. The pre-images collection is ordered by collection 'nsUUID', so that pre-images belonging
 * to a given collection are grouped together. Additionally, pre-images for a given collection
 * 'nsUUID' are stored in timestamp order, which makes range truncation possible.
 *
 * Implementation of truncate markers for pre-images associated with a single collection 'nsUUID'
 * within a pre-images collection.
 */
namespace mongo {

class PreImagesTruncateMarkersPerNsUUID final
    : public CollectionTruncateMarkersWithPartialExpiration {
public:
    PreImagesTruncateMarkersPerNsUUID(boost::optional<TenantId> tenantId,
                                      std::deque<Marker> markers,
                                      int64_t leftoverRecordsCount,
                                      int64_t leftoverRecordsBytes,
                                      int64_t minBytesPerMarker);

    /**
     * Creates an initial set of markers for pre-images from 'nsUUID'.
     */
    static CollectionTruncateMarkers::InitialSetOfMarkers createInitialMarkersScanning(
        OperationContext* opCtx,
        RecordStore* rs,
        const UUID& nsUUID,
        RecordId& highestSeenRecordId,
        Date_t& highestSeenWallTime,
        int64_t minBytesPerMarker);

    static CollectionTruncateMarkers::RecordIdAndWallTime getRecordIdAndWallTime(
        const Record& record);

    /**
     * Returns whether there are no more markers and no partial marker pending creation.
     */
    bool isEmpty() const {
        return CollectionTruncateMarkers::isEmpty();
    }

private:
    friend class PreImagesRemoverTest;

    bool _hasExcessMarkers(OperationContext* opCtx) const override;

    bool _hasPartialMarkerExpired(OperationContext* opCtx) const override;

    /**
     * When initialized, indicates this is a serverless environment.
     */
    boost::optional<TenantId> _tenantId;
};

/**
 * Manages the truncation of expired pre-images for pre-images collection(s). There is up to one
 * "system.config.preimages" pre-images collection per tenant.
 *
 * In a single-tenant environment, there is only one "system.config.preimages" pre-images
 * collection. In which case, the corresponding truncate markers are mapped to TenantId
 * 'boost::none'.
 *
 * Responsible for constructing and managing truncate markers across tenants - and for each tenant,
 * across all 'nsUUID's with pre-images enabled on the tenant.
 */
class PreImagesTruncateManager {
public:
    /**
     * Statistcs for a truncate pass over a given tenant's pre-images collection.
     */
    struct TruncateStats {
        int64_t bytesDeleted{0};
        int64_t docsDeleted{0};

        // The number of 'nsUUID's scanned in the truncate pass.
        int64_t scannedInternalCollections{0};

        // The maximum wall time from the pre-images truncated across the collection.
        Date_t maxStartWallTime{};
    };

    /**
     * If truncate markers do not exist for 'tenantId', create the initial set of markers by
     * sampling or scanning records in the collection. Otherwise, this is a no-op.
     */
    void ensureMarkersInitialized(OperationContext* opCtx,
                                  boost::optional<TenantId> tenantId,
                                  const CollectionPtr& preImagesColl);

    /*
     * Truncates expired pre-images spanning the 'preImagesColl' associated with the 'tenantId'.
     * Performs in-memory cleanup of the tenant's truncate markers whenever an underlying 'nsUUID'
     * associated with pre-images is dropped.
     */
    TruncateStats truncateExpiredPreImages(OperationContext* opCtx,
                                           boost::optional<TenantId> tenantId,
                                           const CollectionPtr& preImagesColl);

    /**
     * Exclusively used when the config.preimages collection associated with 'tenantId' is dropped.
     * All markers will be dropped immediately.
     */
    void dropAllMarkersForTenant(boost::optional<TenantId> tenantId);

    /**
     * Updates truncate markers to account for a newly inserted 'preImage' into the tenant's
     * pre-images collection. If no truncate markers have been created for the 'tenantId's
     * pre-images collection, this is a no-op.
     */
    void updateMarkersOnInsert(OperationContext* opCtx,
                               boost::optional<TenantId> tenantId,
                               const ChangeStreamPreImage& preImage,
                               int64_t bytesInserted);

    using TenantTruncateMarkers =
        absl::flat_hash_map<UUID, std::shared_ptr<PreImagesTruncateMarkersPerNsUUID>, UUID::Hash>;
    /**
     * Similar to the 'TenantTruncateMarkers' type, but with an added wrapper which enables copy on
     * write semantics.
     */
    using TenantTruncateMarkersCopyOnWrite =
        ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerNsUUID, UUID::Hash>;

    static TenantTruncateMarkers createInitialTruncateMapScanning(
        OperationContext* opCtx,
        boost::optional<TenantId> tenantId,
        const CollectionPtr& preImagesCollectionPtr);

    static void initialisePreImagesCollectionTruncateMarkers(
        OperationContext* opCtx,
        boost::optional<TenantId> tenantId,
        const CollectionPtr& preImagesCollectionPtr,
        TenantTruncateMarkersCopyOnWrite& finalTruncateMap);

private:
    using TenantMap =
        ConcurrentSharedValuesMap<boost::optional<TenantId>, TenantTruncateMarkersCopyOnWrite>;
    TenantMap _tenantMap;
};
}  // namespace mongo
