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
#include "mongo/db/change_stream_pre_images_truncate_markers_per_nsUUID.h"

#include "mongo/db/change_stream_options_gen.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_pre_image_test_helpers.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/util/clock_source_mock.h"

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
using namespace change_stream_pre_image_test_helper;

BSONObj toBSON(
    const PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers& initialSetOfPreImageMarkers) {
    BSONObjBuilder builder;

    BSONArrayBuilder markersBuilder;
    for (const auto& marker : initialSetOfPreImageMarkers.markers) {
        markersBuilder.append(change_stream_pre_image_test_helper::toBSON(marker));
    }
    builder.appendArray("markers", markersBuilder.arr());
    initialSetOfPreImageMarkers.highestRecordId.serializeToken("highestRecordId", &builder);
    builder.append("highestWallTime", initialSetOfPreImageMarkers.highestWallTime);
    builder.append("leftoverRecordsCount", initialSetOfPreImageMarkers.leftoverRecordsCount);
    builder.append("leftoverRecordsBytes", initialSetOfPreImageMarkers.leftoverRecordsBytes);
    builder.append("methodUsed",
                   CollectionTruncateMarkers::toString(initialSetOfPreImageMarkers.creationMethod));
    return builder.obj();
}

// In order to be expirable with partial marker expiration, a pre-image record must be less than or
// equal to the highest tracked record, and there must be a non-zero bytes and records count across
// the truncate markers.
void assertTracksPreImage(
    const PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers& initialSetOfMarkers,
    const ChangeStreamPreImage& preImage) {
    const auto [preImageRid, preImageWallTime] = extractRecordIdAndWallTime(preImage);
    auto errMsg = [&]() {
        return fmt::format("Initial set of markers: {}, aren't tracking preImage {}",
                           toBSON(initialSetOfMarkers).toString(),
                           preImage.toBSON().toString());
    };
    ASSERT_GTE(initialSetOfMarkers.highestRecordId, preImageRid) << errMsg();
    ASSERT_GTE(initialSetOfMarkers.highestWallTime, preImageWallTime) << errMsg();

    bool hasWholeMarkers = !initialSetOfMarkers.markers.empty();
    bool hasPartialMarkerCount = initialSetOfMarkers.leftoverRecordsCount != 0 &&
        initialSetOfMarkers.leftoverRecordsBytes != 0;
    ASSERT(hasWholeMarkers || hasPartialMarkerCount) << errMsg();
}

//
// Tests the generation of 'PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers' for
// an nsUUID with pre-images via scanning.
//
// Scanning generates an initial set of markers which accounts for all bytes and records for the
// nsUUID scanned by the pre-image collection.
class PreImageInitialSetOfMarkersScanningTest : public CatalogTestFixture,
                                                public ChangeStreamPreImageTestConstants {};

TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersEmptyCollection) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
    int64_t minBytesPerMarker = 4;
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

    auto expectedInitialMarkers = PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers{};
    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersNoFullMarkers1PreImage) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);

    // Large 'minBytesPerMarker' to test 0 markers created.
    int64_t minBytesPerMarker = 10000;
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

    const auto expectedInitialMarkers =
        makeInitialSetOfMarkers({},
                                kPreImage1,
                                1 /* leftoverRecordsCount */,
                                bytes(kPreImage1) /* leftoverRecordsBytes */,
                                CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersFullMarker1PreImage) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);

    // Small 'minBytesPerMarker' to test full marker creation.
    int64_t minBytesPerMarker = 3;
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

    const auto expectedInitialMarkers =
        makeInitialSetOfMarkers({makeWholeMarker(kPreImage1, 1, bytes(kPreImage1))},
                                kPreImage1,
                                0 /* leftoverRecordsCount */,
                                0 /* leftoverRecordsBytes */,
                                CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

// Markers are created when 'minBytesPerMarker' is met or exceeded. Tests that a full marker
// accounts for all records and bytes within its bound - even when the range slightly exceeds
// 'minBytesPerMarker'.
TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersFullMarkerNoLeftovers) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);
    insertDirectlyToPreImagesCollection(opCtx, kPreImage2);
    const auto preImageBytesTotal = bytes(kPreImage1) + bytes(kPreImage2);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);

    int64_t minBytesPerMarker = preImageBytesTotal - 2;
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

    const auto expectedInitialMarkers =
        makeInitialSetOfMarkers({makeWholeMarker(kPreImage2, 2, preImageBytesTotal)},
                                kPreImage2,
                                0 /* leftoverRecordsCount */,
                                0 /* leftoverRecordsBytes */,
                                CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersFullMarkerLeftovers) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);
    insertDirectlyToPreImagesCollection(opCtx, kPreImage2);
    insertDirectlyToPreImagesCollection(opCtx, kPreImage3);

    // Full marker is created with 'preImage2' as its bound.
    const auto bytesPreImage1And2 = bytes(kPreImage1) + bytes(kPreImage2);
    const auto minBytesPerMarker = bytesPreImage1And2 - 2;

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);

    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

    const auto expectedInitialMarkers =
        makeInitialSetOfMarkers({makeWholeMarker(kPreImage2, 2, bytesPreImage1And2)},
                                kPreImage3,
                                1 /* leftoverRecordsCount */,
                                bytes(kPreImage3) /* leftoverRecordsBytes */,
                                CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersManyFullMarkers) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    const auto numPreImages = 20;
    std::vector<ChangeStreamPreImage> preImages(numPreImages);
    const Date_t baseDate = dateFromISOString("2024-01-01T00:00:01.000Z").getValue();
    const Timestamp baseTimestamp = Timestamp(1, 1);
    for (int i = 0; i < numPreImages; i++) {
        // Insert pre-images of uniform size for testing simplicitly.
        preImages[i] = makePreImage(kNsUUID, baseTimestamp + i, baseDate);
        insertDirectlyToPreImagesCollection(opCtx, preImages[i]);
    }

    const auto bytesPerPreImage = bytes(preImages[0]);
    const auto targetPreImagesPerMarker = 7;

    // Subtract a small amount from the target bytes per marker so every 'targetPreImagesPerMarker'
    // pre-image generates a marker.
    const auto minBytesPerMarker = bytesPerPreImage * targetPreImagesPerMarker - 2;

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
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
        expectedWholeMarkersQueue.push_back(makeWholeMarker(
            preImages[preImageIndex], targetPreImagesPerMarker, expectedBytesPerMarker));
    }

    const int32_t expectedLeftoverRecords = numPreImages % targetPreImagesPerMarker;
    const int32_t expectedLeftoverBytes = expectedLeftoverRecords * bytesPerPreImage;
    const ChangeStreamPreImage expectedHighestPreImage = preImages[preImages.size() - 1];
    const auto expectedInitialMarkers =
        makeInitialSetOfMarkers(std::move(expectedWholeMarkersQueue),
                                expectedHighestPreImage,
                                expectedLeftoverRecords,
                                expectedLeftoverBytes,
                                CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

// Tests the extreme bounds of possible pre-images for a given nsUUID.
TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersIncludeMinAndMax) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    insertDirectlyToPreImagesCollection(opCtx, kPreImageMin);
    insertDirectlyToPreImagesCollection(opCtx, kPreImageMax);

    const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);

    // Small 'minBytesPerMarker' to test the minimum pre-image for 'kNsUUID' is accounted for via
    // scanning.
    int64_t minBytesPerMarker = 1;
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

    const auto expectedInitialMarkers =
        makeInitialSetOfMarkers({makeWholeMarker(kPreImageMin, 1, bytes(kPreImageMin)),
                                 makeWholeMarker(kPreImageMax, 1, bytes(kPreImageMax))},
                                kPreImageMax,
                                0 /* leftoverRecordsCount */,
                                0 /* leftoverRecordsBytes */,
                                CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
    ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
}

// Tests that when there are pre-images for multiple nsUUIDs, scanning a specific nsUUID generates
// markers only for the target nsUUID.
TEST_F(PreImageInitialSetOfMarkersScanningTest, InitialMarkersIsolatedPerNsUUID) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    insertDirectlyToPreImagesCollection(opCtx, kPreImageMin);
    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);
    insertDirectlyToPreImagesCollection(opCtx, kPreImageMax);

    insertDirectlyToPreImagesCollection(opCtx, kPreImageOtherMin);
    insertDirectlyToPreImagesCollection(opCtx, kPreImageOther);
    insertDirectlyToPreImagesCollection(opCtx, kPreImageOtherMax);

    // Test that initial markers are isolated for 'kNsUUID'.
    {
        const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
        int64_t minBytesPerMarker = 1;
        const auto actualInitialMarkers =
            PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
                opCtx, preImagesRAII, kNsUUID, minBytesPerMarker);

        const auto expectedInitialMarkers =
            makeInitialSetOfMarkers({makeWholeMarker(kPreImageMin, 1, bytes(kPreImageMin)),
                                     makeWholeMarker(kPreImage1, 1, bytes(kPreImage1)),
                                     makeWholeMarker(kPreImageMax, 1, bytes(kPreImageMax))},
                                    kPreImageMax,
                                    0 /* leftoverRecordsCount */,
                                    0 /* leftoverRecordsBytes */,
                                    CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
        ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
    }

    // Test that initial markers are isolated for 'kNsUUIDOther'.
    {
        const auto preImagesRAII = acquirePreImagesCollectionForRead(opCtx);
        int64_t minBytesPerMarker = 1;
        const auto actualInitialMarkers =
            PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
                opCtx, preImagesRAII, kNsUUIDOther, minBytesPerMarker);

        const auto expectedInitialMarkers = makeInitialSetOfMarkers(
            {makeWholeMarker(kPreImageOtherMin, 1, bytes(kPreImageOtherMin)),
             makeWholeMarker(kPreImageOther, 1, bytes(kPreImageOther)),
             makeWholeMarker(kPreImageOtherMax, 1, bytes(kPreImageOtherMax))},
            kPreImageOtherMax,
            0 /* leftoverRecordsCount */,
            0 /* leftoverRecordsBytes */,
            CollectionTruncateMarkers::MarkersCreationMethod::Scanning);

        ASSERT_BSONOBJ_EQ(toBSON(expectedInitialMarkers), toBSON(actualInitialMarkers));
    }
}

