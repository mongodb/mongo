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

#include "mongo/db/change_stream_pre_images_truncate_markers_per_nsUUID.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/timer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {
// Returns true if the pre-image with highestRecordId and highestWallTime is expired.
bool isExpired(OperationContext* opCtx, const RecordId& highestRecordId, Date_t highestWallTime) {
    auto currentTimeForTimeBasedExpiration =
        change_stream_pre_image_util::getCurrentTimeForPreImageRemoval(opCtx);

    auto opTimeExpirationDate = change_stream_pre_image_util::getPreImageOpTimeExpirationDate(
        opCtx, currentTimeForTimeBasedExpiration);

    // A marker is expired if either:
    //     (1) 'highestWallTime' of the (partial) marker <= current node time -
    //     'expireAfterSeconds' OR
    //     (2) Timestamp of the 'highestRecordId' in the oldest marker <
    //     Timestamp of earliest oplog entry

    // The 'expireAfterSeconds' may or may not be set.
    bool expiredByTimeBasedExpiration =
        opTimeExpirationDate ? highestWallTime <= opTimeExpirationDate : false;

    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(opCtx->getServiceContext())->getEarliestOplogTimestamp(opCtx);
    auto highestRecordTimestamp =
        change_stream_pre_image_util::getPreImageTimestamp(highestRecordId);
    return expiredByTimeBasedExpiration || highestRecordTimestamp < currentEarliestOplogEntryTs;
}

// Returns information (RecordId, wall time, size in bytes) about the last record for 'nsUUID' in
// the pre-images collection, boost::none if no record is found.
boost::optional<std::tuple<RecordId, Date_t, int>> getLastRecordInfo(
    OperationContext* opCtx, const CollectionAcquisition& preImagesCollection, const UUID& nsUUID) {
    const auto rs = preImagesCollection.getCollectionPtr()->getRecordStore();
    auto cursor =
        rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx), false /** forward **/);

    // A reverse inclusive 'seek' will return the previous entry in the collection if the record
    // searched does not exist. This should ensure that the record's id is less than or equal to the
    // 'maxRecordIdForNsUUID'.
    RecordId maxRecordIdForNsUUID =
        change_stream_pre_image_util::getAbsoluteMaxPreImageRecordIdBoundForNs(nsUUID).recordId();
    boost::optional<Record> lastRecord =
        cursor->seek(maxRecordIdForNsUUID, SeekableRecordCursor::BoundInclusion::kInclude);
    if (!lastRecord ||
        nsUUID != change_stream_pre_image_util::getPreImageNsUUID(lastRecord->data.toBson())) {
        return boost::none;
    }
    auto [rid, wallTime] = PreImagesTruncateMarkersPerNsUUID::getRecordIdAndWallTime(*lastRecord);
    return std::tuple<RecordId, Date_t, int>{rid, wallTime, lastRecord->data.size()};
}
}  // namespace

PreImagesTruncateMarkersPerNsUUID::PreImagesTruncateMarkersPerNsUUID(
    const UUID& nsUUID, InitialSetOfMarkers initialSetOfMarkers, int64_t minBytesPerMarker)
    : CollectionTruncateMarkersWithPartialExpiration(std::move(initialSetOfMarkers.markers),
                                                     initialSetOfMarkers.highestRecordId,
                                                     initialSetOfMarkers.highestWallTime,
                                                     initialSetOfMarkers.leftoverRecordsCount,
                                                     initialSetOfMarkers.leftoverRecordsBytes,
                                                     minBytesPerMarker,
                                                     initialSetOfMarkers.timeTaken,
                                                     initialSetOfMarkers.creationMethod),
      _nsUUID(nsUUID) {}

