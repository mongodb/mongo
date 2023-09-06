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

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_stream_options_gen.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/change_stream_pre_images_truncate_markers_per_nsUUID.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/change_streams_cluster_parameter_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/oplog_writer_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/server_parameter_with_storage.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo {

namespace {
std::unique_ptr<ChangeStreamOptions> populateChangeStreamPreImageOptions(
    stdx::variant<std::string, std::int64_t> expireAfterSeconds) {
    PreAndPostImagesOptions preAndPostImagesOptions;
    preAndPostImagesOptions.setExpireAfterSeconds(expireAfterSeconds);

    auto changeStreamOptions = std::make_unique<ChangeStreamOptions>();
    changeStreamOptions->setPreAndPostImages(std::move(preAndPostImagesOptions));

    return changeStreamOptions;
}

void setChangeStreamOptionsToManager(OperationContext* opCtx,
                                     ChangeStreamOptions& changeStreamOptions) {
    auto& changeStreamOptionsManager = ChangeStreamOptionsManager::get(opCtx);
    ASSERT_EQ(changeStreamOptionsManager.setOptions(opCtx, changeStreamOptions).getStatus(),
              ErrorCodes::OK);
}

class ChangeStreamPreImageExpirationPolicyTest : public ServiceContextTest {
public:
    ChangeStreamPreImageExpirationPolicyTest() {
        ChangeStreamOptionsManager::create(getServiceContext());
    }
};

TEST_F(ChangeStreamPreImageExpirationPolicyTest,
       getPreImageOpTimeExpirationDateWithValidIntegralValue) {
    auto opCtx = cc().makeOperationContext();
    const int64_t expireAfterSeconds = 10;

    auto changeStreamOptions = populateChangeStreamPreImageOptions(expireAfterSeconds);
    setChangeStreamOptionsToManager(opCtx.get(), *changeStreamOptions.get());

    auto currentTime = Date_t::now();
    auto receivedExpireAfterSeconds = change_stream_pre_image_util::getPreImageOpTimeExpirationDate(
        opCtx.get(), boost::none /** tenantId **/, currentTime);
    ASSERT(receivedExpireAfterSeconds);
    ASSERT_EQ(*receivedExpireAfterSeconds, currentTime - Seconds(expireAfterSeconds));
}

TEST_F(ChangeStreamPreImageExpirationPolicyTest, getPreImageOpTimeExpirationDateWithUnsetValue) {
    auto opCtx = cc().makeOperationContext();

    auto currentTime = Date_t::now();
    auto receivedExpireAfterSeconds = change_stream_pre_image_util::getPreImageOpTimeExpirationDate(
        opCtx.get(), boost::none /** tenantId **/, currentTime);
    ASSERT_FALSE(receivedExpireAfterSeconds);
}

TEST_F(ChangeStreamPreImageExpirationPolicyTest, getPreImageOpTimeExpirationDateWithOffValue) {
    auto opCtx = cc().makeOperationContext();

    auto changeStreamOptions = populateChangeStreamPreImageOptions("off");
    setChangeStreamOptionsToManager(opCtx.get(), *changeStreamOptions.get());

    auto currentTime = Date_t::now();
    auto receivedExpireAfterSeconds = change_stream_pre_image_util::getPreImageOpTimeExpirationDate(
        opCtx.get(), boost::none /** tenantId **/, currentTime);
    ASSERT_FALSE(receivedExpireAfterSeconds);
}
}  // namespace

class PreImagesRemoverTest : public CatalogTestFixture {
protected:
    const NamespaceString kPreImageEnabledCollection =
        NamespaceString::createNamespaceString_forTest("test.collection");

    // All truncate markers require a creation method. Unless specifically testing the creation
    // method, the creation method is arbitrary and should not impact post-initialisation behavior.
    const CollectionTruncateMarkers::MarkersCreationMethod kArbitraryMarkerCreationMethod{
        CollectionTruncateMarkers::MarkersCreationMethod::Scanning};

    PreImagesRemoverTest() : CatalogTestFixture(Options{}.useMockClock(true)) {
        // Advance the clock to generate valid Timestamp objects. Timestamp objects in this test are
        // generated with the walltime, and Timestamp uses the higher 32 bits of a Date_t as the
        // secs part. Timestamps which have secs == 0 are considered null.
        clockSource()->advance(Milliseconds{int64_t(1) << 32});
    }

    ChangeStreamPreImage generatePreImage(
        const UUID& nsUUID,
        Timestamp ts,
        boost::optional<Date_t> forcedOperationTime = boost::none) {
        auto preImageId = ChangeStreamPreImageId(nsUUID, ts, 0);
        const BSONObj doc = BSON("x" << 1);
        auto operationTime = forcedOperationTime
            ? *forcedOperationTime
            : Date_t::fromDurationSinceEpoch(Seconds{ts.getSecs()});
        return ChangeStreamPreImage(preImageId, operationTime, doc);
    }

    // Populates the pre-images collection with 'numRecords'. Generates pre-images with Timestamps 1
    // millisecond apart starting at 'startOperationTime'.
    void prePopulatePreImagesCollection(boost::optional<TenantId> tenantId,
                                        const NamespaceString& nss,
                                        int64_t numRecords,
                                        Date_t startOperationTime) {
        auto preImagesCollectionNss = NamespaceString::makePreImageCollectionNSS(tenantId);
        auto opCtx = operationContext();
        auto nsUUID = CollectionCatalog::get(opCtx)
                          ->lookupCollectionByNamespace(operationContext(), nss)
                          ->uuid();

        std::vector<ChangeStreamPreImage> preImages;
        for (int64_t i = 0; i < numRecords; i++) {
            preImages.push_back(
                generatePreImage(nsUUID, Timestamp{startOperationTime + Milliseconds{i}}));
        }

        std::vector<InsertStatement> preImageInsertStatements;
        std::transform(preImages.begin(),
                       preImages.end(),
                       std::back_inserter(preImageInsertStatements),
                       [](const auto& preImage) { return InsertStatement{preImage.toBSON()}; });

        AutoGetCollection preImagesCollectionRaii(opCtx, preImagesCollectionNss, MODE_IX);
        ASSERT(preImagesCollectionRaii);
        WriteUnitOfWork wuow(opCtx);
        auto& changeStreamPreImagesCollection = preImagesCollectionRaii.getCollection();

        auto status = collection_internal::insertDocuments(opCtx,
                                                           changeStreamPreImagesCollection,
                                                           preImageInsertStatements.begin(),
                                                           preImageInsertStatements.end(),
                                                           nullptr);
        wuow.commit();
    };

    // Inserts a pre-image into the pre-images collection. The pre-image inserted has a 'ts' of
    // 'preImageTS', and an 'operationTime' of either (1) 'preImageOperationTime', when explicitly
    // specified, or (2) a 'Date_t' derived from the 'preImageTS'.
    void insertPreImage(NamespaceString nss,
                        Timestamp preImageTS,
                        boost::optional<Date_t> preImageOperationTime = boost::none) {
        auto opCtx = operationContext();
        auto uuid = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss)->uuid();
        auto& manager = ChangeStreamPreImagesCollectionManager::get(getServiceContext());
        WriteUnitOfWork wuow(opCtx);
        auto image = generatePreImage(uuid, preImageTS, preImageOperationTime);
        manager.insertPreImage(opCtx, boost::none, image);
        wuow.commit();
    }

    ClockSourceMock* clockSource() {
        return static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

    BSONObj performPass(Milliseconds timeToAdvance) {
        auto clock = clockSource();
        clock->advance(timeToAdvance);
        auto& manager = ChangeStreamPreImagesCollectionManager::get(getServiceContext());
        auto newClient = getServiceContext()->makeClient("");
        AlternativeClientRegion acr(newClient);
        manager.performExpiredChangeStreamPreImagesRemovalPass(&cc());
        return manager.getPurgingJobStats().toBSON();
    }

    void setExpirationTime(Seconds seconds) {
        auto opCtx = operationContext();
        auto& optionsManager = ChangeStreamOptionsManager::get(opCtx);
        auto options = optionsManager.getOptions(opCtx);
        auto preAndPostOptions = options.getPreAndPostImages();
        preAndPostOptions.setExpireAfterSeconds(seconds.count());
        options.setPreAndPostImages(preAndPostOptions);
        invariantStatusOK(optionsManager.setOptions(opCtx, options));
    }

    void setExpirationTime(const TenantId& tenantId, Seconds seconds) {
        auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
        auto* changeStreamsParam =
            clusterParameters
                ->get<ClusterParameterWithStorage<ChangeStreamsClusterParameterStorage>>(
                    "changeStreams");

        auto oldSettings = changeStreamsParam->getValue(tenantId);
        oldSettings.setExpireAfterSeconds(seconds.count());
        changeStreamsParam->setValue(oldSettings, tenantId).ignore();
    }

    RecordId generatePreImageRecordId(Timestamp timestamp) {
        const UUID uuid{UUID::gen()};
        ChangeStreamPreImageId preImageId(uuid, timestamp, 0);
        return change_stream_pre_image_util::toRecordId(preImageId);
    }

    RecordId generatePreImageRecordId(Date_t wallTime) {
        const UUID uuid{UUID::gen()};
        Timestamp timestamp{wallTime};
        ChangeStreamPreImageId preImageId(uuid, timestamp, 0);
        return change_stream_pre_image_util::toRecordId(preImageId);
    }

    bool hasExcessMarkers(OperationContext* opCtx, PreImagesTruncateMarkersPerNsUUID& markers) {
        return markers._hasExcessMarkers(opCtx);
    }

    void setUp() override {
        CatalogTestFixture::setUp();
        ChangeStreamOptionsManager::create(getServiceContext());

        // Set up OpObserver so that the test will append actual oplog entries to the oplog using
        // repl::logOp().
        auto opObserverRegistry =
            dynamic_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        opObserverRegistry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OplogWriterImpl>()));

        auto& manager = ChangeStreamPreImagesCollectionManager::get(getServiceContext());
        manager.createPreImagesCollection(operationContext(), boost::none);

        invariantStatusOK(storageInterface()->createCollection(
            operationContext(), kPreImageEnabledCollection, CollectionOptions{}));
    }

    // Forces the 'lastApplied' Timestamp to be 'targetTimestamp'. The ReplicationCoordinator keeps
    // track of OpTimeAndWallTime for 'lastApplied'. This method exclusively changes the
    // 'opTime.timestamp', but not the other values (term, wallTime, etc) associated with
    // 'lastApplied'.
    void forceLastAppliedTimestamp(Timestamp targetTimestamp) {
        auto replCoord = repl::ReplicationCoordinator::get(operationContext());
        auto lastAppliedOpTimeAndWallTime = replCoord->getMyLastAppliedOpTimeAndWallTime();
        auto newOpTime =
            repl::OpTime(targetTimestamp, lastAppliedOpTimeAndWallTime.opTime.getTerm());
        replCoord->setMyLastAppliedOpTimeAndWallTime(
            repl::OpTimeAndWallTime(newOpTime, lastAppliedOpTimeAndWallTime.wallTime));

        // Verify the Timestamp is set accordingly.
        ASSERT_EQ(replCoord->getMyLastAppliedOpTimeAndWallTime().opTime.getTimestamp(),
                  targetTimestamp);
    }

    // A 'boost::none' tenantId implies a single tenant environment.
    boost::optional<TenantId> nullTenantId() {
        return boost::none;
    }
};