//
// Tests the generation of 'PreImagesTruncateMarkersPerNsUUID::InitialSetOfMarkers' for
// an nsUUID with pre-images via samples.
class PreImageInitialSetOfMarkersSamplingTest : public CatalogTestFixture,
                                                public ChangeStreamPreImageTestConstants {};

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

    assertTracksPreImage(actualInitialMarkers, kPreImage1);
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

    assertTracksPreImage(actualInitialMarkers, kPreImage1);
}

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

    assertTracksPreImage(actualInitialMarkers, kPreImage3);
}

// Exercises a scenario where the samples are for the same pre-image, and the
// 'estimatedRecordsPerMarker' exceed the 'randomSamplesPerMarker'.
TEST_F(PreImageInitialSetOfMarkersSamplingTest, InitialMarkers1RecordManySamples) {
    auto opCtx = operationContext();
    const auto kPreImageUUID = UUID::gen();

    // Assume an incorrect 'avgRecordSize', which is possible if there is 1 large record.
    const auto avgRecordSize = 16777328;
    const auto estimatedRecordsPerMarker = 2;
    const auto estimatedBytesPerMarker = estimatedRecordsPerMarker * avgRecordSize;
    const auto randomSamplesPerMarker = 10;
    const auto actualInitialMarkers =
        PreImagesTruncateMarkersPerNsUUID::createInitialMarkersFromSamples(
            opCtx,
            kPreImageUUID,
            kNsUUID,
            {extractRecordIdAndWallTime(kPreImage1),
             extractRecordIdAndWallTime(kPreImage1),
             extractRecordIdAndWallTime(kPreImage1)},
            estimatedRecordsPerMarker,
            estimatedBytesPerMarker,
            randomSamplesPerMarker);

    assertTracksPreImage(actualInitialMarkers, kPreImage1);
}

