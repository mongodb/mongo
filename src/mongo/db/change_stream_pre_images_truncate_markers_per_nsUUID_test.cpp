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
#include <fmt/format.h>

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/change_stream_pre_image_test_helpers.h"
#include "mongo/db/change_stream_pre_images_truncate_markers_per_nsUUID.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
using namespace change_stream_pre_image_test_helper;

BSONObj toBSON(const CollectionTruncateMarkers::InitialSetOfMarkers& initialSetOfPreImageMarkers) {
    BSONObjBuilder builder;

    BSONArrayBuilder markersBuilder;
    for (const auto& marker : initialSetOfPreImageMarkers.markers) {
        markersBuilder.append(change_stream_pre_image_test_helper::toBSON(marker));
    }
    builder.appendArray("markers", markersBuilder.arr());
    builder.append("leftoverRecordsCount", initialSetOfPreImageMarkers.leftoverRecordsCount);
    builder.append("leftoverRecordsBytes", initialSetOfPreImageMarkers.leftoverRecordsBytes);
    builder.append("methodUsed",
                   CollectionTruncateMarkers::toString(initialSetOfPreImageMarkers.methodUsed));
    return builder.obj();
}

// Returns a marker with the 'preImage' as its upper bound.
CollectionTruncateMarkers::Marker markerAtBound(const ChangeStreamPreImage& preImage,
                                                int64_t records,
                                                int64_t bytes) {
    const auto [recordId, wallTime] = extractRecordIdAndWallTime(preImage);
    return CollectionTruncateMarkers::Marker{records, bytes, recordId, wallTime};
}

//
// Tests the generation of 'CollectionTruncateMarkers::InitialSetOfMarkers' for an nsUUID with
// pre-images via scanning.
//
// Scanning generates an initial set of markers which accounts for all bytes and records for the
// nsUUID scanned by the pre-image collection.
class PreImageInitialSetOfMarkersScanningTest : public CatalogTestFixture,
                                                public ChangeStreamPreImageTestConstants {};

TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersEmptyCollection) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx, kTenantId);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx, kTenantId);
    int64_t minBytesPerMarker = 4;
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

    auto expectedInitialMarkers = CollectionTruncateMarkers::InitialSetOfMarkers{};
    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersNoFullMarkers1PreImage) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx, kTenantId);

    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImage1);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx, kTenantId);

    // Large 'minBytesPerMarker' to test 0 markers created.
    int64_t minBytesPerMarker = 10000;
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

    CollectionTruncateMarkers::InitialSetOfMarkers expectedInitialMarkers{
        std::deque<CollectionTruncateMarkers::Marker>{},
        1 /* leftoverRecordsCount*/,
        bytes(kPreImage1) /* leftoverRecordsBytes */,
        Microseconds{0},
        CollectionTruncateMarkers::MarkersCreationMethod::Scanning};

    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersFullMarker1PreImage) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx, kTenantId);

    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImage1);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx, kTenantId);

    // Small 'minBytesPerMarker' to test full marker creation.
    int64_t minBytesPerMarker = 3;
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

    CollectionTruncateMarkers::InitialSetOfMarkers expectedInitialMarkers{
        std::deque<CollectionTruncateMarkers::Marker>{
            markerAtBound(kPreImage1, 1, bytes(kPreImage1))},
        0 /* leftoverRecordsCount*/,
        0 /* leftoverRecordsBytes */,
        Microseconds{0},
        CollectionTruncateMarkers::MarkersCreationMethod::Scanning};

    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

