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

#include "mongo/db/change_stream_pre_images_truncate_markers.h"

#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"

namespace mongo {

PreImagesTruncateMarkers::PreImagesTruncateMarkers(boost::optional<TenantId> tenantId,
                                                   std::deque<Marker> markers,
                                                   int64_t leftoverRecordsCount,
                                                   int64_t leftoverRecordsBytes,
                                                   int64_t minBytesPerMarker)
    : CollectionTruncateMarkers(
          std::move(markers), leftoverRecordsCount, leftoverRecordsBytes, minBytesPerMarker, true),
      _tenantId(std::move(tenantId)) {}

bool PreImagesTruncateMarkers::_hasExcessMarkers(OperationContext* opCtx) const {
    const auto& markers = getMarkers();
    if (markers.empty()) {
        // If there's nothing in the markers queue then we don't have excess markers by definition.
        return false;
    }

    const Marker& oldestMarker = markers.front();
    Date_t currentTimeForTimeBasedExpiration =
        opCtx->getServiceContext()->getFastClockSource()->now();

    if (_tenantId) {
        // In a serverless environment, the 'expireAfterSeconds' is set per tenant and is the only
        // criteria considered when determining whether a marker is expired.
        //
        // The oldest marker is expired if:
        //   'wallTime' of the oldest marker <= current node time - 'expireAfterSeconds'
        auto expireAfterSeconds =
            Seconds{change_stream_serverless_helpers::getExpireAfterSeconds(_tenantId.get())};
        auto preImageExpirationTime = currentTimeForTimeBasedExpiration - expireAfterSeconds;
        return oldestMarker.wallTime <= preImageExpirationTime;
    }

    // In a non-serverless enviornment, the oldest marker is expired if either:
    //     (1) 'wallTime' of the oldest marker <= current node time - 'expireAfterSeconds'
    //     OR
    //     (2) Timestamp of the 'lastRecord' in the oldest marker < Timestamp of earliest oplog
    //     entry

    // The 'expireAfterSeconds' may or may not be set in a non-serverless enviornment.
    const auto preImageExpirationTime = change_stream_pre_image_helpers::getPreImageExpirationTime(
        opCtx, currentTimeForTimeBasedExpiration);
    bool expiredByTimeBasedExpiration =
        preImageExpirationTime ? oldestMarker.wallTime <= preImageExpirationTime : false;

    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(opCtx->getServiceContext())->getEarliestOplogTimestamp(opCtx);
    auto lastRecordTS =
        change_stream_pre_image_helpers::getPreImageTimestamp(oldestMarker.lastRecord);
    return expiredByTimeBasedExpiration || lastRecordTS < currentEarliestOplogEntryTs;
}
}  // namespace mongo