class PreImagesPerNsUUIDRefreshHighestTrackedRecord : public CatalogTestFixture,
                                                      public ChangeStreamPreImageTestConstants {
public:
    // Confirms that there is no, and has never been, a highest tracked record for the truncate
    // markers. This should be true iff the the 'nsUUIDTruncateMarkers' were initially created when
    // there were no pre-images for the corresponding 'nsUUID'.
    void assertNoHighestTrackedRecord(
        const PreImagesTruncateMarkersPerNsUUID& nsUUIDTruncateMarkers) {
        ASSERT_TRUE(nsUUIDTruncateMarkers.isEmpty());
        const auto [highestTrackedRid, highestTrackedWallTime] =
            nsUUIDTruncateMarkers.getHighestRecordMetrics_forTest();
        ASSERT(highestTrackedRid.isNull());
        ASSERT_EQ(highestTrackedWallTime, Date_t{});
    }

    // Strict assertions that the 'nsUUIDTruncateMarkers' only track 'preImage' and no other
    // pre-images.
    void assertTracksSinglePreImage(
        OperationContext* opCtx,
        const ChangeStreamPreImage& preImage,
        const PreImagesTruncateMarkersPerNsUUID& nsUUIDTruncateMarkers) {
        auto errMsg = [&]() {
            return fmt::format(
                "Expected markers to only track preImage: {}, but got {}",
                preImage.toBSON().toString(),
                change_stream_pre_image_test_helper::toBSON(nsUUIDTruncateMarkers).toString());
        };

        const auto& [highestTrackedRid, highestTrackedWallTime] =
            nsUUIDTruncateMarkers.getHighestRecordMetrics_forTest();
        const auto [preImageRid, preImageWallTime] = extractRecordIdAndWallTime(preImage);
        ASSERT_EQ(preImageRid, highestTrackedRid) << errMsg();
        ASSERT_EQ(preImageWallTime, highestTrackedWallTime) << errMsg();

        const auto wholeMarkers = nsUUIDTruncateMarkers.getMarkers_forTest();
        const auto partialMarkerBytes = nsUUIDTruncateMarkers.currentBytes_forTest();
        const auto partialMarkerRecords = nsUUIDTruncateMarkers.currentRecords_forTest();
        if (wholeMarkers.empty()) {
            // Tracked by the partial marker.
            ASSERT_EQ(bytes(preImage), partialMarkerBytes) << errMsg();
            ASSERT_EQ(1, partialMarkerRecords) << errMsg();
        } else {
            // Tracked by a whole marker.
            ASSERT_EQ(1, wholeMarkers.size()) << errMsg();
            ASSERT_EQ(0, partialMarkerBytes) << errMsg();
            ASSERT_EQ(0, partialMarkerRecords) << errMsg();
            const auto wholeMarker = wholeMarkers.front();
            ASSERT_EQ(preImageRid, wholeMarker.lastRecord) << errMsg();
            ASSERT_EQ(preImageWallTime, wholeMarker.wallTime) << errMsg();
            ASSERT_EQ(bytes(preImage), wholeMarker.bytes) << errMsg();
            ASSERT_EQ(1, wholeMarker.records) << errMsg();
        }
    }

    // Generates an empty set of NsUUID truncate markers for 'preImage's respective nsUUID
    // and confirms that 'refreshHighestTrackedRecord()' results in the tracking of 'preImage', and
    // 'preImage' only.
    void testRefreshOnEmptyTracksSingleRecord(OperationContext* opCtx,
                                              int64_t minBytesPerMarker,
                                              const ChangeStreamPreImage& preImage) {
        const auto nsUUID = preImage.getId().getNsUUID();
        auto nsUUIDTruncateMarkers = makeEmptyTruncateMarkers(nsUUID, minBytesPerMarker);
        assertNoHighestTrackedRecord(nsUUIDTruncateMarkers);

        {
            const auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
            nsUUIDTruncateMarkers.refreshHighestTrackedRecord(opCtx, preImagesCollection);
        }
        assertTracksSinglePreImage(opCtx, preImage, nsUUIDTruncateMarkers);
    }
};

TEST_F(PreImagesPerNsUUIDRefreshHighestTrackedRecord, InitiallyEmptyBasic) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    const auto minBytesPerMarker = 1;
    auto nsUUIDTruncateMarkers = makeEmptyTruncateMarkers(kNsUUID, minBytesPerMarker);
    assertNoHighestTrackedRecord(nsUUIDTruncateMarkers);

    {
        // Refreshing on an empty pre-images collection does not alter the truncate markers.
        const auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        nsUUIDTruncateMarkers.refreshHighestTrackedRecord(opCtx, preImagesCollection);
        assertNoHighestTrackedRecord(nsUUIDTruncateMarkers);
    }

    insertDirectlyToPreImagesCollection(opCtx, kPreImageOtherMin);
    insertDirectlyToPreImagesCollection(opCtx, kPreImageOther);
    insertDirectlyToPreImagesCollection(opCtx, kPreImageOtherMax);

    {
        // Pre-images are present for 'kNsUUIDOther', but shouldn't effect truncate markers for
        // 'kNsUUID'.
        const auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        nsUUIDTruncateMarkers.refreshHighestTrackedRecord(opCtx, preImagesCollection);
        assertNoHighestTrackedRecord(nsUUIDTruncateMarkers);
    }

    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);

    {
        const auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        nsUUIDTruncateMarkers.refreshHighestTrackedRecord(opCtx, preImagesCollection);
        assertTracksSinglePreImage(opCtx, kPreImage1, nsUUIDTruncateMarkers);
    }
}

TEST_F(PreImagesPerNsUUIDRefreshHighestTrackedRecord, InitiallyEmptyMinBoundary) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    // Only one pre-image: The minimum possible pre-image for 'kNsUUID'.
    insertDirectlyToPreImagesCollection(opCtx, kPreImageMin);

    // 'kPreImageMin' won't fill a whole marker.
    const auto largeMinBytesPerMarker = bytes(kPreImageMin) * 10;
    testRefreshOnEmptyTracksSingleRecord(opCtx, largeMinBytesPerMarker, kPreImageMin);

    // 'kPreImageMin' will fill a whole marker.
    const auto smallMinBytesPerMarker = 1;
    testRefreshOnEmptyTracksSingleRecord(opCtx, smallMinBytesPerMarker, kPreImageMin);

    // Pre-images spanning the bounds of 'kNsUUIDOther'.
    insertDirectlyToPreImagesCollection(opCtx, kPreImageOtherMin);
    insertDirectlyToPreImagesCollection(opCtx, kPreImageOther);
    insertDirectlyToPreImagesCollection(opCtx, kPreImageOtherMax);

    // The presence of pre-images from 'kNsUUIDOther' shouldn't impact truncate markers generated
    // for 'kNsUUID'.
    testRefreshOnEmptyTracksSingleRecord(opCtx, largeMinBytesPerMarker, kPreImageMin);
    testRefreshOnEmptyTracksSingleRecord(opCtx, smallMinBytesPerMarker, kPreImageMin);
}

