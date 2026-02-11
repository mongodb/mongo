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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/change_stream_pre_image_test_helpers.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_expired_pre_image_remover.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
using namespace change_stream_pre_image_test_helper;

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
    auto receivedExpireAfterSeconds =
        change_stream_pre_image_util::getPreImageOpTimeExpirationDate(opCtx.get(), currentTime);
    ASSERT(receivedExpireAfterSeconds);
    ASSERT_EQ(*receivedExpireAfterSeconds, currentTime - Seconds(expireAfterSeconds));
}

TEST_F(ChangeStreamPreImageExpirationPolicyTest, getPreImageOpTimeExpirationDateWithUnsetValue) {
    auto opCtx = cc().makeOperationContext();

    auto currentTime = Date_t::now();
    auto receivedExpireAfterSeconds =
        change_stream_pre_image_util::getPreImageOpTimeExpirationDate(opCtx.get(), currentTime);
    ASSERT_FALSE(receivedExpireAfterSeconds);
}

TEST_F(ChangeStreamPreImageExpirationPolicyTest, getPreImageOpTimeExpirationDateWithOffValue) {
    auto opCtx = cc().makeOperationContext();

    auto changeStreamOptions = populateChangeStreamPreImageOptions("off");
    setChangeStreamOptionsToManager(opCtx.get(), *changeStreamOptions.get());

    auto currentTime = Date_t::now();
    auto receivedExpireAfterSeconds =
        change_stream_pre_image_util::getPreImageOpTimeExpirationDate(opCtx.get(), currentTime);
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
        auto operationTime = forcedOperationTime
            ? *forcedOperationTime
            : Date_t::fromDurationSinceEpoch(Seconds{ts.getSecs()});
        return makePreImage(nsUUID, ts, operationTime);
    }

    // Populates the pre-images collection with 'numRecords'. Generates pre-images with Timestamps 1
    // millisecond apart starting at 'startOperationTime'.
    void prePopulatePreImagesCollection(const NamespaceString& nss,
                                        int64_t numRecords,
                                        Date_t startOperationTime) {
        auto preImagesCollectionNss = NamespaceString::kChangeStreamPreImagesNamespace;
        auto opCtx = operationContext();
        auto nsUUID = CollectionCatalog::get(opCtx)
                          ->lookupCollectionByNamespace(operationContext(), nss)
                          ->uuid();

        std::vector<BSONObj> preImageDocs;
        preImageDocs.reserve(numRecords);
        for (int64_t i = 0; i < numRecords; i++) {
            preImageDocs.push_back(
                generatePreImage(nsUUID, Timestamp{startOperationTime + Milliseconds{i}}).toBSON());
        }

        AutoGetCollection preImagesCollectionRaii(opCtx, preImagesCollectionNss, MODE_IX);
        ASSERT(preImagesCollectionRaii);

        WriteUnitOfWork wuow(opCtx);
        auto& changeStreamPreImagesCollection = *preImagesCollectionRaii;

        ASSERT_OK(Helpers::insert(opCtx, changeStreamPreImagesCollection, preImageDocs));
        wuow.commit();
    }

    int countPreImagesForNsUUID(OperationContext* opCtx, UUID const& nsUUID) {
        auto preImagesCollectionUUID =
            CollectionCatalog::get(opCtx)
                ->lookupCollectionByNamespace(operationContext(),
                                              NamespaceString::kChangeStreamPreImagesNamespace)
                ->uuid();

        CollectionAcquisition preImagesCollection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(
                NamespaceStringOrUUID{NamespaceString::kChangeStreamPreImagesNamespace.dbName(),
                                      preImagesCollectionUUID},
                PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                repl::ReadConcernArgs::get(opCtx),
                AcquisitionPrerequisites::kRead),
            MODE_IS);

        int count = 0;
        auto cursor = preImagesCollection.getCollectionPtr()->getCursor(opCtx, true /* forward */);

        RecordId seekTo =
            change_stream_pre_image_util::getAbsoluteMinPreImageRecordIdBoundForNs(nsUUID)
                .recordId();

        boost::optional<Record> record =
            cursor->seek(seekTo, SeekableRecordCursor::BoundInclusion::kInclude);
        while (record) {
            const BSONObj preImageObj = record->data.toBson();

            if (nsUUID != change_stream_pre_image_util::getPreImageNsUUID(preImageObj)) {
                // Record is already for a different 'nsUUID' value. Abort sampling.
                break;
            }
            record = cursor->next();
            ++count;
        }
        return count;
    }

    // Inserts a pre-image into the pre-images collection. The pre-image inserted has a 'ts' of
    // 'preImageTS', and an 'operationTime' of either (1) 'preImageOperationTime', when
    // explicitly specified, or (2) a 'Date_t' derived from the 'preImageTS'.
    void insertPreImage(const NamespaceString& nss,
                        Timestamp preImageTS,
                        boost::optional<Date_t> preImageOperationTime = boost::none) {
        auto opCtx = operationContext();
        auto uuid = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss)->uuid();
        auto& manager = ChangeStreamPreImagesCollectionManager::get(getServiceContext());

        WriteUnitOfWork wuow(opCtx);
        auto image = generatePreImage(uuid, preImageTS, preImageOperationTime);
        manager.insertPreImage(opCtx, image);
        wuow.commit();
    }

    ClockSourceMock* clockSource() {
        return static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

    BSONObj performPass(Milliseconds timeToAdvance) {
        auto clock = clockSource();
        clock->advance(timeToAdvance);
        auto& manager = ChangeStreamPreImagesCollectionManager::get(getServiceContext());
        auto newClient = getServiceContext()->getService()->makeClient("");
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

    void setUp() override {
        CatalogTestFixture::setUp();
        ChangeStreamOptionsManager::create(getServiceContext());

        // Set up OpObserver so that the test will append actual oplog entries to the oplog using
        // repl::logOp().
        auto opObserverRegistry =
            dynamic_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        opObserverRegistry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));

        auto& manager = ChangeStreamPreImagesCollectionManager::get(getServiceContext());
        manager.createPreImagesCollection(operationContext());

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
        replCoord->setMyLastAppliedOpTimeAndWallTimeForward(
            repl::OpTimeAndWallTime(newOpTime, lastAppliedOpTimeAndWallTime.wallTime));

        // Verify the Timestamp is set accordingly.
        ASSERT_EQ(replCoord->getMyLastAppliedOpTimeAndWallTime().opTime.getTimestamp(),
                  targetTimestamp);
    }

    // This test is executed twice, with and without replicated truncates.
    void testEnsureNoMoreInternalScansWithTruncates(long long expectedDocsDeleted) {
        RAIIServerParameterControllerForTest minBytesPerMarker{
            "preImagesCollectionTruncateMarkersMinBytes", 1};

        auto uuid =
            CollectionCatalog::get(operationContext())
                ->lookupCollectionByNamespace(operationContext(), kPreImageEnabledCollection)
                ->uuid();

        auto clock = clockSource();
        insertPreImage(kPreImageEnabledCollection, Timestamp{clock->now()});
        clock->advance(Milliseconds{1});
        insertPreImage(kPreImageEnabledCollection, Timestamp{clock->now()});

        ASSERT_EQ(2, countPreImagesForNsUUID(operationContext(), uuid));

        setExpirationTime(Seconds{1});
        // Verify that expiration works as expected.
        auto passStats = performPass(Milliseconds{2000});
        ASSERT_EQ(passStats["totalPass"].numberLong(), 1);
        ASSERT_EQ(passStats["docsDeleted"].numberLong(), expectedDocsDeleted);
        ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 1);

        ASSERT_EQ(0, countPreImagesForNsUUID(operationContext(), uuid));

        // Assert that internal scans still occur while the collection exists.
        passStats = performPass(Milliseconds{2000});
        ASSERT_EQ(passStats["totalPass"].numberLong(), 2);
        ASSERT_EQ(passStats["docsDeleted"].numberLong(), expectedDocsDeleted);
        ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 2);

        // Assert that internal scans don't occur if the collection is dropped and no more documents
        // exist.
        invariantStatusOK(
            storageInterface()->dropCollection(operationContext(), kPreImageEnabledCollection));
        passStats = performPass(Milliseconds{2000});
        ASSERT_EQ(passStats["totalPass"].numberLong(), 3);
        ASSERT_EQ(passStats["docsDeleted"].numberLong(), expectedDocsDeleted);
        // One more scan occurs after the drop verifying there's no more data and it is safe to
        // ignore in the future.
        ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 3);

        passStats = performPass(Milliseconds{2000});
        ASSERT_EQ(passStats["totalPass"].numberLong(), 4);
        ASSERT_EQ(passStats["docsDeleted"].numberLong(), expectedDocsDeleted);
        ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 3);
    }

    // This test is executed twice, with and without replicated truncates.
    void testEnsureAllDocsEventuallyTruncatedFromPrePopulatedCollection(
        long long expectedDocsDeleted) {
        auto uuid =
            CollectionCatalog::get(operationContext())
                ->lookupCollectionByNamespace(operationContext(), kPreImageEnabledCollection)
                ->uuid();

        auto clock = clockSource();
        auto startOperationTime = clock->now();
        auto numRecords = 1000;
        prePopulatePreImagesCollection(kPreImageEnabledCollection, numRecords, startOperationTime);

        ASSERT_EQ(numRecords, countPreImagesForNsUUID(operationContext(), uuid));

        // Advance the clock to align with the most recent pre-image inserted.
        clock->advance(Milliseconds{numRecords});

        // Move the clock further ahead to simulate startup with a collection of expired pre-images.
        clock->advance(Seconds{10});

        setExpirationTime(Seconds{1});

        auto passStats = performPass(Milliseconds{0});
        ASSERT_EQ(passStats["totalPass"].numberLong(), 1);
        ASSERT_EQ(passStats["docsDeleted"].numberLong(), expectedDocsDeleted);
        ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 1);

        ASSERT_EQ(0, countPreImagesForNsUUID(operationContext(), uuid));
    }

    // This test is executed twice, with and without replicated truncates.
    void testTruncatesAreOnlyAfterAllDurable(long long expectedDocsDeleted) {
        RAIIServerParameterControllerForTest minBytesPerMarkerController{
            "preImagesCollectionTruncateMarkersMinBytes", 1};

        auto uuid =
            CollectionCatalog::get(operationContext())
                ->lookupCollectionByNamespace(operationContext(), kPreImageEnabledCollection)
                ->uuid();

        auto clock = clockSource();
        auto startOperationTime = clock->now();
        auto numRecordsBeforeAllDurableTimestamp = 1000;
        prePopulatePreImagesCollection(
            kPreImageEnabledCollection, numRecordsBeforeAllDurableTimestamp, startOperationTime);

        ASSERT_EQ(numRecordsBeforeAllDurableTimestamp,
                  countPreImagesForNsUUID(operationContext(), uuid));

        // Advance the clock to align with the most recent pre-image inserted.
        clock->advance(Milliseconds{numRecordsBeforeAllDurableTimestamp});

        auto allDurableTS = storageInterface()->getAllDurableTimestamp(getServiceContext());

        // Insert a pre-image that would be expired by truncate given its 'ts' is greater than the
        // 'allDurableTS'. Force the 'operationTime' so the pre-image is expired by its
        // 'operationTime'.
        insertPreImage(kPreImageEnabledCollection, allDurableTS + 1, clock->now());

        ASSERT_EQ(numRecordsBeforeAllDurableTimestamp + 1,
                  countPreImagesForNsUUID(operationContext(), uuid));

        // Pre-images eligible for truncation must have timestamps less than both the 'allDurable'
        // and 'lastApplied' timestamps. In this test case, demonstrate that the 'allDurable'
        // timestamp is respected even if the most recent pre-image 'ts' is less than the
        // 'lastApplied'.
        forceLastAppliedTimestamp(allDurableTS + 2);

        // Force all pre-images to be expired by 'operationTime'.
        clock->advance(Seconds{10});
        setExpirationTime(Seconds{1});

        auto passStats = performPass(Milliseconds{0});
        ASSERT_EQ(passStats["maxTimestampEligibleForTruncate"].timestamp(), allDurableTS);
        ASSERT_EQ(passStats["totalPass"].numberLong(), 1);
        ASSERT_EQ(passStats["docsDeleted"].numberLong(), expectedDocsDeleted);
        ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 1);

        ASSERT_EQ(1, countPreImagesForNsUUID(operationContext(), uuid));
    }
};