// Markers are created when 'minBytesPerMarker' is met or exceeded. Tests that a full marker
// accounts for all records and bytes within its bound - even when the range slightly exceeds
// 'minBytesPerMarker'.
TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersFullMarkerNoLeftovers) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx, kTenantId);

    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImage1);
    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImage2);
    const auto preImageBytesTotal = bytes(kPreImage1) + bytes(kPreImage2);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx, kTenantId);

    int64_t minBytesPerMarker = preImageBytesTotal - 2;
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

    CollectionTruncateMarkers::InitialSetOfMarkers expectedInitialMarkers{
        std::deque<CollectionTruncateMarkers::Marker>{
            markerAtBound(kPreImage2, 2, preImageBytesTotal)},
        0 /* leftoverRecordsCount*/,
        0 /* leftoverRecordsBytes */,
        Microseconds{0},
        CollectionTruncateMarkers::MarkersCreationMethod::Scanning};

    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersFullMarkerLeftovers) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx, kTenantId);

    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImage1);
    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImage2);
    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImage3);

    // Full marker is created with 'preImage2' as its bound.
    const auto bytesPreImage1And2 = bytes(kPreImage1) + bytes(kPreImage2);
    const auto minBytesPerMarker = bytesPreImage1And2 - 2;

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx, kTenantId);

    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

    CollectionTruncateMarkers::InitialSetOfMarkers expectedInitialMarkers{
        std::deque<CollectionTruncateMarkers::Marker>{
            markerAtBound(kPreImage2, 2, bytesPreImage1And2)},
        1 /* leftoverRecordsCount*/,
        bytes(kPreImage3) /* leftoverRecordsBytes */,
        Microseconds{0},
        CollectionTruncateMarkers::MarkersCreationMethod::Scanning};

    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersManyFullMarkers) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx, kTenantId);

    const auto numPreImages = 20;
    std::vector<ChangeStreamPreImage> preImages(numPreImages);
    const Date_t baseDate = dateFromISOString("2024-01-01T00:00:01.000Z").getValue();
    const Timestamp baseTimestamp = Timestamp(1, 1);
    for (int i = 0; i < numPreImages; i++) {
        // Insert pre-images of uniform size for testing simplicitly.
        preImages[i] = makePreImage(kNsUUID, baseTimestamp + i, baseDate);
        insertDirectlyToPreImagesCollection(opCtx, kTenantId, preImages[i]);
    }

    const auto bytesPerPreImage = bytes(preImages[0]);
    const auto targetPreImagesPerMarker = 7;

    // Subtract a small amount from the target bytes per marker so every 'targetPreImagesPerMarker'
    // pre-image generates a marker.
    const auto minBytesPerMarker = bytesPerPreImage * targetPreImagesPerMarker - 2;

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx, kTenantId);
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

    const int32_t expectedWholeMarkers = numPreImages / targetPreImagesPerMarker;
    const int32_t expectedBytesPerMarker = targetPreImagesPerMarker * bytesPerPreImage;
    std::deque<CollectionTruncateMarkers::Marker> expectedWholeMarkersQueue{};
    for (int i = 1; i <= expectedWholeMarkers; i++) {
        // Whole markers start at the 'targetPreImagesPerMarker'ith pre-image. Since pre-images
        // begin with 0 index, subtract 1.
        const auto preImageIndex = i * targetPreImagesPerMarker - 1;
        expectedWholeMarkersQueue.push_back(markerAtBound(
            preImages[preImageIndex], targetPreImagesPerMarker, expectedBytesPerMarker));
    }

    const int32_t expectedLeftoverRecords = numPreImages % targetPreImagesPerMarker;
    const int32_t expectedLeftoverBytes = expectedLeftoverRecords * bytesPerPreImage;

    CollectionTruncateMarkers::InitialSetOfMarkers expectedInitialMarkers{
        std::move(expectedWholeMarkersQueue),
        expectedLeftoverRecords /* leftoverRecordsCount*/,
        expectedLeftoverBytes /* leftoverRecordsBytes */,
        Microseconds{0},
        CollectionTruncateMarkers::MarkersCreationMethod::Scanning};

    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

// Tests the extreme bounds of possible pre-images for a given nsUUID.
TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersIncludeMinAndMax) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx, kTenantId);

    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImageMin);
    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImageMax);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx, kTenantId);

    // Small 'minBytesPerMarker' to test the minimum pre-image for 'kNsUUID' is accounted for via
    // scanning.
    int64_t minBytesPerMarker = 1;
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

    CollectionTruncateMarkers::InitialSetOfMarkers expectedInitialMarkers{
        std::deque<CollectionTruncateMarkers::Marker>{
            markerAtBound(kPreImageMin, 1, bytes(kPreImageMin)),
            markerAtBound(kPreImageMax, 1, bytes(kPreImageMax))},
        0 /* leftoverRecordsCount*/,
        0 /* leftoverRecordsBytes */,
        Microseconds{0},
        CollectionTruncateMarkers::MarkersCreationMethod::Scanning};

    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

// Tests that when there are pre-images for multiple nsUUIDs, scanning a specific nsUUID generates
// markers only for the target nsUUID.
TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersIsolatedPerNsUUID) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx, kTenantId);

    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImageMin);
    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImage1);
    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImageMax);

    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImageOtherMin);
    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImageOther);
    insertDirectlyToPreImagesCollection(opCtx, kTenantId, kPreImageOtherMax);

    // Test that initial markers are isolated for 'kNsUUID'.
    {
        const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx, kTenantId);
        int64_t minBytesPerMarker = 1;
        const auto actualInitialMarkers =
            PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
                opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

        CollectionTruncateMarkers::InitialSetOfMarkers expectedInitialMarkers{
            std::deque<CollectionTruncateMarkers::Marker>{
                markerAtBound(kPreImageMin, 1, bytes(kPreImageMin)),
                markerAtBound(kPreImage1, 1, bytes(kPreImage1)),
                markerAtBound(kPreImageMax, 1, bytes(kPreImageMax))},
            0 /* leftoverRecordsCount*/,
            0 /* leftoverRecordsBytes */,
            Microseconds{0},
            CollectionTruncateMarkers::MarkersCreationMethod::Scanning};
        ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
    }

    // Test that initial markers are isolated for 'kNsUUIDOther'.
    {
        const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx, kTenantId);
        int64_t minBytesPerMarker = 1;
        const auto actualInitialMarkers =
            PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
                opCtx, preImagesRAII, kNsUUIDOther, minBytesPerMarker);

        CollectionTruncateMarkers::InitialSetOfMarkers expectedInitialMarkers{
            std::deque<CollectionTruncateMarkers::Marker>{
                markerAtBound(kPreImageOtherMin, 1, bytes(kPreImageOtherMin)),
                markerAtBound(kPreImageOther, 1, bytes(kPreImageOther)),
                markerAtBound(kPreImageOtherMax, 1, bytes(kPreImageOtherMax))},
            0 /* leftoverRecordsCount*/,
            0 /* leftoverRecordsBytes */,
            Microseconds{0},
            CollectionTruncateMarkers::MarkersCreationMethod::Scanning};
        ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
    }
}

