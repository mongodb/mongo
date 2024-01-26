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
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <cstdint>
#include <deque>
#include <iterator>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/change_stream_pre_images_truncate_manager.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
class PreImagesTruncateManagerTest : public CatalogTestFixture {
protected:
    PreImagesTruncateManagerTest() : CatalogTestFixture(Options{}.useMockClock(true)) {}
    ChangeStreamPreImage generatePreImage(const UUID& nsUUID, Timestamp ts, int64_t dataFieldSize) {
        auto preImageId = ChangeStreamPreImageId(nsUUID, ts, 0);
        const auto strField = std::string(dataFieldSize, 'a');
        const BSONObj doc = BSON("dataField" << strField);
        auto operationTime = Date_t::fromDurationSinceEpoch(Seconds{ts.getSecs()});
        return ChangeStreamPreImage(preImageId, operationTime, doc);
    }

    void prePopulatePreImagesCollection(const NamespaceString& preImagesNss,
                                        const UUID& nsUUID,
                                        int64_t dataFieldSize,
                                        int64_t numPreImages) {
        std::vector<ChangeStreamPreImage> preImages;
        for (int64_t i = 0; i < numPreImages; i++) {
            preImages.push_back(
                generatePreImage(nsUUID, Timestamp{Date_t::now()} + i, dataFieldSize));
        }

        std::vector<InsertStatement> preImageInsertStatements;
        std::transform(preImages.begin(),
                       preImages.end(),
                       std::back_inserter(preImageInsertStatements),
                       [](const auto& preImage) { return InsertStatement{preImage.toBSON()}; });

        auto opCtx = operationContext();
        AutoGetCollection preImagesCollectionRaii(opCtx, preImagesNss, MODE_IX);
        ASSERT(preImagesCollectionRaii);
        WriteUnitOfWork wuow(opCtx);
        auto& changeStreamPreImagesCollection = preImagesCollectionRaii.getCollection();

        auto status = collection_internal::insertDocuments(opCtx,
                                                           changeStreamPreImagesCollection,
                                                           preImageInsertStatements.begin(),
                                                           preImageInsertStatements.end(),
                                                           nullptr);
        wuow.commit();
    }