TEST_F(PreImagesRemoverTest, RecordIdToPreImageTimestampRetrieval) {
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

// Run test with local truncates.
TEST_F(PreImagesRemoverTest, EnsureNoMoreInternalScansWithLocalTruncates) {
    RAIIServerParameterControllerForTest featureFlagScope{
        "featureFlagUseReplicatedTruncatesForDeletions", false};
    testEnsureNoMoreInternalScansWithTruncates(2 /* expectedDocsDeleted */);
}

// Run test with replicated truncates.
TEST_F(PreImagesRemoverTest, EnsureNoMoreInternalScansWithReplicatedTruncates) {
    RAIIServerParameterControllerForTest featureFlagScope{
        "featureFlagUseReplicatedTruncatesForDeletions", true};
    testEnsureNoMoreInternalScansWithTruncates(3 /* expectedDocsDeleted */);
}

TEST_F(PreImagesRemoverTest,
       EnsureAllDocsEventuallyTruncatedFromPrePopulatedCollectionLocalTruncates) {
    RAIIServerParameterControllerForTest featureFlagScope{
        "featureFlagUseReplicatedTruncatesForDeletions", false};
    testEnsureAllDocsEventuallyTruncatedFromPrePopulatedCollection(1000 /* expectedDocsDeleted */);
}

TEST_F(PreImagesRemoverTest,
       EnsureAllDocsEventuallyTruncatedFromPrePopulatedCollectionReplicatedTruncates) {
    RAIIServerParameterControllerForTest featureFlagScope{
        "featureFlagUseReplicatedTruncatesForDeletions", true};
    // Note: the expected value here is very inaccurate, but this is due to no size information
    // being used when estimating the number of documents in the truncate markers.
    testEnsureAllDocsEventuallyTruncatedFromPrePopulatedCollection(
        360448 /* expectedDocsDeleted */);
}

TEST_F(PreImagesRemoverTest, RemoverPassWithTruncateOnEmptyCollectionLocalTruncates) {
    RAIIServerParameterControllerForTest featureFlagScope{
        "featureFlagUseReplicatedTruncatesForDeletions", false};

    setExpirationTime(Seconds{1});

    auto passStats = performPass(Milliseconds{0});
    ASSERT_EQ(passStats["totalPass"].numberLong(), 1);
    ASSERT_EQ(passStats["docsDeleted"].numberLong(), 0);
    ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 0);
}

