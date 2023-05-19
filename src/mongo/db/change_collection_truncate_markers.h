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
 * Implementation of truncate markers for Change Collections. Respects the requirement of always
 * maintaining at least 1 entry in the change collection.
 */
namespace mongo {
class ChangeCollectionTruncateMarkers final
    : public CollectionTruncateMarkersWithPartialExpiration {
public:
    ChangeCollectionTruncateMarkers(TenantId tenantId,
                                    std::deque<Marker> markers,
                                    int64_t leftoverRecordsCount,
                                    int64_t leftoverRecordsBytes,
                                    int64_t minBytesPerMarker);

    // Expires the partial marker with proper care for the last entry. Expiring here means:
    //   * Turning the partial marker into an actual marker
    //   * Ensuring the last entry isn't present in the generated marker
    // The last entry is necessary for correctness of the change collection. This method will shift
    // the last entry size and count to the next partial marker.
    void expirePartialMarker(OperationContext* opCtx, const Collection* changeCollection);

private:
    bool _hasExcessMarkers(OperationContext* opCtx) const override;

    bool _hasPartialMarkerExpired(OperationContext* opCtx) const override;

    TenantId _tenantId;
};
}  // namespace mongo