// When 'expireAfterSeconds' is off, defaults to comparing the 'lastRecord's Timestamp of oldest
// marker with the Timestamp of the ealiest oplog entry.
//
// When 'expireAfterSeconds' is on, defaults to comparing the 'lastRecord's wallTime with
// the current time - 'expireAfterSeconds',  which is already tested as a part of the
// ChangeStreamPreImageExpirationPolicyTest.
TEST_F(PreImagesRemoverTest, hasExcessMarkersExpiredAfterSecondsOff) {
    auto opCtx = operationContext();

    // With no explicit 'expireAfterSeconds', excess markers are determined by whether the Timestamp
    // of the 'lastRecord' in the oldest marker is greater than the Timestamp of the earliest oplog
    // entry.
    auto changeStreamOptions = populateChangeStreamPreImageOptions("off");
    setChangeStreamOptionsToManager(opCtx, *changeStreamOptions.get());

    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(opCtx->getServiceContext())->getEarliestOplogTimestamp(opCtx);

    // Ensure that the generated Timestamp associated with the lastRecord of the marker is less than
    // the earliest oplog entry Timestamp.
    auto ts = currentEarliestOplogEntryTs - 1;
    ASSERT_GT(currentEarliestOplogEntryTs, ts);
    auto wallTime = Date_t::fromMillisSinceEpoch(ts.asInt64());
    auto lastRecordId = generatePreImageRecordId(wallTime);

    auto numRecords = 1;
    auto numBytes = 100;
    std::deque<CollectionTruncateMarkers::Marker> initialMarkers{
        {numRecords, numBytes, lastRecordId, wallTime}};

    PreImagesTruncateMarkersPerNsUUID markers(nullTenantId() /* tenantId */,
                                              std::move(initialMarkers),
                                              0,
                                              0,
                                              100,
                                              kArbitraryMarkerCreationMethod);
    bool excessMarkers = hasExcessMarkers(opCtx, markers);
    ASSERT_TRUE(excessMarkers);
}

