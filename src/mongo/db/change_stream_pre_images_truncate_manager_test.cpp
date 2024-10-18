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

#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/change_stream_pre_images_truncate_manager.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/collection_truncate_markers.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

class PreImagesTruncateManagerTest : public CatalogTestFixture {
protected:
    const UUID kNsUUID0 = UUID::gen();
    const UUID kNsUUID1 = UUID::gen();
    const UUID kNsUUID2 = UUID::gen();

    const TenantId kTenantIdA = TenantId(OID::gen());
    const TenantId kTenantIdB = TenantId(OID::gen());
    const boost::optional<TenantId> kNullTenantId{};

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

    auto acquirePreImagesCollectionForRead(NamespaceStringOrUUID nssOrUUID) {
        const auto opCtx = operationContext();
        return acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(std::move(nssOrUUID),
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
    }

    void createPreImagesCollection(boost::optional<TenantId> tenantId) {
        const auto preImagesCollectionNss = NamespaceString::makePreImageCollectionNSS(tenantId);
        const auto opCtx = operationContext();
        ChangeStreamPreImagesCollectionManager::get(opCtx).createPreImagesCollection(opCtx,
                                                                                     tenantId);
    }

    void insertPreImages(boost::optional<TenantId> tenantId,
                         const UUID& nsUUID,
                         int64_t numPreImages,
                         int64_t docPaddingBytes) {

        const auto opCtx = operationContext();
        const auto preImagesNss = NamespaceString::makePreImageCollectionNSS(tenantId);
        AutoGetCollection preImagesCollectionRaii(opCtx, preImagesNss, MODE_IX);
        ASSERT(preImagesCollectionRaii);

        // 'getNextOpTimes' requires us to be inside a WUOW.
        WriteUnitOfWork wuow(opCtx);
        auto opTimes = repl::getNextOpTimes(opCtx, numPreImages);

        std::vector<InsertStatement> preImageInsertStatements;
        for (const auto& opTime : opTimes) {
            ChangeStreamPreImageId preImageId(nsUUID, opTime.getTimestamp(), 0);
            const auto operationTime = Date_t() + Seconds(opTime.getSecs());
            ChangeStreamPreImage preImage(std::move(preImageId),
                                          operationTime,
                                          BSON("padding" << std::string(docPaddingBytes, 'a')));
            preImageInsertStatements.push_back(InsertStatement{preImage.toBSON()});
        }

        auto& changeStreamPreImagesCollection = preImagesCollectionRaii.getCollection();
        ASSERT_OK(collection_internal::insertDocuments(opCtx,
                                                       changeStreamPreImagesCollection,
                                                       preImageInsertStatements.begin(),
                                                       preImageInsertStatements.end(),
                                                       nullptr));
        wuow.commit();
    }

    std::shared_ptr<PreImagesTenantMarkers> getInitializedTruncateMarkers(
        boost::optional<TenantId> tenantId) {
        return _truncateManager._getInitializedMarkersForPreImagesCollection(operationContext(),
                                                                             tenantId);
    }

    int64_t getNumRecordsInMarkers(std::shared_ptr<PreImagesTenantMarkers> tenantMarkers) {
        const auto markersSnapshot = tenantMarkers->_markersMap.getUnderlyingSnapshot();
        int64_t numRecords{0};
        for (const auto& [nsUUID, truncateMarkersForNsUUID] : *markersSnapshot) {
            auto markers = truncateMarkersForNsUUID->getMarkers_forTest();
            for (const auto& marker : markers) {
                numRecords = numRecords + marker.records;
            }
            numRecords = numRecords + truncateMarkersForNsUUID->currentRecords_forTest();
        }
        return numRecords;
    }

    int64_t getBytesInMarkers(std::shared_ptr<PreImagesTenantMarkers> tenantMarkers) {
        const auto markersSnapshot = tenantMarkers->_markersMap.getUnderlyingSnapshot();
        int64_t bytes{0};
        for (const auto& [nsUUID, truncateMarkersForNsUUID] : *markersSnapshot) {
            auto markers = truncateMarkersForNsUUID->getMarkers_forTest();
            for (const auto& marker : markers) {
                bytes = bytes + marker.bytes;
            }
            bytes = bytes + truncateMarkersForNsUUID->currentBytes_forTest();
        }
        return bytes;
    }

    // Validates that the truncate markers capture the metadata for the pre-images collection
    // accurately.
    void validateMarkerMetadataMatchesCollection(
        boost::optional<TenantId> tenantId,
        const CollectionAcquisition& preImagesCollection,
        std::shared_ptr<PreImagesTenantMarkers> tenantMarkers) {
        ASSERT(preImagesCollection.exists());

        // Also validate that the _truncateManager contains the markers we are validated.
        ASSERT(_truncateManager._tenantMap.find(tenantId));

        // Truncate markers store the UUID, nss, and tenantId corresponding the tenant's pre-images
        // collection they were created with.
        ASSERT_EQ(tenantMarkers->_preImagesCollectionUUID, preImagesCollection.uuid());
        ASSERT_EQ(tenantMarkers->_preImagesCollectionNss, preImagesCollection.nss());
        ASSERT_EQ(tenantMarkers->_tenantId, tenantId);

        const auto& preImagesCollPtr = preImagesCollection.getCollectionPtr();
        const auto opCtx = operationContext();

        const auto numRecordsInMarkers = getNumRecordsInMarkers(tenantMarkers);
        ASSERT_EQ(numRecordsInMarkers, preImagesCollPtr->numRecords(opCtx));

        const auto bytesInMarkers = getBytesInMarkers(tenantMarkers);
        ASSERT_EQ(bytesInMarkers, preImagesCollPtr->dataSize(opCtx));
    }

    void validateMarkersExistForNsUUID(std::shared_ptr<PreImagesTenantMarkers> tenantMarkers,
                                       const UUID& nsUUID) {
        ASSERT(tenantMarkers->_markersMap.find(nsUUID));
    }

    void validateCreationMethod(
        std::shared_ptr<PreImagesTenantMarkers> tenantMarkers,
        const UUID& nsUUID,
        CollectionTruncateMarkers::MarkersCreationMethod expectedCreationMethod) {
        auto nsUUIDTruncateMarkers = tenantMarkers->_markersMap.find(nsUUID);
        ASSERT(nsUUIDTruncateMarkers);
        ASSERT_EQ(nsUUIDTruncateMarkers->markersCreationMethod(), expectedCreationMethod);
    }

    void validateMarkersDontExistForNsUUID(std::shared_ptr<PreImagesTenantMarkers> tenantMarkers,
                                           const UUID& nsUUID) {
        ASSERT(!tenantMarkers->_markersMap.find(nsUUID));
    }

    void validateIncreasingRidAndWallTimesInMarkers(
        std::shared_ptr<PreImagesTenantMarkers> tenantMarkers) {
        auto markersSnapshot = tenantMarkers->_markersMap.getUnderlyingSnapshot();
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
                                "tenant"_attr = tenantMarkers->_tenantId,
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
                truncateMarkersForNsUUID->getHighestRecordMetrics_forTest();
            ASSERT_GTE(partialMarkerHighestRid, highestSeenRecordId);
            ASSERT_GTE(partialMarkerHighestWallTime, highestSeenWallTime);
        }
    }

private:
    PreImagesTruncateManager _truncateManager;
};