TEST_F(PreImagesPerNsUUIDRefreshHighestTrackedRecord, InitiallyEmptyMaxBoundary) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    // Only one pre-image: The maximum possible pre-image for 'kNsUUID'.
    insertDirectlyToPreImagesCollection(opCtx, kPreImageMax);

    // 'kPreImageMax' won't fill a whole marker.
    const auto largeMaxBytesPerMarker = bytes(kPreImageMax) * 10;
    testRefreshOnEmptyTracksSingleRecord(opCtx, largeMaxBytesPerMarker, kPreImageMax);

    // 'kPreImageMax' will fill a whole marker.
    const auto smallMaxBytesPerMarker = 1;
    testRefreshOnEmptyTracksSingleRecord(opCtx, smallMaxBytesPerMarker, kPreImageMax);

    // Pre-images spanning the bounds of 'kNsUUIDOther'.
    insertDirectlyToPreImagesCollection(opCtx, kPreImageOtherMin);
    insertDirectlyToPreImagesCollection(opCtx, kPreImageOther);
    insertDirectlyToPreImagesCollection(opCtx, kPreImageOtherMax);

    // The presence of pre-images from 'kNsUUIDOther' shouldn't impact truncate markers generated
    // for 'kNsUUID'.
    testRefreshOnEmptyTracksSingleRecord(opCtx, largeMaxBytesPerMarker, kPreImageMax);
    testRefreshOnEmptyTracksSingleRecord(opCtx, smallMaxBytesPerMarker, kPreImageMax);
}

TEST_F(PreImagesPerNsUUIDRefreshHighestTrackedRecord, RefreshTracksNewInsert) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);
    insertDirectlyToPreImagesCollection(opCtx, kPreImage1);

    const auto minBytesPerMarker = bytes(kPreImage1) + 1;
    const auto initialSetOfMarkers =
        makeInitialSetOfMarkers({},
                                kPreImage1,
                                1 /* leftoverRecordsCount */,
                                bytes(kPreImage1) /* leftoverRecordsBytes */,
                                CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
    PreImagesTruncateMarkersPerNsUUID nsUUIDTruncateMarkers{
        kNsUUID, initialSetOfMarkers, minBytesPerMarker};
    assertTracksSinglePreImage(opCtx, kPreImage1, nsUUIDTruncateMarkers);

    {
        insertDirectlyToPreImagesCollection(opCtx, kPreImage2);

        const auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        nsUUIDTruncateMarkers.refreshHighestTrackedRecord(opCtx, preImagesCollection);
        ASSERT_TRUE(activelyTrackingPreImage(nsUUIDTruncateMarkers, kPreImage2));
    }
}