TEST_F(PreImagesRemoverTest, hasNoExcessMarkersExpiredAfterSecondsOff) {
    auto opCtx = operationContext();

    // With no explicit 'expireAfterSeconds', excess markers are determined by whether the Timestamp
    // of the 'lastRecord' in the oldest marker is greater than the Timestamp of the earliest oplog
    // entry.
    auto changeStreamOptions = populateChangeStreamPreImageOptions("off");
    setChangeStreamOptionsToManager(opCtx, *changeStreamOptions.get());

    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(opCtx->getServiceContext())->getEarliestOplogTimestamp(opCtx);

    // Ensure that the generated Timestamp associated with the lastRecord of the marker is less than
    // the earliest oplog entry Timestamp.
    auto ts = currentEarliestOplogEntryTs + 1;
    ASSERT_LT(currentEarliestOplogEntryTs, ts);
    auto wallTime = Date_t::fromMillisSinceEpoch(ts.asInt64());
    auto lastRecordId = generatePreImageRecordId(wallTime);

    auto numRecords = 1;
    auto numBytes = 100;
    std::deque<CollectionTruncateMarkers::Marker> initialMarkers{
        {numRecords, numBytes, lastRecordId, wallTime}};

    PreImagesTruncateMarkersPerNsUUID markers(nullTenantId() /* tenantId */,
                                              std::move(initialMarkers),
                                              0,
                                              0,
                                              100,
                                              kArbitraryMarkerCreationMethod);
    bool excessMarkers = hasExcessMarkers(opCtx, markers);
    ASSERT_FALSE(excessMarkers);
}