TEST_F(PreImagesTruncateManagerTest, ScanningSingleNsUUIDSingleTenant) {
    auto minBytesPerMarker = 1;
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};

    createPreImagesCollection(kNullTenantId);

    insertPreImages(kNullTenantId, kNsUUID0, /*numPreImages*/ 3000, /*docPaddingBytes*/ 1);

    auto tenantMarkers = getInitializedTruncateMarkers(kNullTenantId);
    ASSERT(tenantMarkers);

    const auto preImagesCollection = acquirePreImagesCollectionForRead(
        NamespaceString::makePreImageCollectionNSS(kNullTenantId));

    validateMarkerMetadataMatchesCollection(kNullTenantId, preImagesCollection, tenantMarkers);

    validateIncreasingRidAndWallTimesInMarkers(tenantMarkers);

    validateMarkersExistForNsUUID(tenantMarkers, kNsUUID0);

    validateCreationMethod(
        tenantMarkers, kNsUUID0, CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
}

TEST_F(PreImagesTruncateManagerTest, ScanningSingleNsUUIDSingleTenant1Doc) {
    auto minBytesPerMarker = 1;
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};

    createPreImagesCollection(kNullTenantId);

    insertPreImages(kNullTenantId, kNsUUID0, /*numPreImages*/ 1, /*docPaddingBytes*/ 1);

    auto tenantMarkers = getInitializedTruncateMarkers(kNullTenantId);
    ASSERT(tenantMarkers);

    const auto preImagesCollection = acquirePreImagesCollectionForRead(
        NamespaceString::makePreImageCollectionNSS(kNullTenantId));

    validateMarkerMetadataMatchesCollection(kNullTenantId, preImagesCollection, tenantMarkers);

    validateIncreasingRidAndWallTimesInMarkers(tenantMarkers);

    validateMarkersExistForNsUUID(tenantMarkers, kNsUUID0);

    validateCreationMethod(
        tenantMarkers, kNsUUID0, CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
}