    ClockSourceMock* clockSource() {
        return static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

    void ensureMarkersInitialized(const NamespaceString& preImagesCollectionNss,
                                  boost::optional<TenantId> tenantId,
                                  PreImagesTruncateManager& truncateManager) {
        auto opCtx = operationContext();
        const auto preImagesCollRAII = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(preImagesCollectionNss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);

        truncateManager._fetchOrCreateMarkersForPreImagesCollection(
            opCtx, tenantId, preImagesCollRAII);
    };

    void createPreImagesCollection(boost::optional<TenantId> tenantId) {
        auto preImagesCollectionNss = NamespaceString::makePreImageCollectionNSS(tenantId);
        const auto opCtx = operationContext();
        ChangeStreamPreImagesCollectionManager::get(opCtx).createPreImagesCollection(opCtx,
                                                                                     tenantId);
    }

    void setUp() override {
        CatalogTestFixture::setUp();
        ChangeStreamOptionsManager::create(getServiceContext());

        // Set up OpObserver so that the test will append actual oplog entries to the oplog
        // using repl::logOp().
        auto opObserverRegistry =
            dynamic_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        opObserverRegistry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
    }

    std::tuple<int64_t, int64_t> getNumRecordsAndDataSize(
        const NamespaceString& preImagesCollectionNss) {
        auto opCtx = operationContext();
        const auto preImagesCollRAII = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(preImagesCollectionNss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        // Retrieve the actual data size and number of records for the collection.
        const auto& preImagesColl = preImagesCollRAII.getCollectionPtr();
        return std::make_tuple(preImagesColl->numRecords(opCtx), preImagesColl->dataSize(opCtx));
    }

    void validateNumRecordsInMarkers(const PreImagesTruncateManager& truncateManager,
                                     boost::optional<TenantId> tenantId,
                                     int64_t expectedNumRecords) {
        auto preImagesTenantMarkers = truncateManager._tenantMap.find(tenantId);
        ASSERT(preImagesTenantMarkers);

        auto markersSnapshot = preImagesTenantMarkers->_markersMap.getUnderlyingSnapshot();
        int64_t numRecords{0};
        for (auto& [nsUUID, truncateMarkersForNsUUID] : *markersSnapshot) {
            auto markers = truncateMarkersForNsUUID->getMarkers_forTest();
            for (const auto& marker : markers) {
                numRecords = numRecords + marker.records;
            }
            numRecords = numRecords + truncateMarkersForNsUUID->currentRecords_forTest();
        }
        ASSERT_EQ(numRecords, expectedNumRecords);
    }

    void validateNumBytesInMarkers(const PreImagesTruncateManager& truncateManager,
                                   boost::optional<TenantId> tenantId,
                                   int64_t expectedNumBytes) {
        auto preImagesTenantMarkers = truncateManager._tenantMap.find(tenantId);
        ASSERT(preImagesTenantMarkers);

        auto markersSnapshot = preImagesTenantMarkers->_markersMap.getUnderlyingSnapshot();
        int64_t numBytes{0};
        for (auto& [nsUUID, truncateMarkersForNsUUID] : *markersSnapshot) {
            auto markers = truncateMarkersForNsUUID->getMarkers_forTest();
            for (const auto& marker : markers) {
                numBytes = numBytes + marker.bytes;
            }
            numBytes = numBytes + truncateMarkersForNsUUID->currentBytes_forTest();
        }
        ASSERT_EQ(numBytes, expectedNumBytes);
    }

    void validateMarkersDontExistForNsUUID(const PreImagesTruncateManager& truncateManager,
                                           boost::optional<TenantId> tenantId,
                                           const UUID& nsUUID) {
        auto preImagesTenantMarkers = truncateManager._tenantMap.find(tenantId);
        ASSERT(preImagesTenantMarkers);

        ASSERT(!preImagesTenantMarkers->_markersMap.find(nsUUID));
    }

    void validateCreationMethod(
        const PreImagesTruncateManager& truncateManager,
        boost::optional<TenantId> tenantId,
        const UUID& nsUUID,
        CollectionTruncateMarkers::MarkersCreationMethod expectedCreationMethod) {
        auto preImagesTenantMarkers = truncateManager._tenantMap.find(tenantId);
        ASSERT(preImagesTenantMarkers);

        auto nsUUIDTruncateMarkers = preImagesTenantMarkers->_markersMap.find(nsUUID);
        ASSERT(nsUUIDTruncateMarkers);
        ASSERT_EQ(nsUUIDTruncateMarkers->markersCreationMethod(), expectedCreationMethod);
    }

    void validateMarkersExistForNsUUID(const PreImagesTruncateManager& truncateManager,
                                       boost::optional<TenantId> tenantId,
                                       const UUID& nsUUID) {
        auto preImagesTenantMarkers = truncateManager._tenantMap.find(tenantId);
        ASSERT(preImagesTenantMarkers);

        ASSERT(preImagesTenantMarkers->_markersMap.find(nsUUID));
    }

    void validateIncreasingRidAndWallTimesInMarkers(const PreImagesTruncateManager& truncateManager,
                                                    boost::optional<TenantId> tenantId) {
        auto preImagesTenantMarkers = truncateManager._tenantMap.find(tenantId);
        ASSERT(preImagesTenantMarkers);

        auto markersSnapshot = preImagesTenantMarkers->_markersMap.getUnderlyingSnapshot();
        for (auto& [nsUUID, truncateMarkersForNsUUID] : *markersSnapshot) {
            auto markers = truncateMarkersForNsUUID->getMarkers_forTest();

            RecordId highestSeenRecordId{};
            Date_t highestSeenWallTime{};
            for (const auto& marker : markers) {
                auto currentRid = marker.lastRecord;
                auto currentWallTime = marker.wallTime;
                if (currentRid < highestSeenRecordId || currentWallTime < highestSeenWallTime) {
                    // Something went wrong during marker initialisation. Log the details of which
                    // 'nsUUID' failed for debugging before failing the test.
                    LOGV2_ERROR(7658610,
                                "Truncate markers created for pre-images with nsUUID were not "
                                "initialised in increasing order of highest wall time and RecordId",
                                "nsUUID"_attr = nsUUID,
                                "tenant"_attr = tenantId,
                                "highestSeenWallTime"_attr = highestSeenWallTime,
                                "highestSeenRecordId"_attr = highestSeenRecordId,
                                "markerRecordId"_attr = currentRid,
                                "markerWallTime"_attr = currentWallTime);
                }
                ASSERT_GTE(currentRid, highestSeenRecordId);
                ASSERT_GTE(currentWallTime, highestSeenWallTime);
                highestSeenRecordId = currentRid;
                highestSeenWallTime = currentWallTime;
            }

            const auto& [partialMarkerHighestRid, partialMarkerHighestWallTime] =
                truncateMarkersForNsUUID->getPartialMarker_forTest();
            ASSERT_GTE(partialMarkerHighestRid, highestSeenRecordId);
            ASSERT_GTE(partialMarkerHighestWallTime, highestSeenWallTime);
        }
    }
};

TEST_F(PreImagesTruncateManagerTest, ScanningSingleNsUUIDSingleTenant) {
    auto minBytesPerMarker = 1;
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};

    boost::optional<TenantId> nullTenantId = boost::none;
    createPreImagesCollection(nullTenantId);

    auto preImagesCollectionNss = NamespaceString::makePreImageCollectionNSS(nullTenantId);
    auto nsUUID0 = UUID::gen();

    prePopulatePreImagesCollection(preImagesCollectionNss, nsUUID0, 1, 3000);
    auto [preImagesRecordStoreNumRecords, preImagesRecordStoreDataSize] =
        getNumRecordsAndDataSize(preImagesCollectionNss);

    PreImagesTruncateManager truncateManager;
    ensureMarkersInitialized(preImagesCollectionNss, nullTenantId, truncateManager);

    validateMarkersExistForNsUUID(truncateManager, nullTenantId, nsUUID0);

    validateCreationMethod(truncateManager,
                           nullTenantId,
                           nsUUID0,
                           CollectionTruncateMarkers::MarkersCreationMethod::Scanning);

    validateNumRecordsInMarkers(truncateManager, nullTenantId, preImagesRecordStoreNumRecords);
    validateNumBytesInMarkers(truncateManager, nullTenantId, preImagesRecordStoreDataSize);

    validateIncreasingRidAndWallTimesInMarkers(truncateManager, nullTenantId);
}

TEST_F(PreImagesTruncateManagerTest, ScanningTwoNsUUIDsSingleTenant) {
    auto minBytesPerMarker = 1;
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};