TEST_F(PreImagesRemoverTest, serverlessHasNoExcessMarkers) {
    Seconds expireAfter{1000};
    auto tenantId = change_stream_serverless_helpers::getTenantIdForTesting();
    setExpirationTime(tenantId, expireAfter);

    auto opCtx = operationContext();
    auto wallTime = opCtx->getServiceContext()->getFastClockSource()->now() + Minutes(120);
    auto lastRecordId = generatePreImageRecordId(wallTime);
    auto numRecords = 1;
    auto numBytes = 100;
    std::deque<CollectionTruncateMarkers::Marker> initialMarkers{
        {numRecords, numBytes, lastRecordId, wallTime}};

    PreImagesTruncateMarkersPerNsUUID markers(
        tenantId, std::move(initialMarkers), 0, 0, 100, kArbitraryMarkerCreationMethod);
    bool excessMarkers = hasExcessMarkers(opCtx, markers);
    ASSERT_FALSE(excessMarkers);
}

TEST_F(PreImagesRemoverTest, serverlessHasExcessMarkers) {
    Seconds expireAfter{1};
    auto tenantId = change_stream_serverless_helpers::getTenantIdForTesting();
    setExpirationTime(tenantId, expireAfter);

    auto opCtx = operationContext();
    auto wallTime = opCtx->getServiceContext()->getFastClockSource()->now() - Minutes(120);
    auto lastRecordId = generatePreImageRecordId(wallTime);
    auto numRecords = 1;
    auto numBytes = 100;
    std::deque<CollectionTruncateMarkers::Marker> initialMarkers{
        {numRecords, numBytes, lastRecordId, wallTime}};

    PreImagesTruncateMarkersPerNsUUID markers(
        tenantId, std::move(initialMarkers), 0, 0, 100, kArbitraryMarkerCreationMethod);
    bool excessMarkers = hasExcessMarkers(opCtx, markers);
    ASSERT_TRUE(excessMarkers);
}