//
// Tests the generation of 'CollectionTruncateMarkers::InitialSetOfMarkers' for an nsUUID with
// pre-images via samples.
//
// The samples generate an initial set of markers which only accounts for bytes and records
// estimated to fit into whole markers.
class PreImageInitialSetOfMarkersSamplingTest : public CatalogTestFixture,
                                                public ChangeStreamPreImageTestConstants {};

// Documents that if there aren't enough samples to complete a whole marker, an empty
// InitialSetOfMarkers is created with 'creationMethod'
// 'CollectionTruncateMarkers::MarkersCreationMethod::Sampling'.
TEST_F(PreImageInitialSetOfMarkersSamplingTest, InitialMarkersNoFullMarkers1Sample) {
    auto opCtx = operationContext();
    const auto kPreImageUUID = UUID::gen();

    const auto randomSamplesPerMarker = 2;
    const auto estimatedRecordsPerMarker = 4;
    const auto estimatedBytesPerMarker = 100;
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersFromSamples(
            opCtx,
            kPreImageUUID,
            kNsUUID,
            {extractRecordIdAndWallTime(kPreImage1)},
            estimatedRecordsPerMarker,
            estimatedBytesPerMarker,
            randomSamplesPerMarker);

    CollectionTruncateMarkers::InitialSetOfMarkers expectedInitialMarkers{
        std::deque<CollectionTruncateMarkers::Marker>{},
        0 /* leftoverRecordsCount*/,
        0 /* leftoverRecordsBytes */,
        Microseconds{0},
        CollectionTruncateMarkers::MarkersCreationMethod::Sampling};

    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

TEST_F(PreImageInitialSetOfMarkersSamplingTest, InitialMarkersFullMarker1Sample) {
    auto opCtx = operationContext();
    const auto kPreImageUUID = UUID::gen();

    const auto randomSamplesPerMarker = 1;
    const auto estimatedRecordsPerMarker = 2;
    const auto estimatedBytesPerMarker = bytes(kPreImage1) * 2;
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersFromSamples(
            opCtx,
            kPreImageUUID,
            kNsUUID,
            {extractRecordIdAndWallTime(kPreImage1)},
            estimatedRecordsPerMarker,
            estimatedBytesPerMarker,
            randomSamplesPerMarker);

    CollectionTruncateMarkers::InitialSetOfMarkers expectedInitialMarkers{
        std::deque<CollectionTruncateMarkers::Marker>{
            markerAtBound(kPreImage1, estimatedRecordsPerMarker, estimatedBytesPerMarker)},
        0 /* leftoverRecordsCount*/,
        0 /* leftoverRecordsBytes */,
        Microseconds{0},
        CollectionTruncateMarkers::MarkersCreationMethod::Sampling};

    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

// Documents that all samples that don't contribute to a whole marker are ignored in the overall
// bytes and records count in the 'CollectionTruncateMarkers::InitialSetOfMarkers' created from
// sampling.
TEST_F(PreImageInitialSetOfMarkersSamplingTest, InitialMarkersFullMarkerNoTrackedLeftovers) {
    auto opCtx = operationContext();
    const auto kPreImageUUID = UUID::gen();

    const auto randomSamplesPerMarker = 2;
    const auto estimatedRecordsPerMarker = 2;
    const auto estimatedBytesPerMarker = estimatedRecordsPerMarker * bytes(kPreImage1);
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersFromSamples(
            opCtx,
            kPreImageUUID,
            kNsUUID,
            {extractRecordIdAndWallTime(kPreImage1),
             extractRecordIdAndWallTime(kPreImage2),
             extractRecordIdAndWallTime(kPreImage3)},
            estimatedRecordsPerMarker,
            estimatedBytesPerMarker,
            randomSamplesPerMarker);

    CollectionTruncateMarkers::InitialSetOfMarkers expectedInitialMarkers{
        std::deque<CollectionTruncateMarkers::Marker>{
            markerAtBound(kPreImage2, estimatedRecordsPerMarker, estimatedBytesPerMarker)},
        0 /* leftoverRecordsCount*/,
        0 /* leftoverRecordsBytes */,
        Microseconds{0},
        CollectionTruncateMarkers::MarkersCreationMethod::Sampling};

    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}
}  // namespace
}  // namespace mongo