TEST_F(PreImagesRemoverTest, RemoverPassWithTruncateOnEmptyCollectionReplicatesTruncates) {
    RAIIServerParameterControllerForTest featureFlagScope{
        "featureFlagUseReplicatedTruncatesForDeletions", true};

    setExpirationTime(Seconds{1});

    auto passStats = performPass(Milliseconds{0});
    ASSERT_EQ(passStats["totalPass"].numberLong(), 1);
    ASSERT_EQ(passStats["docsDeleted"].numberLong(), 0);
    ASSERT_EQ(passStats["scannedInternalCollections"].numberLong(), 0);
}

TEST_F(PreImagesRemoverTest, TruncatesAreOnlyAfterAllDurableLocalTruncates) {
    RAIIServerParameterControllerForTest featureFlagScope{
        "featureFlagUseReplicatedTruncatesForDeletions", false};
    testTruncatesAreOnlyAfterAllDurable(1000 /* expectedDocsDeleted */);
}

// This test will currently fail when enabling the feature flag for replicated truncates.
// The reason is that the sampling algorithm that is used for sampling documents in the pre-images
// collection when the feature flag is enabled will not find enough of the existing documents. The
// test creates many documents with Timestamps close to Timestamp(1, 2001) and then bumps the
// majority-commited op time to Timestamp(4294969, 2) and creates another document.
// The range of timestamps in the pre-images collection thus is very large, but the distribution of
// documents in the range is very skewed. The equal range sampling won't consider most of the
// documents, which makes the test fail.
// TODO SERVER-119222: Reconsider the sampling algorithm and re-enable this test.
// TEST_F(PreImagesRemoverTest, TruncatesAreOnlyAfterAllDurableReplicatedTruncates) {
//     RAIIServerParameterControllerForTest featureFlagScope{
//         "featureFlagUseReplicatedTruncatesForDeletions", true};
//     testTruncatesAreOnlyAfterAllDurable(1 /* expectedDocsDeleted */);
// }