TEST_F(PreImagesRemoverTest, RecordIdToPreImageTimstampRetrieval) {
    // Basic case.
    {
        Timestamp ts0(Date_t::now());
        int64_t applyOpsIndex = 0;

        ChangeStreamPreImageId preImageId(UUID::gen(), ts0, applyOpsIndex);
        auto preImageRecordId = change_stream_pre_image_util::toRecordId(preImageId);

        auto ts1 = change_stream_pre_image_util::getPreImageTimestamp(preImageRecordId);
        ASSERT_EQ(ts0, ts1);
    }

    // Min Timestamp.
    {
        Timestamp ts0 = Timestamp::min();
        int64_t applyOpsIndex = 0;

        ChangeStreamPreImageId preImageId(UUID::gen(), ts0, applyOpsIndex);
        auto preImageRecordId = change_stream_pre_image_util::toRecordId(preImageId);

        auto ts1 = change_stream_pre_image_util::getPreImageTimestamp(preImageRecordId);
        ASSERT_EQ(ts0, ts1);
    }

    // Max Timestamp
    {
        Timestamp ts0 = Timestamp::max();
        int64_t applyOpsIndex = 0;

        ChangeStreamPreImageId preImageId(UUID::gen(), ts0, applyOpsIndex);
        auto preImageRecordId = change_stream_pre_image_util::toRecordId(preImageId);

        auto ts1 = change_stream_pre_image_util::getPreImageTimestamp(preImageRecordId);
        ASSERT_EQ(ts0, ts1);
    }

    // Extra large 'applyOpsIndex'.
    //
    // Parsing a RecordId with an underlying KeyString representation into BSON discards type bits.
    // Since the 'applyOpsIndex' is the only field in 'ChangeStreamPreImageId' that requires type
    // bits to generate the original value from KeyString, ensure different numeric values of
    // 'applyOpsIndex' don't impact the Timestamp retrieval.
    {
        Timestamp ts0(Date_t::now());
        int64_t applyOpsIndex = std::numeric_limits<int64_t>::max();

        ChangeStreamPreImageId preImageId(UUID::gen(), ts0, applyOpsIndex);
        auto preImageRecordId = change_stream_pre_image_util::toRecordId(preImageId);

        auto ts1 = change_stream_pre_image_util::getPreImageTimestamp(preImageRecordId);
        ASSERT_EQ(ts0, ts1);
    }

    // Extra large 'applyOpsIndex' with Timestamp::max().
    {
        Timestamp ts0 = Timestamp::max();
        int64_t applyOpsIndex = std::numeric_limits<int64_t>::max();

        ChangeStreamPreImageId preImageId(UUID::gen(), ts0, applyOpsIndex);
        auto preImageRecordId = change_stream_pre_image_util::toRecordId(preImageId);

        auto ts1 = change_stream_pre_image_util::getPreImageTimestamp(preImageRecordId);
        ASSERT_EQ(ts0, ts1);
    }
}

// TODO SERVER-70591: Remove this test as the feature flag will be removed.
TEST_F(PreImagesRemoverTest, EnsureNoMoreInternalScansWithCollectionScans) {
    RAIIServerParameterControllerForTest truncateFeatureFlag{
        "featureFlagUseUnreplicatedTruncatesForDeletions", false};

    auto clock = clockSource();
    insertPreImage(kPreImageEnabledCollection, Timestamp{clock->now()});
    clock->advance(Milliseconds{1});
    insertPreImage(kPreImageEnabledCollection, Timestamp{clock->now()});

    setExpirationTime(Seconds{1});
    // Verify that expiration works as expected.
    auto passStats = performPass(Milliseconds{2000});
    ASSERT_EQ(passStats["totalPass"].numberLong(), 1);
    ASSERT_EQ(passStats["docsDeleted"].numberLong(), 2);
    ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 1);

    // Assert that internal scans do not occur in the old collection scan approach.
    passStats = performPass(Milliseconds{2000});
    ASSERT_EQ(passStats["totalPass"].numberLong(), 2);
    ASSERT_EQ(passStats["docsDeleted"].numberLong(), 2);
    ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 1);
}

