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

#include "mongo/db/change_collection_truncate_markers.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/operation_context.h"

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(injectCurrentWallTimeForCheckingMarkers);

Date_t getWallTimeToUse(OperationContext* opCtx) {
    auto now = opCtx->getServiceContext()->getFastClockSource()->now();
    injectCurrentWallTimeForCheckingMarkers.execute(
        [&](const BSONObj& data) { now = data.getField("currentWallTime").date(); });
    return now;
}

bool hasMarkerWallTimeExpired(OperationContext* opCtx,
                              Date_t markerWallTime,
                              const TenantId& tenantId) {
    auto now = getWallTimeToUse(opCtx);
    auto expireAfterSeconds =
        Seconds{change_stream_serverless_helpers::getExpireAfterSeconds(tenantId)};
    auto expirationTime = now - expireAfterSeconds;
    return markerWallTime <= expirationTime;
}
}  // namespace

ChangeCollectionTruncateMarkers::ChangeCollectionTruncateMarkers(TenantId tenantId,
                                                                 std::deque<Marker> markers,
                                                                 int64_t leftoverRecordsCount,
                                                                 int64_t leftoverRecordsBytes,
                                                                 int64_t minBytesPerMarker)
    : CollectionTruncateMarkersWithPartialExpiration(
          std::move(markers), leftoverRecordsCount, leftoverRecordsBytes, minBytesPerMarker),
      _tenantId(std::move(tenantId)) {}

bool ChangeCollectionTruncateMarkers::_hasExcessMarkers(OperationContext* opCtx) const {
    const auto& markers = getMarkers();
    if (markers.empty()) {
        // If there's nothing in the markers queue then we don't have excess markers by definition.
        return false;
    }

    const Marker& oldestMarker = markers.front();
    const auto& [highestRecordIdInserted, _] = getPartialMarker();
    if (highestRecordIdInserted <= oldestMarker.lastRecord) {
        // We cannot expire the marker when the last entry is present there as it would break the
        // requirement of always having at least 1 entry present in the collection.
        return false;
    }

    return hasMarkerWallTimeExpired(opCtx, oldestMarker.wallTime, _tenantId);
}

bool ChangeCollectionTruncateMarkers::_hasPartialMarkerExpired(OperationContext* opCtx) const {
    const auto& [_, highestSeenWallTime] = getPartialMarker();

    return hasMarkerWallTimeExpired(opCtx, highestSeenWallTime, _tenantId);
}

void ChangeCollectionTruncateMarkers::expirePartialMarker(OperationContext* opCtx,
                                                          const Collection* changeCollection) {
    createPartialMarkerIfNecessary(opCtx);
    // We can't use the normal peekOldestMarkerIfNeeded method since that calls _hasExcessMarkers
    // and it will return false since the new oldest marker will have the last entry.
    auto oldestMarker =
        checkMarkersWith([&](const std::deque<CollectionTruncateMarkers::Marker>& markers)
                             -> boost::optional<CollectionTruncateMarkers::Marker> {
            // Partial marker did not get generated, early exit.
            if (markers.empty()) {
                return {};
            }
            auto firstMarker = markers.front();
            // We will only consider the case of an expired marker.
            if (!hasMarkerWallTimeExpired(opCtx, firstMarker.wallTime, _tenantId)) {
                return {};
            }
            return firstMarker;
        });

    if (!oldestMarker) {
        // The oldest marker hasn't expired, nothing to do here.
        return;
    }

    // Abandon the snapshot so we can fetch the most recent version of the table. This increases the
    // chances the last entry isn't present in the new partial marker.
    opCtx->recoveryUnit()->abandonSnapshot();
    WriteUnitOfWork wuow(opCtx);

    auto backCursor = changeCollection->getRecordStore()->getCursor(opCtx, false);
    // If the oldest marker does not contain the last entry it's a normal marker, don't perform any
    // modifications to it.
    auto obj = backCursor->next();
    if (!obj || obj->id > oldestMarker->lastRecord) {
        return;
    }

    // At this point the marker contains the last entry of the collection, we have to shift the last
    // entry to the next marker so we can expire the previous entries.
    auto bytesNotTruncated = obj->data.size();
    const auto& doc = obj->data.toBson();
    auto wallTime = doc[repl::OplogEntry::kWallClockTimeFieldName].Date();

    updateCurrentMarkerAfterInsertOnCommit(opCtx, bytesNotTruncated, obj->id, wallTime, 1);

    auto bytesDeleted = oldestMarker->bytes - bytesNotTruncated;
    auto docsDeleted = oldestMarker->records - 1;

    // We build the previous record id based on the extracted value
    auto previousRecordId = [&] {
        auto currId = doc[repl::OplogEntry::k_idFieldName].timestamp();
        invariant(currId > Timestamp::min(), "Last entry timestamp must be larger than 0");

        auto fixedBson = BSON(repl::OplogEntry::k_idFieldName << (currId - 1));

        auto recordId = invariantStatusOK(
            record_id_helpers::keyForDoc(fixedBson,
                                         changeCollection->getClusteredInfo()->getIndexSpec(),
                                         changeCollection->getDefaultCollator()));
        return recordId;
    }();
    auto newMarker =
        CollectionTruncateMarkers::Marker{docsDeleted, bytesDeleted, previousRecordId, wallTime};

    // Replace now the oldest marker with a version that doesn't contain the last entry. This is
    // susceptible to races with concurrent inserts. But the invariant of metrics being correct in
    // aggregate still holds. Ignoring this issue is a valid strategy here as we move the ignored
    // bytes to the next partial marker and we only guarantee eventual correctness.
    modifyMarkersWith([&](std::deque<CollectionTruncateMarkers::Marker>& markers) {
        markers.pop_front();
        markers.emplace_front(std::move(newMarker));
    });
    wuow.commit();
}
}  // namespace mongo