TEST_F(PreImagesTruncateManagerTest, SingleTenantEmptyCollection) {
    auto minBytesPerMarker = 1;
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};

    createPreImagesCollection(kNullTenantId);

    auto tenantMarkers = getInitializedTruncateMarkers(kNullTenantId);
    ASSERT(tenantMarkers);

    const auto preImagesCollection = acquirePreImagesCollectionForRead(
        NamespaceString::makePreImageCollectionNSS(kNullTenantId));

    validateMarkerMetadataMatchesCollection(kNullTenantId, preImagesCollection, tenantMarkers);
}

TEST_F(PreImagesTruncateManagerTest, ScanningTwoNsUUIDsSingleTenant) {
    auto minBytesPerMarker = 1;
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};

    createPreImagesCollection(kNullTenantId);

    insertPreImages(kNullTenantId, kNsUUID0, /*numPreImages*/ 10, /*docPaddingSize*/ 100);
    insertPreImages(kNullTenantId, kNsUUID1, /*numPreImages*/ 1990, /*docPaddingSize*/ 1);

    auto tenantMarkers = getInitializedTruncateMarkers(kNullTenantId);
    ASSERT(tenantMarkers);

    const auto preImagesCollection = acquirePreImagesCollectionForRead(
        NamespaceString::makePreImageCollectionNSS(kNullTenantId));

    validateMarkerMetadataMatchesCollection(kNullTenantId, preImagesCollection, tenantMarkers);

    validateIncreasingRidAndWallTimesInMarkers(tenantMarkers);

    std::vector<UUID> nsUUIDs{kNsUUID0, kNsUUID1};
    for (const auto& nsUUID : nsUUIDs) {
        validateMarkersExistForNsUUID(tenantMarkers, nsUUID);
        validateCreationMethod(
            tenantMarkers, nsUUID, CollectionTruncateMarkers::MarkersCreationMethod::Scanning);
    }
}

TEST_F(PreImagesTruncateManagerTest, SamplingSingleNsUUIDSingleTenant) {
    auto minBytesPerMarker = 1024 * 25;  // 25KB to downsize for testing.
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};

    createPreImagesCollection(kNullTenantId);

    insertPreImages(kNullTenantId, kNsUUID0, /*numPreImages*/ 4000, /*docPaddingBytes*/ 1);

    auto tenantMarkers = getInitializedTruncateMarkers(kNullTenantId);
    ASSERT(tenantMarkers);

    const auto preImagesCollection = acquirePreImagesCollectionForRead(
        NamespaceString::makePreImageCollectionNSS(kNullTenantId));

    validateMarkerMetadataMatchesCollection(kNullTenantId, preImagesCollection, tenantMarkers);

    validateIncreasingRidAndWallTimesInMarkers(tenantMarkers);

    validateMarkersExistForNsUUID(tenantMarkers, kNsUUID0);

    validateCreationMethod(
        tenantMarkers, kNsUUID0, CollectionTruncateMarkers::MarkersCreationMethod::Sampling);
}

// Tests that markers initialized from a pre-populated pre-images collection guarantee that the
// total size and number of records across the pre-images collection are captured in the generated
// truncate markers. No other guarantees can be made aside from that the cumulative size and number
// of records across the tenant's nsUUIDs will be consistent.
TEST_F(PreImagesTruncateManagerTest, SamplingTwoNsUUIDsSingleTenant) {
    auto minBytesPerMarker = 1024 * 100;  // 100KB.
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};

    createPreImagesCollection(kNullTenantId);

    insertPreImages(kNullTenantId, kNsUUID0, /*numPreImages*/ 1000, /*docPaddingSize*/ 100);
    insertPreImages(kNullTenantId, kNsUUID1, /*numPreImages*/ 1000, /*docPaddingSize*/ 1);

    auto tenantMarkers = getInitializedTruncateMarkers(kNullTenantId);

    const auto preImagesCollection = acquirePreImagesCollectionForRead(
        NamespaceString::makePreImageCollectionNSS(kNullTenantId));

    validateMarkerMetadataMatchesCollection(kNullTenantId, preImagesCollection, tenantMarkers);

    validateIncreasingRidAndWallTimesInMarkers(tenantMarkers);

    std::vector<UUID> nsUUIDs{kNsUUID0, kNsUUID1};
    for (const auto& nsUUID : nsUUIDs) {
        validateMarkersExistForNsUUID(tenantMarkers, nsUUID);
        validateCreationMethod(
            tenantMarkers, nsUUID, CollectionTruncateMarkers::MarkersCreationMethod::Sampling);
    }
}

