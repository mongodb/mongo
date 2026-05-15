/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/collection_crud/collection_write_path.h"

#include "mongo/base/string_data.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_pre_image_id_util.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class TruncateRangeFixture : public CatalogTestFixture {

public:
    void setUp() override {
        CatalogTestFixture::setUp();
        ChangeStreamOptionsManager::create(getServiceContext());

        auto* opCtx = operationContext();
        ChangeStreamPreImagesCollectionManager::get(opCtx).createPreImagesCollection(opCtx);
    }

    CollectionAcquisition acquirePreImagesCollectionForRead(OperationContext* opCtx) {
        return acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(NamespaceString::kChangeStreamPreImagesNamespace,
                                         PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
    }

    CollectionAcquisition acquirePreImagesCollectionForWrite(OperationContext* opCtx) {
        return acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(NamespaceString::kChangeStreamPreImagesNamespace,
                                         PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kWrite),
            MODE_IX);
    }
};

TEST_F(TruncateRangeFixture,
       Given_PreImagesCollectionLockedInISMode_When_truncateRange_ThrowsIllegalOperationException) {
    auto* opCtx = operationContext();

    auto preImagesCollection = acquirePreImagesCollectionForRead(opCtx);
    auto& collPtr = preImagesCollection.getCollectionPtr();

    RecordId minRecordId(0);
    RecordId maxRecordId(1);

    ASSERT_THROWS_CODE(
        collection_internal::truncateRange(
            opCtx, collPtr, minRecordId, maxRecordId, 0 /* bytesDeleted */, 0 /* docsDeleted */),
        DBException,
        ErrorCodes::IllegalOperation);
}

TEST_F(TruncateRangeFixture,
       Given_NonClusteredCollection_When_truncateRange_ThrowsIllegalOperationException) {
    auto* opCtx = operationContext();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.nonClustered");
    CollectionOptions options;
    ASSERT_OK(storageInterface()->createCollection(opCtx, nss, options));

    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const CollectionPtr& coll = *autoColl;

    RecordId minRecordId(0);
    RecordId maxRecordId(1);

    ASSERT_THROWS_CODE(
        collection_internal::truncateRange(
            opCtx, coll, minRecordId, maxRecordId, 0 /* bytesDeleted */, 0 /* docsDeleted */),
        DBException,
        ErrorCodes::IllegalOperation);
}

TEST_F(TruncateRangeFixture,
       Given_PreimagesEnabledCollection_When_truncateRange_ThrowsIllegalOperationException) {
    auto* opCtx = operationContext();

    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.preimagesEnabledColl");
    CollectionOptions options;
    options.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    options.changeStreamPreAndPostImagesOptions.setEnabled(true);
    ASSERT_OK(storageInterface()->createCollection(opCtx, nss, options));

    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const CollectionPtr& coll = *autoColl;

    RecordId minRecordId(0);
    RecordId maxRecordId(1);

    ASSERT_THROWS_CODE(
        collection_internal::truncateRange(
            opCtx, coll, minRecordId, maxRecordId, 0 /* bytesDeleted */, 0 /* docsDeleted */),
        DBException,
        ErrorCodes::IllegalOperation);
}

TEST_F(
    TruncateRangeFixture,
    Given_ClusteredCollectionWithSecondaryIndex_When_truncateRange_ThrowsIllegalOperationException) {
    auto* opCtx = operationContext();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.clusteredWithIndex");
    CollectionOptions options;
    options.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    ASSERT_OK(storageInterface()->createCollection(opCtx, nss, options));

    // Add a secondary index.
    {
        AutoGetCollection autoColl(opCtx, nss, MODE_X);
        WriteUnitOfWork wuow(opCtx);

        CollectionWriter writer{opCtx, autoColl};
        auto writable = writer.getWritableCollection(opCtx);

        ASSERT_OK(writable->getIndexCatalog()->createIndexOnEmptyCollection(
            opCtx,
            writable,
            BSON("v" << 2 << "name"
                     << "idx"
                     << "key" << BSON("a" << 1))));
        wuow.commit();
    }

    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const CollectionPtr& coll = *autoColl;

    RecordId minRecordId(0);
    RecordId maxRecordId(1);

    ASSERT_THROWS_CODE(
        collection_internal::truncateRange(
            opCtx, coll, minRecordId, maxRecordId, 0 /* bytesDeleted */, 0 /* docsDeleted */),
        DBException,
        ErrorCodes::IllegalOperation);
}

