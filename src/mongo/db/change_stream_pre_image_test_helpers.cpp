/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/change_stream_pre_image_test_helpers.h"

#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/collection_crud/collection_write_path.h"

namespace mongo {
namespace {
// Given a RecordId that is either null or a pre-image RecordId, appends field '<ridFieldName>' with
// the 'rid' and field '<ridFieldName>Timestamp' when the RecordId is non-null.
void appendPreImageRecordIdAndTS(const RecordId& rid,
                                 std::string&& ridFieldName,
                                 BSONObjBuilder* builder) {
    rid.serializeToken(ridFieldName, builder);
    if (!rid.isNull()) {
        builder->append(ridFieldName + "Timestamp",
                        change_stream_pre_image_util::getPreImageTimestamp(rid));
    }
}

}  // namespace
namespace change_stream_pre_image_test_helper {

void createPreImagesCollection(OperationContext* opCtx) {
    ChangeStreamPreImagesCollectionManager::get(opCtx).createPreImagesCollection(opCtx);
}

void insertDirectlyToPreImagesCollection(OperationContext* opCtx,
                                         const ChangeStreamPreImage& preImage) {
    const auto preImagesAcq = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(NamespaceString::kChangeStreamPreImagesNamespace,
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);

    WriteUnitOfWork wuow(opCtx);
    uassertStatusOK(collection_internal::insertDocument(
        opCtx, preImagesAcq.getCollectionPtr(), InsertStatement{preImage.toBSON()}, nullptr));
    wuow.commit();
}

ChangeStreamPreImage makePreImage(const UUID& nsUUID,
                                  const Timestamp& timestamp,
                                  const Date_t& wallTime) {
    ChangeStreamPreImageId preImageId(nsUUID, timestamp, 0);
    return ChangeStreamPreImage{std::move(preImageId), wallTime, BSON("randomField" << 'a')};
}

CollectionTruncateMarkers::RecordIdAndWallTime extractRecordIdAndWallTime(
    const ChangeStreamPreImage& preImage) {
    return {change_stream_pre_image_util::toRecordId(preImage.getId()),
            preImage.getOperationTime()};
}

std::unique_ptr<ChangeStreamOptions> populateChangeStreamPreImageOptions(
    std::variant<std::string, std::int64_t> expireAfterSeconds) {
    PreAndPostImagesOptions preAndPostImagesOptions;
    preAndPostImagesOptions.setExpireAfterSeconds(expireAfterSeconds);

    auto changeStreamOptions = std::make_unique<ChangeStreamOptions>();
    changeStreamOptions->setPreAndPostImages(std::move(preAndPostImagesOptions));

    return changeStreamOptions;
}

void setChangeStreamOptionsToManager(OperationContext* opCtx,
                                     ChangeStreamOptions& changeStreamOptions) {
    auto& changeStreamOptionsManager = ChangeStreamOptionsManager::get(opCtx);
    uassertStatusOK(changeStreamOptionsManager.setOptions(opCtx, changeStreamOptions));
}

BSONObj toBSON(const CollectionTruncateMarkers::Marker& preImageMarker) {
    BSONObjBuilder builder;
    builder.append("records", preImageMarker.records);
    builder.append("bytes", preImageMarker.bytes);
    appendPreImageRecordIdAndTS(preImageMarker.lastRecord, "lastRecord", &builder);
    builder.append("wallTime", preImageMarker.wallTime);

    return builder.obj();
}

BSONObj toBSON(const PreImagesTruncateMarkersPerNsUUID& truncateMarkers) {
    BSONObjBuilder builder;

    BSONArrayBuilder markersBuilder;
    const auto wholeMarkers = truncateMarkers.getMarkers_forTest();
    for (const auto& wholeMarker : wholeMarkers) {
        markersBuilder.append(toBSON(wholeMarker));
    }
    builder.appendArray("wholeMarkers", markersBuilder.arr());
    const auto [highestRid, highestWallTime] = truncateMarkers.getHighestRecordMetrics_forTest();
    appendPreImageRecordIdAndTS(highestRid, "highestRecordId", &builder);
    builder.append("highestWallTime", highestWallTime);

    builder.append("partialMarkerRecords", truncateMarkers.currentBytes_forTest());
    builder.append("partialMarkerBytes", truncateMarkers.currentRecords_forTest());
    return builder.obj();
}

CollectionTruncateMarkers::Marker makeWholeMarker(const ChangeStreamPreImage& lastRecord,
                                                  int64_t records,
                                                  int64_t bytes) {
    const auto [recordId, wallTime] = extractRecordIdAndWallTime(lastRecord);
    return CollectionTruncateMarkers::Marker{records, bytes, recordId, wallTime};
}

PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers makeInitialSetOfMarkers(
    std::deque<CollectionTruncateMarkers::Marker> wholeMarkers,
    const ChangeStreamPreImage& highestPreImage,
    int64_t partialMarkerRecords,
    int64_t partialMarkerBytes,
    CollectionTruncateMarkers::MarkersCreationMethod creationMethod) {
    const auto [highestRecordId, highestWallTime] = extractRecordIdAndWallTime(highestPreImage);
    return {std::move(wholeMarkers),
            std::move(highestRecordId),
            highestWallTime,
            partialMarkerRecords,
            partialMarkerBytes,
            Microseconds(0),
            creationMethod};
}

PreImagesTruncateMarkersPerNsUUID makeEmptyTruncateMarkers(const UUID& nsUUID,
                                                           int64_t minBytesPerMarker) {
    return {nsUUID, PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers{}, minBytesPerMarker};
}

void updateMarkers(const ChangeStreamPreImage& preImage,
                   PreImagesTruncateMarkersPerNsUUID& nsUUIDTruncateMarkers) {
    auto [recordId, wallTime] = extractRecordIdAndWallTime(preImage);
    nsUUIDTruncateMarkers.updateMarkers(bytes(preImage), recordId, wallTime, 1);
}

bool activelyTrackingPreImage(const PreImagesTruncateMarkersPerNsUUID& nsUUIDTruncateMarkers,
                              const ChangeStreamPreImage& preImage) {
    if (nsUUIDTruncateMarkers.isEmpty()) {
        return false;
    }

    const auto [highestTrackedRid, highestTrackedWallTime] =
        nsUUIDTruncateMarkers.getHighestRecordMetrics_forTest();
    const auto [preImageRid, preImageWallTime] = extractRecordIdAndWallTime(preImage);

    if (preImageRid > highestTrackedRid || preImageWallTime > highestTrackedWallTime) {
        return false;
    }

    if (nsUUIDTruncateMarkers.currentRecords_forTest() != 0 &&
        nsUUIDTruncateMarkers.currentBytes_forTest() != 0) {
        // Non-zero bytes and records for the partial marker guarantees all pre-images with a
        // RecordId and wall time less than the highest tracked record can eventually be expired.
        return true;
    }

    const auto wholeMarkers = nsUUIDTruncateMarkers.getMarkers_forTest();
    if (wholeMarkers.empty()) {
        // Pre-image is neither tracked in the partial marker, nor by a whole marker.
        return false;
    }

    // Provided the pre-image is older than the last record of the most recent whole marker, it is
    // tracked.
    const auto& mostRecentWholeMarker = wholeMarkers.back();
    return preImageRid <= mostRecentWholeMarker.lastRecord &&
        preImageWallTime <= mostRecentWholeMarker.wallTime;
}

int64_t bytes(const ChangeStreamPreImage& preImage) {
    return preImage.toBSON().objsize();
}

CollectionAcquisition acquirePreImagesCollectionForRead(OperationContext* opCtx) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(NamespaceString::kChangeStreamPreImagesNamespace,
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
}

}  // namespace change_stream_pre_image_test_helper
}  // namespace mongo
