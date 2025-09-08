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

#pragma once

#include "mongo/db/change_stream_options_gen.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_pre_images_truncate_markers_per_nsUUID.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/storage/collection_truncate_markers.h"

namespace mongo {
namespace change_stream_pre_image_test_helper {

/**
 * Pre-defined pre-images and UUIDs for testing pre-image components.
 */
struct ChangeStreamPreImageTestConstants {
    /**
     * 'kNsUUID' constants.
     */
    const UUID kNsUUID = UUID::gen();
    const ChangeStreamPreImage kPreImageMin{
        ChangeStreamPreImageId{kNsUUID, Timestamp::min(), 0}, Date_t{}, BSON("x" << 0)};
    const ChangeStreamPreImage kPreImage1{ChangeStreamPreImageId{kNsUUID, Timestamp(1, 1), 0},
                                          dateFromISOString("2024-01-01T00:00:01.000Z").getValue(),
                                          BSON("x" << 1)};
    const ChangeStreamPreImage kPreImage2{ChangeStreamPreImageId{kNsUUID, Timestamp(1, 2), 0},
                                          dateFromISOString("2024-01-01T00:00:02.000Z").getValue(),
                                          BSON("x" << 2)};
    const ChangeStreamPreImage kPreImage3{ChangeStreamPreImageId{kNsUUID, Timestamp(1, 3), 0},
                                          dateFromISOString("2024-01-01T00:00:03.000Z").getValue(),
                                          BSON("x" << 3)};
    const ChangeStreamPreImage kPreImageMax{
        ChangeStreamPreImageId{kNsUUID, Timestamp::max(), std::numeric_limits<int64_t>::max()},
        Date_t::max(),
        BSON("x" << 100)};

    /**
     * 'kNsUUIDOther' constants.
     */
    const UUID kNsUUIDOther = UUID::gen();
    const ChangeStreamPreImage kPreImageOtherMin{
        ChangeStreamPreImageId{kNsUUIDOther, Timestamp::min(), 0}, Date_t{}, BSON("x" << 0)};
    const ChangeStreamPreImage kPreImageOther{
        ChangeStreamPreImageId{kNsUUIDOther, Timestamp(1, 1), 0},
        dateFromISOString("2024-01-01T00:00:01.000Z").getValue(),
        BSON("x" << 1)};
    const ChangeStreamPreImage kPreImageOtherMax{
        ChangeStreamPreImageId{kNsUUIDOther, Timestamp::max(), std::numeric_limits<int64_t>::max()},
        Date_t::max(),
        BSON("x" << 100)};
};

/**
 * Returns 'ChangeStreamOptions' populated with 'expireAfterSeconds'.
 */
std::unique_ptr<ChangeStreamOptions> populateChangeStreamPreImageOptions(
    std::variant<std::string, std::int64_t> expireAfterSeconds);

/**
 * Sets the 'changeStreamOptions' on the 'ChangeStreamOptionsManager' tied to the 'opCtx'.
 */
void setChangeStreamOptionsToManager(OperationContext* opCtx,
                                     ChangeStreamOptions& changeStreamOptions);

/**
 * Generates a pre-image specific BSON for the CollectionTruncateMarkers::Marker with the
 * 'lastRecord's timestamp extracted when non-null.
 */
BSONObj toBSON(const CollectionTruncateMarkers::Marker& preImageMarker);

/**
 * Returns a BSON representation of the truncate markers for enhanced failure reporting.
 */
BSONObj toBSON(const PreImagesTruncateMarkersPerNsUUID& truncateMarkers);

CollectionTruncateMarkers::Marker makeWholeMarker(const ChangeStreamPreImage& lastRecord,
                                                  int64_t records,
                                                  int64_t bytes);

/**
 * Creates a set of initial markers that accounts for both the 'wholeMarkers', as well as the
 * partial marker. 'highestPreImage' is the pre-image with the highest seen RecordId and wall time.
 */
PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers makeInitialSetOfMarkers(
    std::deque<CollectionTruncateMarkers::Marker> wholeMarkers,
    const ChangeStreamPreImage& highestPreImage,
    int64_t partialMarkerRecords,
    int64_t partialMarkerBytes,
    CollectionTruncateMarkers::MarkersCreationMethod creationMethod);

PreImagesTruncateMarkersPerNsUUID makeEmptyTruncateMarkers(const UUID& nsUUID,
                                                           int64_t minBytesPerMarker);

/**
 * For test convenience, issues PreImagesTruncateMarkersPerNsUUID::updateMarkers() with input
 * extracted from 'preImage'.
 */
void updateMarkers(const ChangeStreamPreImage& preImage,
                   PreImagesTruncateMarkersPerNsUUID& nsUUIDTruncateMarkers);

/**
 * Returns 'true' if the 'nsUUIDTruncateMarkers' actively track the 'preImage'. A pre-image is
 * actively tracked if:
 *      . The pre-image is less than or equal to the highest tracked record
 *      (according to RecordId and wall time).
 *      . The 'nsUUIDTruncateMarkers' track non-zero bytes and records across whole markers and/or
 *      the partial markers. This ensures the 'preImage' is actively tracked, and not viewed as a
 *      record which has already been truncated.
 */
bool activelyTrackingPreImage(const PreImagesTruncateMarkersPerNsUUID& nsUUIDTruncateMarkers,
                              const ChangeStreamPreImage& preImage);

/**
 * Performs a direct write to the pre-images collection.
 */
void insertDirectlyToPreImagesCollection(OperationContext* opCtx,
                                         const ChangeStreamPreImage& preImage);

ChangeStreamPreImage makePreImage(const UUID& nsUUID,
                                  const Timestamp& timestamp,
                                  const Date_t& wallTime);

CollectionTruncateMarkers::RecordIdAndWallTime extractRecordIdAndWallTime(
    const ChangeStreamPreImage& preImage);

void createPreImagesCollection(OperationContext* opCtx);

/**
 * Returns the size of the 'preImage' in bytes.
 */
int64_t bytes(const ChangeStreamPreImage& preImage);

/**
 * Readability convenience method - obtains locks to read the pre-image collection.
 */
CollectionAcquisition acquirePreImagesCollectionForRead(OperationContext* opCtx);
}  // namespace change_stream_pre_image_test_helper
}  // namespace mongo