    boost::optional<TenantId> nullTenantId = boost::none;
    createPreImagesCollection(nullTenantId);

    auto preImagesCollectionNss = NamespaceString::makePreImageCollectionNSS(nullTenantId);
    auto nsUUID0 = UUID::gen();
    auto nsUUID1 = UUID::gen();

    prePopulatePreImagesCollection(preImagesCollectionNss, nsUUID0, 100, 10);
    prePopulatePreImagesCollection(preImagesCollectionNss, nsUUID1, 1, 1990);

    auto [preImagesRecordStoreNumRecords, preImagesRecordStoreDataSize] =
        getNumRecordsAndDataSize(preImagesCollectionNss);

    PreImagesTruncateManager truncateManager;
    ensureMarkersInitialized(preImagesCollectionNss, nullTenantId, truncateManager);

    validateMarkersExistForNsUUID(truncateManager, nullTenantId, nsUUID0);
    validateMarkersExistForNsUUID(truncateManager, nullTenantId, nsUUID1);

    validateCreationMethod(truncateManager,
                           nullTenantId,
                           nsUUID0,
                           CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
    validateCreationMethod(truncateManager,
                           nullTenantId,
                           nsUUID1,
                           CollectionTruncateMarkers::MarkersCreationMethod::Scanning);

    validateNumRecordsInMarkers(truncateManager, nullTenantId, preImagesRecordStoreNumRecords);
    validateNumBytesInMarkers(truncateManager, nullTenantId, preImagesRecordStoreDataSize);
}

TEST_F(PreImagesTruncateManagerTest, SamplingSingleNsUUIDSingleTenant) {
    auto minBytesPerMarker = 1024 * 25;  // 25KB to downsize for testing.
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};

    boost::optional<TenantId> nullTenantId = boost::none;
    createPreImagesCollection(nullTenantId);

    auto preImagesCollectionNss = NamespaceString::makePreImageCollectionNSS(nullTenantId);
    auto nsUUID0 = UUID::gen();

    prePopulatePreImagesCollection(preImagesCollectionNss, nsUUID0, 1, 4000);
    auto [preImagesRecordStoreNumRecords, preImagesRecordStoreDataSize] =
        getNumRecordsAndDataSize(preImagesCollectionNss);

    PreImagesTruncateManager truncateManager;
    ensureMarkersInitialized(preImagesCollectionNss, nullTenantId, truncateManager);

    validateMarkersExistForNsUUID(truncateManager, nullTenantId, nsUUID0);