/**
 * Tests the conditions under which the ChangeStreamExpiredPreImagesRemoverService starts
 * periodic pre-image removal.
 */
class PreImagesRemoverServiceTest : service_context_test::WithSetupTransportLayer,
                                    public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        // Simulates a standard server startup.
        preImageRemoverService(operationContext())->onStartup(operationContext());
    }

    void tearDown() override {
        // Guarantee the pre-image removal job, if initialized, is shutdown so there is no active
        // client during ServiceContext destruction.
        preImageRemoverService(operationContext())->onShutdown();
        CatalogTestFixture::tearDown();
    }

    ChangeStreamExpiredPreImagesRemoverService* preImageRemoverService(OperationContext* opCtx) {
        return ChangeStreamExpiredPreImagesRemoverService::get(opCtx);
    }
};

TEST_F(PreImagesRemoverServiceTest, PeriodicJobStartsWithRollbackFalse) {
    auto opCtx = operationContext();
    auto preImageRemoverService = ChangeStreamExpiredPreImagesRemoverService::get(opCtx);

    ASSERT_FALSE(preImageRemoverService->startedPeriodicJob_forTest());

    preImageRemoverService->onConsistentDataAvailable(
        opCtx, false /* isMajority */, false /* isRollback */);

    ASSERT_TRUE(preImageRemoverService->startedPeriodicJob_forTest());
}

