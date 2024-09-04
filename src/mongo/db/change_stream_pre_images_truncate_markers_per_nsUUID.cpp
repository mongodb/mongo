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

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/duration.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {
// Returns true if the pre-image with highestRecordId and highestWallTime is expired.
bool isExpired(OperationContext* opCtx,
               const boost::optional<TenantId>& tenantId,
               const RecordId& highestRecordId,
               Date_t highestWallTime) {
    auto currentTimeForTimeBasedExpiration =
        change_stream_pre_image_util::getCurrentTimeForPreImageRemoval(opCtx);

    auto opTimeExpirationDate = change_stream_pre_image_util::getPreImageOpTimeExpirationDate(
        opCtx, tenantId, currentTimeForTimeBasedExpiration);

    if (tenantId) {
        // In a serverless environment, the 'expireAfterSeconds' is set per tenant and pre-images
        // always expire according to their 'operationTime'.
        invariant(opTimeExpirationDate);

        // The oldest marker is expired if:
        //   'wallTime' of the oldest marker <= current node time - 'expireAfterSeconds'.
        return highestWallTime <= *opTimeExpirationDate;
    }

    // In a non-serverless environment, a marker is expired if either:
    //     (1) 'highestWallTime' of the (partial) marker <= current node time -
    //     'expireAfterSeconds' OR
    //     (2) Timestamp of the 'highestRecordId' in the oldest marker <
    //     Timestamp of earliest oplog entry

    // The 'expireAfterSeconds' may or may not be set in a non-serverless environment.
    bool expiredByTimeBasedExpiration =
        opTimeExpirationDate ? highestWallTime <= opTimeExpirationDate : false;

    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(opCtx->getServiceContext())->getEarliestOplogTimestamp(opCtx);
    auto highestRecordTimestamp =
        change_stream_pre_image_util::getPreImageTimestamp(highestRecordId);
    return expiredByTimeBasedExpiration || highestRecordTimestamp < currentEarliestOplogEntryTs;
}

}  // namespace

PreImagesTruncateMarkersPerNsUUID::PreImagesTruncateMarkersPerNsUUID(
    boost::optional<TenantId> tenantId,
    std::deque<Marker> markers,
    int64_t leftoverRecordsCount,
    int64_t leftoverRecordsBytes,
    int64_t minBytesPerMarker,
    CollectionTruncateMarkers::MarkersCreationMethod creationMethod)
    : CollectionTruncateMarkersWithPartialExpiration(
          std::move(markers), leftoverRecordsCount, leftoverRecordsBytes, minBytesPerMarker),
      _tenantId(std::move(tenantId)),
      _creationMethod(creationMethod) {}

CollectionTruncateMarkers::RecordIdAndWallTime
PreImagesTruncateMarkersPerNsUUID::getRecordIdAndWallTime(const Record& record) {
    BSONObj preImageObj = record.data.toBson();
    return CollectionTruncateMarkers::RecordIdAndWallTime(record.id, getWallTime(preImageObj));
}

Date_t PreImagesTruncateMarkersPerNsUUID::getWallTime(const BSONObj& preImageObj) {
    return preImageObj[ChangeStreamPreImage::kOperationTimeFieldName].date();
}

CollectionTruncateMarkers::InitialSetOfMarkers
PreImagesTruncateMarkersPerNsUUID::createInitialMarkersFromSamples(
    OperationContext* opCtx,
    const UUID& preImagesCollectionUUID,
    const UUID& nsUUID,
    const std::vector<CollectionTruncateMarkers::RecordIdAndWallTime>& samples,
    int64_t estimatedRecordsPerMarker,
    int64_t estimatedBytesPerMarker,
    uint64_t randomSamplesPerMarker) {
    std::deque<CollectionTruncateMarkers::Marker> markers;
    auto numSamples = samples.size();
    invariant(numSamples > 0);
    for (size_t i = randomSamplesPerMarker - 1; i < numSamples; i = i + randomSamplesPerMarker) {
        const auto& [id, wallTime] = samples[i];
        LOGV2_DEBUG(7658602,
                    1,
                    "Marking potential future truncation point for pre-images collection",
                    "preImagesCollectionUUID"_attr = preImagesCollectionUUID,
                    "nsUUID"_attr = nsUUID,
                    "wallTime"_attr = wallTime,
                    "ts"_attr = change_stream_pre_image_util::getPreImageTimestamp(id));
        markers.emplace_back(estimatedRecordsPerMarker, estimatedBytesPerMarker, id, wallTime);
    }

    // Sampling is best effort estimations and at this step, only account for the whole markers
    // generated and leave the 'currentRecords' and 'currentBytes' to be filled in at a later time.
    // Additionally, the time taken is relatively arbitrary as the expensive part of the operation
    // was retrieving the samples.
    return CollectionTruncateMarkers::InitialSetOfMarkers{
        std::move(markers),
        0 /** currentRecords **/,
        0 /** currentBytes **/,
        Microseconds{0} /** timeTaken **/,
        CollectionTruncateMarkers::MarkersCreationMethod::Sampling};
}

CollectionTruncateMarkers::InitialSetOfMarkers
PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
    OperationContext* opCtx,
    const CollectionAcquisition& collAcq,
    const UUID& nsUUID,
    int64_t minBytesPerMarker) {
    Timer scanningTimer;

    RecordIdBound minRecordIdBound =
        change_stream_pre_image_util::getAbsoluteMinPreImageRecordIdBoundForNs(nsUUID);
    RecordId minRecordId = minRecordIdBound.recordId();

    RecordIdBound maxRecordIdBound =
        change_stream_pre_image_util::getAbsoluteMaxPreImageRecordIdBoundForNs(nsUUID);
    RecordId maxRecordId = maxRecordIdBound.recordId();

    auto exec = InternalPlanner::collectionScan(opCtx,
                                                collAcq,
                                                PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                InternalPlanner::Direction::FORWARD,
                                                boost::none,
                                                std::move(minRecordIdBound),
                                                std::move(maxRecordIdBound));
    int64_t currentRecords = 0;
    int64_t currentBytes = 0;
    std::deque<CollectionTruncateMarkers::Marker> markers;
    BSONObj docOut;
    RecordId rIdOut;
    while (exec->getNext(&docOut, &rIdOut) == PlanExecutor::ADVANCED) {
        currentRecords++;
        currentBytes += docOut.objsize();

        auto wallTime = getWallTime(docOut);
        if (currentBytes >= minBytesPerMarker) {
            LOGV2_DEBUG(7500500,
                        1,
                        "Marking potential future truncation point for pre-images collection",
                        "preImagesCollectionUUID"_attr = collAcq.uuid(),
                        "nsUuid"_attr = nsUUID,
                        "wallTime"_attr = wallTime,
                        "ts"_attr = change_stream_pre_image_util::getPreImageTimestamp(rIdOut));

            markers.emplace_back(
                std::exchange(currentRecords, 0), std::exchange(currentBytes, 0), rIdOut, wallTime);
        }
    }

    if (currentRecords == 0 && markers.empty()) {
        return CollectionTruncateMarkers::InitialSetOfMarkers{
            {}, 0, 0, Microseconds{0}, MarkersCreationMethod::EmptyCollection};
    }

    return CollectionTruncateMarkers::InitialSetOfMarkers{
        std::move(markers),
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
    return isExpired(opCtx, _tenantId, oldestMarker.lastRecord, oldestMarker.wallTime);
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
    return isExpired(opCtx, _tenantId, highestSeenRecordId, highestSeenWallTime);
}
}  // namespace mongo