// PreImagesTruncateMarkersPerNsUUID are generated on data that can be rolled back. Tests that
// 'refreshHighestTrackedRecord()' is a no-op if the pre-image tracked during construction no longer
// exists in the pre-images collection.
TEST_F(PreImagesPerNsUUIDRefreshHighestTrackedRecord, RefreshAfterRollbackOfTrackedRecordIsNoOp) {
    auto opCtx = operationContext();
    createPreImagesCollection(opCtx);

    const auto minBytesPerMarker = bytes(kPreImage1) * 2;
    const auto initialSetOfMarkers =
        makeInitialSetOfMarkers({},
                                kPreImage1,
                                1 /* leftoverRecordsCount */,
                                bytes(kPreImage1) /* leftoverRecordsBytes */,
                                CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
    PreImagesTruncateMarkersPerNsUUID nsUUIDTruncateMarkers{
        kNsUUID, initialSetOfMarkers, minBytesPerMarker};
    assertTracksSinglePreImage(opCtx, kPreImage1, nsUUIDTruncateMarkers);

    {
        // The absence of 'kPreImage1' in the pre-images collection simulates a pre-image visible
        // during the construction of the nsUUID truncate markers, but rolled back prior to their
        // refresh.
        const auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
        nsUUIDTruncateMarkers.refreshHighestTrackedRecord(opCtx, preImagesCollection);
        assertTracksSinglePreImage(opCtx, kPreImage1, nsUUIDTruncateMarkers);
    }
}

// Tests the 'expiry' conditions of pre-image truncate markers. Leverages 'StorageInterfaceMock' for
// fine grain controlled over the perceived earliest oplog entry timestamp and current wall time -
// both of which are used to determine pre-image expiration.
class PreImageTruncateMarkerExpiryTestFixture : public ServiceContextMongoDTest,
                                                public ChangeStreamPreImageTestConstants {
public:
    PreImageTruncateMarkerExpiryTestFixture()
        : ServiceContextMongoDTest(Options{}.useMockClock(true)) {}

    void setUp() override {
        _opCtx = getClient()->makeOperationContext();
        auto service = getServiceContext();
        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceMock>());
        ChangeStreamOptionsManager::create(getServiceContext());

        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));

        // Tests that create / write to the underlying pre-images collection require an oplog to
        // exist when fetching the next opTime (LocalOplogInfo::getNextOpTimes) for a write.
        repl::createOplog(opCtx());
    }

    void tearDown() override {
        _opCtx.reset();
    }

    ClockSourceMock* clockSource() {
        return static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    void forceEarliestOplogTimestamp(Timestamp earliestOplogTimestamp) {
        repl::StorageInterfaceMock* storageInterface = dynamic_cast<repl::StorageInterfaceMock*>(
            repl::StorageInterface::get(getServiceContext()));
        storageInterface->earliestOplogTimestamp = earliestOplogTimestamp;
        ASSERT_EQ(
            earliestOplogTimestamp,
            repl::StorageInterface::get(getServiceContext())->getEarliestOplogTimestamp(opCtx()));
    }

    // Pre-images are expired either by the earliest oplog timestamp or by time expiry.
    //
    // Given 'preImage', ensures the pre-image isn't expired.
    void forceNotExpired(const ChangeStreamPreImage& preImage,
                         const std::variant<std::string, std::int64_t>& expireAfterSecondsOrOff) {
        auto changeStreamOptions = populateChangeStreamPreImageOptions(expireAfterSecondsOrOff);
        setChangeStreamOptionsToManager(opCtx(), *changeStreamOptions.get());
        boost::optional<Seconds> expireAfterSeconds =
            change_stream_pre_image_util::getExpireAfterSeconds(opCtx());
        if (expireAfterSeconds) {
            // Set back the clock to before the pre-image wall time, to guarantee it is not yet
            // expired.
            const auto [_, preImageWallTime] = extractRecordIdAndWallTime(preImage);
            clockSource()->reset(preImageWallTime);
            ASSERT_LT(clockSource()->now(), preImageWallTime + *expireAfterSeconds);
        }

        // Regardless of 'expireAfterSeconds', pre-images expire when older than the earliest oplog
        // timestamp.
        const Timestamp olderThanTS = preImage.getId().getTs() - 1;
        forceEarliestOplogTimestamp(olderThanTS);
    }

    // Expire the pre-image according to the 'expireAfterSeconds' type.
    //
    // An integer 'expireAfterSeconds' expires the pre-image according to wall time.
    void expirePreImage(const ChangeStreamPreImage& preImage, int64_t expireAfterSeconds) {
        const auto [_, preImageWallTime] = extractRecordIdAndWallTime(preImage);
        clockSource()->reset(preImageWallTime + Seconds(expireAfterSeconds));
    }

    // The only valid 'expireAfterSeconds' of type string is 'off'.
    //
    // When 'expireAfterSeconds' is 'off', expires the pre-image according to the earliest oplog
    // entry.
    void expirePreImage(const ChangeStreamPreImage& preImage, std::string expireAfterSecondsOff) {
        const Timestamp newerThanTS = preImage.getId().getTs() + 1;
        forceEarliestOplogTimestamp(newerThanTS);
    }

    void assertNoExpiredMarker(PreImagesTruncateMarkersPerNsUUID& truncateMarkers) {
        const auto expiredMarker = truncateMarkers.peekOldestMarkerIfNeeded(opCtx());
        ASSERT(!expiredMarker);
    }

    // Confirms that the oldest marker in 'truncateMarkers' is both expired and tracks a range that
    // includes 'preImage'.
    void assertExpiredMarker(const ChangeStreamPreImage& preImage,
                             PreImagesTruncateMarkersPerNsUUID& truncateMarkers) {
        ASSERT_FALSE(truncateMarkers.isEmpty());
        const auto expiredMarker = truncateMarkers.peekOldestMarkerIfNeeded(opCtx());
        ASSERT(expiredMarker);

        auto [preImageRid, preImageWallTime] = extractRecordIdAndWallTime(preImage);
        ASSERT_LTE(preImageRid, expiredMarker->lastRecord);
        ASSERT_LTE(preImageWallTime, expiredMarker->wallTime);
    }

    void assertEmptyTruncateMarkers(PreImagesTruncateMarkersPerNsUUID& truncateMarkers) {
        ASSERT_TRUE(truncateMarkers.isEmpty());

        // Empty implies no full markers.
        assertNoExpiredMarker(truncateMarkers);

        // Empty implies no partial marker to upgrade.
        truncateMarkers.createPartialMarkerIfNecessary(opCtx());
        assertNoExpiredMarker(truncateMarkers);

        // The state is unchanged despite trying to upgrade a partial marker.
        ASSERT_TRUE(truncateMarkers.isEmpty());
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

// Encapsulates test cases for pre-image nsUUID truncate marker expiration compatible with both time
// and earliest oplog timestamp expiry.
//
// Utilizes CRTP so an 'ExpirySpecifcTest' can control the 'expireAfterSeconds' run with each
// test case. This is especially useful since 'expireAfterSeconds' can be of type std::string
// ('off') or type int and all test cases rely on the
// 'PreImageTruncateMarkerExpiryTestFixture'.
template <class ExpirySpecificTest, class ExpireAfterSecondsType>
class PreImageTruncateMarkerExpiryTestCommon : public PreImageTruncateMarkerExpiryTestFixture {
public:
    ExpireAfterSecondsType getExpireAfterSeconds() {
        return expirySpecificImpl().getExpireAfterSeconds();
    }

    // Tests a pre-image is tracked after 'refreshHighestTrackedRecord()' and the pre-image is
    // expirable.
    void testExpiryAfterrefreshHighestTrackedRecord() {
        const auto expireAfterSeconds = getExpireAfterSeconds();
        createPreImagesCollection(opCtx());

        const auto minBytesPerMarker = 1;
        auto truncateMarkers = makeEmptyTruncateMarkers(kNsUUID, minBytesPerMarker);
        assertEmptyTruncateMarkers(truncateMarkers);

        // The pre-image is inserted into the underlying collection without updating the truncate
        // markers.
        insertDirectlyToPreImagesCollection(opCtx(), kPreImage1);
        assertEmptyTruncateMarkers(truncateMarkers);

        {
            const auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx());
            truncateMarkers.refreshHighestTrackedRecord(opCtx(), preImagesCollection);
        }

        ASSERT_FALSE(truncateMarkers.isEmpty());

        // Test that an unexpired, but tracked, record doesn't yield an expired whole marker or
        // partial marker eligible for upgrade.
        forceNotExpired(kPreImage1, expireAfterSeconds);
        assertNoExpiredMarker(truncateMarkers);
        truncateMarkers.createPartialMarkerIfNecessary(opCtx());
        assertNoExpiredMarker(truncateMarkers);

        // When eligible, the truncate markers acknowledge there is an expired marker.
        expirePreImage(kPreImage1, expireAfterSeconds);
        assertExpiredMarker(kPreImage1, truncateMarkers);

        truncateMarkers.popOldestMarker();
        assertEmptyTruncateMarkers(truncateMarkers);
    }

    void testExpiryOneRecordOneWholeMarker() {
        const auto expireAfterSeconds = getExpireAfterSeconds();

        // A 'minBytesPerMarker' smaller than 'kPreImage1' so the insertion of 'kPreImage1'
        // generates a full marker.
        const auto minBytesPerMarker = 1;
        auto truncateMarkers = makeEmptyTruncateMarkers(kNsUUID, minBytesPerMarker);
        assertEmptyTruncateMarkers(truncateMarkers);

        forceNotExpired(kPreImage1, expireAfterSeconds);
        updateMarkers(kPreImage1, truncateMarkers);
        ASSERT_FALSE(truncateMarkers.isEmpty());

        // Confirm neither the whole marker or partial marker are expired.
        const auto numWholeMarkersBefore = truncateMarkers.numMarkers_forTest();
        assertNoExpiredMarker(truncateMarkers);
        truncateMarkers.createPartialMarkerIfNecessary(opCtx());
        const auto numWholeMarkersAfter = truncateMarkers.numMarkers_forTest();
        ASSERT_EQ(numWholeMarkersBefore, numWholeMarkersAfter);

        expirePreImage(kPreImage1, expireAfterSeconds);

        assertExpiredMarker(kPreImage1, truncateMarkers);

        truncateMarkers.popOldestMarker();
        assertEmptyTruncateMarkers(truncateMarkers);
    }

    void testExpiryOneRecordPartialMarker() {
        const auto expireAfterSeconds = getExpireAfterSeconds();

        // A 'minBytesPerMarker' greater than 'kPreImage1's size to ensure inserting 'kPreImage1'
        // doesn't generate a full marker automatically.
        const auto minBytesPerMarker = bytes(kPreImage1) * 4;
        auto truncateMarkers = makeEmptyTruncateMarkers(kNsUUID, minBytesPerMarker);
        assertEmptyTruncateMarkers(truncateMarkers);

        forceNotExpired(kPreImage1, expireAfterSeconds);
        updateMarkers(kPreImage1, truncateMarkers);

        // Tracking 1 unexpired record.
        ASSERT_FALSE(truncateMarkers.isEmpty());
        assertNoExpiredMarker(truncateMarkers);

        expirePreImage(kPreImage1, expireAfterSeconds);

        // Although the record is expired, it is only tracked in by the partial marker.
        ASSERT_FALSE(truncateMarkers.isEmpty());
        assertNoExpiredMarker(truncateMarkers);

        // Force an upgrade from the partial marker to a whole marker.
        truncateMarkers.createPartialMarkerIfNecessary(opCtx());
        assertExpiredMarker(kPreImage1, truncateMarkers);

        truncateMarkers.popOldestMarker();

        assertEmptyTruncateMarkers(truncateMarkers);
    }

    // Tests that an pre-image tracked by the 'InitialSetOfMarkers' is expirable.
    void testExpiryNonEmptyInitialMarkersBasic() {
        const auto expireAfterSeconds = getExpireAfterSeconds();

        const auto minBytesPerMarker = 1;
        const auto initialMarkers = makeInitialSetOfMarkers(
            {makeWholeMarker(kPreImage1, 1 /* records */, bytes(kPreImage1))},
            kPreImage1,
            0 /* leftoverRecordsCount */,
            0 /* leftoverRecordsBytes */,
            CollectionTruncateMarkers::MarkersCreationMethod::Scanning);

        PreImagesTruncateMarkersPerNsUUID truncateMarkers(
            kNsUUID, std::move(initialMarkers), minBytesPerMarker);

        ASSERT_FALSE(truncateMarkers.isEmpty());

        // Tracking 1 unexpired record.
        forceNotExpired(kPreImage1, expireAfterSeconds);
        assertNoExpiredMarker(truncateMarkers);

        expirePreImage(kPreImage1, expireAfterSeconds);

        assertExpiredMarker(kPreImage1, truncateMarkers);

        truncateMarkers.popOldestMarker();

        assertEmptyTruncateMarkers(truncateMarkers);
    }

private:
    ExpirySpecificTest& expirySpecificImpl() {
        return *static_cast<ExpirySpecificTest*>(this);
    }
};

class PreImageTruncateMarkerExpiryByTimeTest
    : public PreImageTruncateMarkerExpiryTestCommon<PreImageTruncateMarkerExpiryByTimeTest, int> {
public:
    int getExpireAfterSeconds() {
        return 100;
    };
};

// Tests pre-image truncate marker expiry where pre-image expiration is determined by the
// earliest oplog entry.
class PreImageTruncateMarkerExpiryByOplogTest
    : public PreImageTruncateMarkerExpiryTestCommon<PreImageTruncateMarkerExpiryByOplogTest,
                                                    std::string> {
public:
    std::string getExpireAfterSeconds() {
        return "off";
    };
};

TEST_F(PreImageTruncateMarkerExpiryByOplogTest, expiryByOplogAfterrefreshHighestTrackedRecord) {
    testExpiryAfterrefreshHighestTrackedRecord();
}
TEST_F(PreImageTruncateMarkerExpiryByTimeTest, expiryByTimeAfterrefreshHighestTrackedRecord) {
    testExpiryAfterrefreshHighestTrackedRecord();
}

TEST_F(PreImageTruncateMarkerExpiryByOplogTest, expiryByOplogOneRecordOneWholeMarker) {
    testExpiryOneRecordOneWholeMarker();
}
TEST_F(PreImageTruncateMarkerExpiryByTimeTest, expiryByTimeOneRecordOneWholeMarker) {
    testExpiryOneRecordOneWholeMarker();
}

TEST_F(PreImageTruncateMarkerExpiryByOplogTest, expiryByOplogOneRecordPartialMarker) {
    testExpiryOneRecordPartialMarker();
}
TEST_F(PreImageTruncateMarkerExpiryByTimeTest, expiryByTimeOneRecordPartialMarker) {
    testExpiryOneRecordPartialMarker();
}

TEST_F(PreImageTruncateMarkerExpiryByOplogTest, expiryByOplogNonEmptyInitialMarkersBasic) {
    testExpiryNonEmptyInitialMarkersBasic();
}
TEST_F(PreImageTruncateMarkerExpiryByTimeTest, expiryByTimeNonEmptyInitialMarkersBasic) {
    testExpiryNonEmptyInitialMarkersBasic();
}

}  // namespace
}  // namespace mongo