TEST_F(TruncateRangeFixture,
       Given_NullUpperBoundRecordId_When_truncateRange_ThrowsIllegalOperationException) {
    auto* opCtx = operationContext();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.nullMax");
    CollectionOptions options;
    options.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    ASSERT_OK(storageInterface()->createCollection(opCtx, nss, options));

    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const CollectionPtr& coll = *autoColl;

    RecordId minRecordId(0);
    RecordId nullMaxRecordId;

    ASSERT_THROWS_CODE(
        collection_internal::truncateRange(
            opCtx, coll, minRecordId, nullMaxRecordId, 0 /* bytesDeleted */, 0 /* docsDeleted */),
        DBException,
        ErrorCodes::IllegalOperation);
}

TEST_F(
    TruncateRangeFixture,
    Given_PreimageCollectionWithMaxTsTruncate_When_truncateRange_ThrowsIllegalOperationException) {
    auto* opCtx = operationContext();

    auto collUUID = UUID::gen();
    auto preImagesCollection = acquirePreImagesCollectionForWrite(opCtx);
    auto& collPtr = preImagesCollection.getCollectionPtr();

    // Use the absolute [min, max] RecordId bounds for kNsUUID. The max bound uses Timestamp::max()
    // and must therefore be considered “in the future” relative to getMaxTSEligibleForTruncate().
    RecordId minRecordId =
        change_stream_pre_image_id_util::getAbsoluteMinPreImageRecordIdBoundForNs(collUUID)
            .recordId();
    RecordId maxRecordId =
        change_stream_pre_image_id_util::getAbsoluteMaxPreImageRecordIdBoundForNs(collUUID)
            .recordId();

    ASSERT_THROWS_CODE(
        collection_internal::truncateRange(
            opCtx, collPtr, minRecordId, maxRecordId, 0 /* bytesDeleted */, 0 /* docsDeleted */),
        DBException,
        ErrorCodes::IllegalOperation);
}

TEST_F(TruncateRangeFixture, Given_OplogCollectionWithoutIndexes_When_truncateRange_Succeeds) {
    auto* opCtx = operationContext();

    const auto& nss = NamespaceString::kRsOplogNamespace;
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    const CollectionPtr& coll = *autoColl;
    ASSERT(coll);
    EXPECT_TRUE(coll->ns().isOplog());
    EXPECT_EQ(0, coll->getTotalIndexCount());
    EXPECT_FALSE(coll->isChangeStreamPreAndPostImagesEnabled());

    RecordId minRecordId(0);
    RecordId maxRecordId(1);

    WriteUnitOfWork wuow(opCtx);
    ASSERT_NO_THROW(collection_internal::truncateRange(
        opCtx, coll, minRecordId, maxRecordId, 0 /* bytesDeleted */, 0 /* docsDeleted */));
    wuow.commit();
}

TEST_F(TruncateRangeFixture,
       Given_PreimagesCollectionWithTsAtMostMaxEligible_When_truncateRange_Succeeds) {
    auto* opCtx = operationContext();

    // Pre-images collection is created in setUp().
    auto preImagesAcq = acquirePreImagesCollectionForWrite(opCtx);
    const auto& collPtr = preImagesAcq.getCollectionPtr();
    ASSERT(collPtr);
    EXPECT_TRUE(collPtr->ns().isChangeStreamPreImagesCollection());
    EXPECT_EQ(0, collPtr->getTotalIndexCount());
    EXPECT_FALSE(collPtr->isChangeStreamPreAndPostImagesEnabled());

    const UUID collUUID = UUID::gen();

    const Timestamp maxTsEligible =
        change_stream_pre_image_id_util::getMaxTSEligibleForTruncate(opCtx);

    // Choose a max RecordId whose timestamp == maxTsEligible,
    // so tsBehindMaxRecordId <= maxTsEligible holds.
    const RecordId minRecordId =
        change_stream_pre_image_id_util::getAbsoluteMinPreImageRecordIdBoundForNs(collUUID)
            .recordId();
    const RecordId maxRecordId =
        change_stream_pre_image_id_util::getPreImageRecordIdForNsTimestampApplyOpsIndex(
            collUUID, maxTsEligible, std::numeric_limits<int64_t>::max() /* applyOpsIndex */)
            .recordId();

    WriteUnitOfWork wuow(opCtx);
    ASSERT_NO_THROW(collection_internal::truncateRange(
        opCtx, collPtr, minRecordId, maxRecordId, 0 /* bytesDeleted */, 0 /* docsDeleted */));
    wuow.commit();
}

}  // namespace
}  // namespace mongo