    validateCreationMethod(truncateManager,
                           nullTenantId,
                           nsUUID0,
                           CollectionTruncateMarkers::MarkersCreationMethod::Sampling);

    validateNumRecordsInMarkers(truncateManager, nullTenantId, preImagesRecordStoreNumRecords);
    validateNumBytesInMarkers(truncateManager, nullTenantId, preImagesRecordStoreDataSize);

    validateIncreasingRidAndWallTimesInMarkers(truncateManager, nullTenantId);
}

// Tests that markers initialized from a pre-populated pre-images collection guarantee that the
// total size and number of records across the pre-images collection are captured in the generated
// truncate markers. No other guarantees can be made aside from that the cumulative size and number
// of records across the tenant's nsUUIDs will be consistent.
TEST_F(PreImagesTruncateManagerTest, SamplingTwoNsUUIDsSingleTenant) {
    auto minBytesPerMarker = 1024 * 100;  // 100KB.
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};

    boost::optional<TenantId> nullTenantId = boost::none;
    createPreImagesCollection(nullTenantId);

    auto preImagesCollectionNss = NamespaceString::makePreImageCollectionNSS(nullTenantId);
    auto nsUUID0 = UUID::gen();
    auto nsUUID1 = UUID::gen();

    prePopulatePreImagesCollection(preImagesCollectionNss, nsUUID0, 100, 1000);
    prePopulatePreImagesCollection(preImagesCollectionNss, nsUUID1, 1, 1000);
    auto [preImagesRecordStoreNumRecords, preImagesRecordStoreDataSize] =
        getNumRecordsAndDataSize(preImagesCollectionNss);

    PreImagesTruncateManager truncateManager;
    ensureMarkersInitialized(preImagesCollectionNss, nullTenantId, truncateManager);

    validateMarkersExistForNsUUID(truncateManager, nullTenantId, nsUUID0);
    validateMarkersExistForNsUUID(truncateManager, nullTenantId, nsUUID1);

    validateCreationMethod(truncateManager,
                           nullTenantId,
                           nsUUID0,
                           CollectionTruncateMarkers::MarkersCreationMethod::Sampling);
    validateCreationMethod(truncateManager,
                           nullTenantId,
                           nsUUID1,
                           CollectionTruncateMarkers::MarkersCreationMethod::Sampling);

    validateNumRecordsInMarkers(truncateManager, nullTenantId, preImagesRecordStoreNumRecords);
    validateNumBytesInMarkers(truncateManager, nullTenantId, preImagesRecordStoreDataSize);

    validateIncreasingRidAndWallTimesInMarkers(truncateManager, nullTenantId);
}

TEST_F(PreImagesTruncateManagerTest, SamplingTwoNsUUIDsManyRecordsToFewSingleTenant) {
    auto minBytesPerMarker = 1024 * 100;  // 100KB.
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};

    // For a single tenant environment.
    boost::optional<TenantId> nullTenantId = boost::none;
    createPreImagesCollection(nullTenantId);

    auto preImagesCollectionNss = NamespaceString::makePreImageCollectionNSS(nullTenantId);
    auto nsUUID0 = UUID::gen();
    auto nsUUID1 = UUID::gen();

    prePopulatePreImagesCollection(preImagesCollectionNss, nsUUID0, 100, 1999);
    prePopulatePreImagesCollection(preImagesCollectionNss, nsUUID1, 1, 1);

    auto [preImagesRecordStoreNumRecords, preImagesRecordStoreDataSize] =
        getNumRecordsAndDataSize(preImagesCollectionNss);

    PreImagesTruncateManager truncateManager;
    ensureMarkersInitialized(preImagesCollectionNss, nullTenantId, truncateManager);

    validateMarkersExistForNsUUID(truncateManager, nullTenantId, nsUUID0);
    validateMarkersExistForNsUUID(truncateManager, nullTenantId, nsUUID1);

    validateCreationMethod(truncateManager,
                           nullTenantId,
                           nsUUID0,
                           CollectionTruncateMarkers::MarkersCreationMethod::Sampling);
    validateCreationMethod(truncateManager,
                           nullTenantId,
                           nsUUID1,
                           CollectionTruncateMarkers::MarkersCreationMethod::Sampling);

    validateNumRecordsInMarkers(truncateManager, nullTenantId, preImagesRecordStoreNumRecords);
    validateNumBytesInMarkers(truncateManager, nullTenantId, preImagesRecordStoreDataSize);

    validateIncreasingRidAndWallTimesInMarkers(truncateManager, nullTenantId);
}