TEST_F(PreImagesTruncateManagerTest, SamplingTwoNsUUIDsManyRecordsToFewSingleTenant) {
    auto minBytesPerMarker = 1024 * 100;  // 100KB.
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};

    createPreImagesCollection(kNullTenantId);

    insertPreImages(kNullTenantId, kNsUUID0, /*numPreImages*/ 1999, /*docPaddingSize*/ 100);
    insertPreImages(kNullTenantId, kNsUUID1, /*numPreImages*/ 1, /*docPaddingSize*/ 1);

    auto tenantMarkers = getInitializedTruncateMarkers(kNullTenantId);
    ASSERT(tenantMarkers);

    const auto preImagesCollection = acquirePreImagesCollectionForRead(
        NamespaceString::makePreImageCollectionNSS(kNullTenantId));

    validateMarkerMetadataMatchesCollection(kNullTenantId, preImagesCollection, tenantMarkers);

    validateIncreasingRidAndWallTimesInMarkers(tenantMarkers);

    std::vector<UUID> nsUUIDs{kNsUUID0, kNsUUID1};
    for (const auto& nsUUID : nsUUIDs) {
        validateMarkersExistForNsUUID(tenantMarkers, nsUUID);
        validateCreationMethod(
            tenantMarkers, nsUUID, CollectionTruncateMarkers::MarkersCreationMethod::Sampling);
    }
}

TEST_F(PreImagesTruncateManagerTest, SamplingManyNsUUIDsSingleTenant) {
    auto minBytesPerMarker = 1024 * 100;  // 100KB.
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};

    createPreImagesCollection(kNullTenantId);

    std::vector<UUID> nsUUIDs{};
    auto numNssUUIDs = 11;
    for (int i = 0; i < numNssUUIDs; i++) {
        nsUUIDs.push_back(UUID::gen());
    }

    for (const auto& nsUUID : nsUUIDs) {
        insertPreImages(kNullTenantId, nsUUID, /*numPreImages*/ 555, /*docPaddingSize*/ 100);
    }

    auto tenantMarkers = getInitializedTruncateMarkers(kNullTenantId);
    ASSERT(tenantMarkers);

    const auto preImagesCollection = acquirePreImagesCollectionForRead(
        NamespaceString::makePreImageCollectionNSS(kNullTenantId));

    validateMarkerMetadataMatchesCollection(kNullTenantId, preImagesCollection, tenantMarkers);

    validateIncreasingRidAndWallTimesInMarkers(tenantMarkers);

    for (const auto& nsUUID : nsUUIDs) {
        validateMarkersExistForNsUUID(tenantMarkers, nsUUID);
        validateCreationMethod(
            tenantMarkers, nsUUID, CollectionTruncateMarkers::MarkersCreationMethod::Sampling);
    }
}

// Test that the PreImagesTruncateManager correctly separates collection truncate markers for each
// tenant.
TEST_F(PreImagesTruncateManagerTest, TwoTenantsGeneratesTwoSeparateSetsOfTruncateMarkers) {
    auto minBytesPerMarker = 1024 * 25;  // 25KB.
    RAIIServerParameterControllerForTest minBytesPerMarkerController{
        "preImagesCollectionTruncateMarkersMinBytes", minBytesPerMarker};
    RAIIServerParameterControllerForTest serverlessFeatureFlagController{
        "featureFlagServerlessChangeStreams", true};

    std::vector<std::pair<UUID, TenantId>> tenants{{kNsUUID1, kTenantIdA}, {kNsUUID2, kTenantIdB}};
    // Pre-populate each tenant's pre-images collection.
    for (const auto& [nsUUID, tenantId] : tenants) {
        createPreImagesCollection(tenantId);
        insertPreImages(tenantId, nsUUID, /*numPreImages*/ 1000, /*docPaddingSize*/ 100);
    }

    // Create both sets of markers serially. This could be combined with the pre-population phase,
    // but separating out the tasks simulates a more realistic scenario as the remover thread would
    // initialize markers if they don't exist one after the other.
    for (const auto& [nsUUID, tenantId] : tenants) {
        auto tenantMarkers = getInitializedTruncateMarkers(tenantId);
        ASSERT(tenantMarkers);

        const auto preImagesCollection =
            acquirePreImagesCollectionForRead(NamespaceString::makePreImageCollectionNSS(tenantId));

        validateMarkerMetadataMatchesCollection(tenantId, preImagesCollection, tenantMarkers);

        validateIncreasingRidAndWallTimesInMarkers(tenantMarkers);

        validateMarkersExistForNsUUID(tenantMarkers, nsUUID);

        // Confirm that the nsUUID for the other tenant doesn't spill into these tenantMarkers.
        for (const auto& [nsUUIDOther, tenantIdOther] : tenants) {
            if (tenantId != tenantIdOther) {
                // nsUUIDOther belongs to a tenant that isn't the current tenant; thus, it should
                // not be accounted for in the truncateMarkers.
                validateMarkersDontExistForNsUUID(tenantMarkers, nsUUIDOther);
            }
        }
    }
}
}  // namespace mongo
