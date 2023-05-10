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

#include "mongo/db/storage/collection_truncate_markers.h"

/**
 * There is up to one 'config.system.preimages' collection per tenant. This pre-images
 * collection contains pre-images for every collection UUID with pre-images enabled on the tenant.
 * The pre-images collection is ordered by collection UUID, so that pre-images belonging to a given
 * collection are grouped together. Additionally, pre-images for a given collection UUID are stored
 * in timestamp order, which makes range truncation possible.
 *
 * Implementation of truncate markers for pre-images associated with a single collection UUID within
 * a pre-images collection.
 */
namespace mongo {

class PreImagesTruncateMarkersPerCollection final
    : public CollectionTruncateMarkersWithPartialExpiration {
public:
    PreImagesTruncateMarkersPerCollection(boost::optional<TenantId> tenantId,
                                          std::deque<Marker> markers,
                                          int64_t leftoverRecordsCount,
                                          int64_t leftoverRecordsBytes,
                                          int64_t minBytesPerMarker);

    /**
     * Creates an initial set of markers for pre-images from 'nsUUID'.
     */
    static CollectionTruncateMarkers::InitialSetOfMarkers createTruncateMarkersByScanning(
        OperationContext* opCtx,
        RecordStore* rs,
        const UUID& nsUUID,
        RecordId& highestSeenRecordId,
        Date_t& highestSeenWallTime);

private:
    friend class PreImagesTruncateMarkersPerCollectionTest;

    bool _hasExcessMarkers(OperationContext* opCtx) const override;

    bool _hasPartialMarkerExpired(OperationContext* opCtx) const override;

    /**
     * When initialized, indicates this is a serverless environment.
     */
    boost::optional<TenantId> _tenantId;
};
}  // namespace mongo