void PreImagesTruncateMarkersPerNsUUID::refreshHighestTrackedRecord(
    OperationContext* opCtx, const CollectionAcquisition& preImagesCollection) {
    const auto lastRecordInfo = getLastRecordInfo(opCtx, preImagesCollection, _nsUUID);
    if (!lastRecordInfo) {
        // There are no records for this 'nsUUID'. It's possible an insert tracked during
        // initialization was rolled back or a new insert created the markers, but has yet to
        // update them.
        return;
    }
    const auto [lastRecordId, lastRecordWallTime, lastRecordBytes] = *lastRecordInfo;

    // The truncate markers are already tracking the 'lastRecord', no need to update.
    bool isTrackingCurrentLastRecord = checkPartialMarkerWith(
        [&lastRecordId = lastRecordId](const RecordId& highestTrackedRecordId, const Date_t&) {
            return highestTrackedRecordId >= lastRecordId;
        });

    if (isTrackingCurrentLastRecord) {
        return;
    }
    updateMarkers(lastRecordBytes, lastRecordId, lastRecordWallTime, 1 /** numRecords */);
}

CollectionTruncateMarkers::RecordIdAndWallTime
PreImagesTruncateMarkersPerNsUUID::getRecordIdAndWallTime(const Record& record) {
    BSONObj preImageObj = record.data.toBson();
    return CollectionTruncateMarkers::RecordIdAndWallTime(record.id, getWallTime(preImageObj));
}

Date_t PreImagesTruncateMarkersPerNsUUID::getWallTime(const BSONObj& preImageObj) {
    return preImageObj[ChangeStreamPreImage::kOperationTimeFieldName].date();
}

PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers
PreImagesTruncateMarkersPerNsUUID::createInitialMarkersFromSamples(
    OperationContext* opCtx,
    const UUID& preImagesCollectionUUID,
    const UUID& nsUUID,
    const std::vector<CollectionTruncateMarkers::RecordIdAndWallTime>& samples,
    int64_t estimatedRecordsPerMarker,
    int64_t estimatedBytesPerMarker,
    uint64_t randomSamplesPerMarker) {
    std::deque<CollectionTruncateMarkers::Marker> wholeMarkers;
    const auto numSamples = samples.size();
    invariant(numSamples > 0);
    invariant(randomSamplesPerMarker > 0);
    for (size_t i = randomSamplesPerMarker - 1; i < numSamples; i = i + randomSamplesPerMarker) {
        const auto& [id, wallTime] = samples[i];
        LOGV2_DEBUG(7658602,
                    1,
                    "Marking potential future truncation point for pre-images collection",
                    "preImagesCollectionUUID"_attr = preImagesCollectionUUID,
                    "nsUUID"_attr = nsUUID,
                    "wallTime"_attr = wallTime,
                    "ts"_attr = change_stream_pre_image_util::getPreImageTimestamp(id));
        wholeMarkers.emplace_back(estimatedRecordsPerMarker, estimatedBytesPerMarker, id, wallTime);
    }

    int64_t currentBytes = 0;
    int64_t currentRecords = 0;
    const auto [highestRecordId, highestWallTime] = samples.back();
    if (wholeMarkers.size() == 0 || wholeMarkers.front().lastRecord < highestRecordId) {
        // For partial marker expiry, the highest tracked record must be tracked with non-zero bytes
        // and count. Otherwise, it will be interpreted as a record which was already truncated.
        //
        // Compute the average records and bytes per sample.
        currentRecords =
            std::max(static_cast<int64_t>(1),
                     (estimatedRecordsPerMarker / static_cast<int64_t>(randomSamplesPerMarker)));
        currentBytes =
            std::max(static_cast<int64_t>(1),
                     (estimatedBytesPerMarker / static_cast<int64_t>(randomSamplesPerMarker)));
    }
    return InitialSetOfMarkers{std::move(wholeMarkers),
                               std::move(highestRecordId),
                               highestWallTime,
                               currentRecords,
                               currentBytes,
                               Microseconds{0} /** timeTaken **/,
                               CollectionTruncateMarkers::MarkersCreationMethod::Sampling};
}

PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers
PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
    OperationContext* opCtx,
    const CollectionAcquisition& preImagesCollection,
    const UUID& nsUUID,
    int64_t minBytesPerMarker) {

    // Snapshots the last Record to help properly account for rollbacks and other events that could
    // affect iteration
    const auto lastRecordInfo = getLastRecordInfo(opCtx, preImagesCollection, nsUUID);
    if (!lastRecordInfo) {
        return InitialSetOfMarkers{};
    }

    // Bound the scan with the snapshotted 'last' record for 'nsUUID'. This caps the amount of work
    // scanning must do with concurrent inserts and guarantees the 'highestRecordId' and
    // 'highestWallTime' are accurate with respect to the upper bound of records tracked.
    const auto [highestRecordId, highestWallTime, _] = *lastRecordInfo;
    RecordIdBound minRecordIdBound =
        change_stream_pre_image_util::getAbsoluteMinPreImageRecordIdBoundForNs(nsUUID);
    RecordIdBound maxRecordIdBound = RecordIdBound(highestRecordId);
    auto exec = InternalPlanner::collectionScan(opCtx,
                                                preImagesCollection,
                                                PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                InternalPlanner::Direction::FORWARD,
                                                boost::none,
                                                std::move(minRecordIdBound),
                                                std::move(maxRecordIdBound));

    std::deque<CollectionTruncateMarkers::Marker> wholeMarkers;
    int64_t currentRecords = 0;
    int64_t currentBytes = 0;
    BSONObj currentPreImageDoc;
    RecordId currentRecordId;
    Timer scanningTimer;
    while (exec->getNext(&currentPreImageDoc, &currentRecordId) == PlanExecutor::ADVANCED) {
        currentRecords++;
        currentBytes += currentPreImageDoc.objsize();
        auto currWallTime = getWallTime(currentPreImageDoc);
        if (currentBytes >= minBytesPerMarker) {
            LOGV2_DEBUG(7500500,
                        1,
                        "Marking potential future truncation point for pre-images collection",
                        "preImagesCollectionUUID"_attr = preImagesCollection.uuid(),
                        "nsUuid"_attr = nsUUID,
                        "wallTime"_attr = currWallTime,
                        "ts"_attr =
                            change_stream_pre_image_util::getPreImageTimestamp(currentRecordId));

            wholeMarkers.emplace_back(std::exchange(currentRecords, 0),
                                      std::exchange(currentBytes, 0),
                                      currentRecordId,
                                      currWallTime);
        }
    }

    if (currentRecords == 0 && wholeMarkers.empty()) {
        // Unlikely, but it's possible the highest record inserted was rolled back.
        return InitialSetOfMarkers{};
    }

    return InitialSetOfMarkers{std::move(wholeMarkers),
                               highestRecordId,
                               highestWallTime,
                               currentRecords,
                               currentBytes,
                               scanningTimer.elapsed(),
                               CollectionTruncateMarkers::MarkersCreationMethod::Scanning};
}

void PreImagesTruncateMarkersPerNsUUID::updateMarkers(int64_t numBytes,
                                                      RecordId recordId,
                                                      Date_t wallTime,
                                                      int64_t numRecords) {
    updateCurrentMarker(numBytes, recordId, wallTime, numRecords);
}

bool PreImagesTruncateMarkersPerNsUUID::_hasExcessMarkers(OperationContext* opCtx) const {
    const auto& markers = getMarkers();
    if (markers.empty()) {
        // If there's nothing in the markers queue then we don't have excess markers by definition.
        return false;
    }

    const Marker& oldestMarker = markers.front();
    return isExpired(opCtx, oldestMarker.lastRecord, oldestMarker.wallTime);
}

bool PreImagesTruncateMarkersPerNsUUID::_hasPartialMarkerExpired(
    OperationContext* opCtx,
    const RecordId& highestSeenRecordId,
    const Date_t& highestSeenWallTime) const {
    if (highestSeenRecordId.isNull()) {
        // 'PreImagesTruncateMarkersPerNsUUID' are constructed without specifying a
        // 'highestSeenRecordId'. Account for newly constructed markers that have yet to be updated.
        return false;
    }
    return isExpired(opCtx, highestSeenRecordId, highestSeenWallTime);
}
}  // namespace mongo