TEST_F(PreImagesRemoverTest, EnsureNoMoreInternalScansWithTruncates) {
    RAIIServerParameterControllerForTest minBytesPerMarker{
        "preImagesCollectionTruncateMarkersMinBytes", 1};
    RAIIServerParameterControllerForTest truncateFeatureFlag{
        "featureFlagUseUnreplicatedTruncatesForDeletions", true};

    auto clock = clockSource();
    insertPreImage(kPreImageEnabledCollection, Timestamp{clock->now()});
    clock->advance(Milliseconds{1});
    insertPreImage(kPreImageEnabledCollection, Timestamp{clock->now()});

    setExpirationTime(Seconds{1});
    // Verify that expiration works as expected.
    auto passStats = performPass(Milliseconds{2000});
    ASSERT_EQ(passStats["totalPass"].numberLong(), 1);
    ASSERT_EQ(passStats["docsDeleted"].numberLong(), 2);
    ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 1);

    // Assert that internal scans still occur while the collection exists.
    passStats = performPass(Milliseconds{2000});
    ASSERT_EQ(passStats["totalPass"].numberLong(), 2);
    ASSERT_EQ(passStats["docsDeleted"].numberLong(), 2);
    ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 2);

    // Assert that internal scans don't occur if the collection is dropped and no more documents
    // exist.
    invariantStatusOK(
        storageInterface()->dropCollection(operationContext(), kPreImageEnabledCollection));
    passStats = performPass(Milliseconds{2000});
    ASSERT_EQ(passStats["totalPass"].numberLong(), 3);
    ASSERT_EQ(passStats["docsDeleted"].numberLong(), 2);
    // One more scan occurs after the drop verifying there's no more data and it is safe to ignore
    // in the future.
    ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 3);

    passStats = performPass(Milliseconds{2000});
    ASSERT_EQ(passStats["totalPass"].numberLong(), 4);
    ASSERT_EQ(passStats["docsDeleted"].numberLong(), 2);
    ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 3);
}

TEST_F(PreImagesRemoverTest, EnsureAllDocsEventualyTruncatedFromPrePopulatedCollection) {
    RAIIServerParameterControllerForTest truncateFeatureFlag{
        "featureFlagUseUnreplicatedTruncatesForDeletions", true};

    auto clock = clockSource();
    auto startOperationTime = clock->now();
    auto numRecords = 1000;
    prePopulatePreImagesCollection(
        nullTenantId(), kPreImageEnabledCollection, numRecords, startOperationTime);

    // Advance the clock to align with the most recent pre-image inserted.
    clock->advance(Milliseconds{numRecords});

    // Move the clock further ahead to simulate startup with a collection of expired pre-images.
    clock->advance(Seconds{10});

    setExpirationTime(Seconds{1});

    auto passStats = performPass(Milliseconds{0});
    ASSERT_EQ(passStats["totalPass"].numberLong(), 1);
    ASSERT_EQ(passStats["docsDeleted"].numberLong(), numRecords);
    ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 1);
}

TEST_F(PreImagesRemoverTest, RemoverPassWithTruncateOnEmptyCollection) {
    RAIIServerParameterControllerForTest truncateFeatureFlag{
        "featureFlagUseUnreplicatedTruncatesForDeletions", true};

    setExpirationTime(Seconds{1});

    auto passStats = performPass(Milliseconds{0});
    ASSERT_EQ(passStats["totalPass"].numberLong(), 1);
    ASSERT_EQ(passStats["docsDeleted"].numberLong(), 0);
    ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 0);
}

TEST_F(PreImagesRemoverTest, TruncatesAreOnlyAfterAllDurable) {
    RAIIServerParameterControllerForTest truncateFeatureFlag{
        "featureFlagUseUnreplicatedTruncatesForDeletions", true};
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", 1};

    auto clock = clockSource();
    auto startOperationTime = clock->now();
    auto numRecordsBeforeAllDurableTimestamp = 1000;
    prePopulatePreImagesCollection(nullTenantId(),
                                   kPreImageEnabledCollection,
                                   numRecordsBeforeAllDurableTimestamp,
                                   startOperationTime);

    // Advance the clock to align with the most recent pre-image inserted.
    clock->advance(Milliseconds{numRecordsBeforeAllDurableTimestamp});

    auto allDurableTS = storageInterface()->getAllDurableTimestamp(getServiceContext());

    // Insert a pre-image that would be expired by truncate given its 'ts' is greater than
    // the 'allDurable'. Force the 'operationTime' so the pre-image is expired by it's
    // 'operationTime'.
    insertPreImage(kPreImageEnabledCollection, allDurableTS + 1, clock->now());

    // Pre-images eligible for truncation must have timestamps less than both the 'allDurable' and
    // 'lastApplied' timestamps. In this test case, demonstrate that the 'allDurable' timestamp is
    // respected even if the most recent pre-image 'ts' is less than the 'lastApplied'.
    forceLastAppliedTimestamp(allDurableTS + 2);

    // Force all pre-images to be expired by 'operationTime'.
    clock->advance(Seconds{10});
    setExpirationTime(Seconds{1});

    auto passStats = performPass(Milliseconds{0});
    ASSERT_EQ(passStats["totalPass"].numberLong(), 1);
    ASSERT_EQ(passStats["docsDeleted"].numberLong(), numRecordsBeforeAllDurableTimestamp);
    ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 1);
}

}  // namespace mongo