TEST_F(PreImagesTruncateManagerTest, SamplingManyNsUUIDsSingleTenant) {
    auto minBytesPerMarker = 1024 * 100;  // 100KB.
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};

    boost::optional<TenantId> nullTenantId = boost::none;
    createPreImagesCollection(nullTenantId);

    auto preImagesCollectionNss = NamespaceString::makePreImageCollectionNSS(nullTenantId);
    std::vector<UUID> nsUUIDs{};
    auto numNssUUIDs = 11;
    for (int i = 0; i < numNssUUIDs; i++) {
        nsUUIDs.push_back(UUID::gen());
    }

    for (const auto& nsUUID : nsUUIDs) {
        prePopulatePreImagesCollection(preImagesCollectionNss, nsUUID, 100, 555);
    }

    auto [preImagesRecordStoreNumRecords, preImagesRecordStoreDataSize] =
        getNumRecordsAndDataSize(preImagesCollectionNss);

    PreImagesTruncateManager truncateManager;
    ensureMarkersInitialized(preImagesCollectionNss, nullTenantId, truncateManager);

    for (const auto& nsUUID : nsUUIDs) {
        validateMarkersExistForNsUUID(truncateManager, nullTenantId, nsUUID);
        validateCreationMethod(truncateManager,
                               nullTenantId,
                               nsUUID,
                               CollectionTruncateMarkers::MarkersCreationMethod::Sampling);
    }

    validateNumRecordsInMarkers(truncateManager, nullTenantId, preImagesRecordStoreNumRecords);
    validateNumBytesInMarkers(truncateManager, nullTenantId, preImagesRecordStoreDataSize);
    validateIncreasingRidAndWallTimesInMarkers(truncateManager, nullTenantId);
}

// Test that the PreImagesTruncateManager correctly separates collection truncate markers for each
// tenant.
TEST_F(PreImagesTruncateManagerTest, TwoTenantsGeneratesTwoSeparateSetsOfTruncateMarkers) {
    auto minBytesPerMarker = 1024 * 25;  // 25KB.
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};
    RAIIServerParameterControllerForTest serverlessFeatureFlagController{
        "featureFlagServerlessChangeStreams", true};

    TenantId tid1 = TenantId(OID::gen());
    TenantId tid2 = TenantId(OID::gen());

    createPreImagesCollection(tid1);
    createPreImagesCollection(tid2);

    auto preImagesCollectionNss1 = NamespaceString::makePreImageCollectionNSS(tid1);
    auto preImagesCollectionNss2 = NamespaceString::makePreImageCollectionNSS(tid2);
    auto nsUUID1 = UUID::gen();
    auto nsUUID2 = UUID::gen();

    prePopulatePreImagesCollection(preImagesCollectionNss1, nsUUID1, 100, 1000);
    prePopulatePreImagesCollection(preImagesCollectionNss2, nsUUID2, 100, 1000);

    auto [tid1NumRecords, tid1DataSize] = getNumRecordsAndDataSize(preImagesCollectionNss1);
    auto [tid2NumRecords, tid2DataSize] = getNumRecordsAndDataSize(preImagesCollectionNss2);

    // Initialise the truncate markers for each colleciton.
    PreImagesTruncateManager truncateManager;
    ensureMarkersInitialized(preImagesCollectionNss1, tid1, truncateManager);
    ensureMarkersInitialized(preImagesCollectionNss2, tid2, truncateManager);

    validateNumRecordsInMarkers(truncateManager, tid1, tid1NumRecords);
    validateNumRecordsInMarkers(truncateManager, tid2, tid2NumRecords);

    validateNumBytesInMarkers(truncateManager, tid1, tid1DataSize);
    validateNumBytesInMarkers(truncateManager, tid2, tid2DataSize);

    validateIncreasingRidAndWallTimesInMarkers(truncateManager, tid1);
    validateIncreasingRidAndWallTimesInMarkers(truncateManager, tid2);

    // Confirm that markers for each nsUUID are correctly isolated on their tenant.
    validateMarkersExistForNsUUID(truncateManager, tid1, nsUUID1);
    validateMarkersDontExistForNsUUID(truncateManager, tid1, nsUUID2);

    validateMarkersExistForNsUUID(truncateManager, tid2, nsUUID2);
    validateMarkersDontExistForNsUUID(truncateManager, tid2, nsUUID1);
}
}  // namespace mongo