TEST_F(PreImagesRemoverServiceTest, PeriodicJobDoesNotStartWhenRollbackTrue) {
    auto opCtx = operationContext();
    auto preImageRemoverService = ChangeStreamExpiredPreImagesRemoverService::get(opCtx);

    ASSERT_FALSE(preImageRemoverService->startedPeriodicJob_forTest());

    preImageRemoverService->onConsistentDataAvailable(
        opCtx, false /* isMajority */, true /* isRollback */);
    ASSERT_FALSE(preImageRemoverService->startedPeriodicJob_forTest());

    // Test a second call doesn't start up the periodic remover, since 'isRollback' true may be
    // called multiple times throughout the lifetime of the mongod.
    preImageRemoverService->onConsistentDataAvailable(
        opCtx, false /* isMajority */, true /* isRollback */);
    ASSERT_FALSE(preImageRemoverService->startedPeriodicJob_forTest());
}

TEST_F(PreImagesRemoverServiceTest, PeriodicJobOnSecondary) {
    auto opCtx = operationContext();
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY));

    auto preImageRemoverService = ChangeStreamExpiredPreImagesRemoverService::get(opCtx);

    ASSERT_FALSE(preImageRemoverService->startedPeriodicJob_forTest());

    preImageRemoverService->onConsistentDataAvailable(
        opCtx, false /* isMajority */, true /* isRollback */);
    ASSERT_FALSE(preImageRemoverService->startedPeriodicJob_forTest());

    preImageRemoverService->onConsistentDataAvailable(
        opCtx, false /* isMajority */, false /* isRollback */);
    ASSERT_TRUE(preImageRemoverService->startedPeriodicJob_forTest());
}

TEST_F(PreImagesRemoverServiceTest, PeriodicJobDoesntStartOnStandalone) {
    auto opCtx = operationContext();
    repl::ReplicationCoordinator::set(getServiceContext(),
                                      std::make_unique<repl::ReplicationCoordinatorMock>(
                                          getServiceContext(), repl::ReplSettings()));

    auto preImageRemoverService = ChangeStreamExpiredPreImagesRemoverService::get(opCtx);

    ASSERT_FALSE(preImageRemoverService->startedPeriodicJob_forTest());

    preImageRemoverService->onConsistentDataAvailable(
        opCtx, false /* isMajority */, false /* isRollback */);

    ASSERT_FALSE(preImageRemoverService->startedPeriodicJob_forTest());
}

}  // namespace mongo
