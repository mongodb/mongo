/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>

namespace mongo {
namespace repl {
namespace {

/**
 * Creates a delete oplog entry without a recordId. Uses DurableOplogEntryParams to construct
 * the entry.
 */
OplogEntry makeDeleteOplogEntry(OpTime opTime,
                                const NamespaceString& nss,
                                const UUID& uuid,
                                const BSONObj& docToDelete) {
    return {DurableOplogEntry{DurableOplogEntryParams{
        .opTime = opTime,
        .opType = OpTypeEnum::kDelete,
        .nss = nss,
        .uuid = uuid,
        .oField = docToDelete,
        .wallClockTime = Date_t::now(),
    }}};
}

/**
 * Creates a delete oplog entry with the given recordId. Uses DurableOplogEntryParams to construct
 * the entry, then adds the recordId since it's not included in the params struct.
 */
OplogEntry makeDeleteOplogEntryWithRecordId(OpTime opTime,
                                            const NamespaceString& nss,
                                            const UUID& uuid,
                                            const BSONObj& docToDelete,
                                            const RecordId& rid) {
    OplogEntry baseEntry = makeDeleteOplogEntry(opTime, nss, uuid, docToDelete);

    // Add the recordId field since it's not included in DurableOplogEntryParams.
    BSONObjBuilder builder;
    builder.appendElements(baseEntry.getEntry().toBSON());
    rid.serializeToken("rid", &builder);

    return {DurableOplogEntry(builder.obj())};
}

/**
 * Creates a collection with recordIdsReplicated enabled.
 */
UUID createCollectionWithRecordIdsReplicated(OperationContext* opCtx, const NamespaceString& nss) {
    CollectionOptions options;
    options.uuid = UUID::gen();
    options.recordIdsReplicated = true;
    createCollection(opCtx, nss, options);
    return options.uuid.value();
}

/**
 * Creates a collection with both recordIdsReplicated and change stream pre-images enabled.
 */
UUID createCollectionWithRecordIdsReplicatedAndPreImages(OperationContext* opCtx,
                                                         const NamespaceString& nss) {
    CollectionOptions options;
    options.uuid = UUID::gen();
    options.recordIdsReplicated = true;
    options.changeStreamPreAndPostImagesOptions.setEnabled(true);
    createCollection(opCtx, nss, options);
    return options.uuid.value();
}

/**
 * Inserts a document into a collection at a specific recordId. Returns the RecordId where the
 * document was inserted.
 */
void insertDocumentAtRecordId(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& doc,
                              const RecordId& rid) {
    WriteUnitOfWork wuow(opCtx);
    AutoGetCollection coll(opCtx, nss, MODE_IX);
    ASSERT(coll);

    InsertStatement stmt{doc};
    stmt.replicatedRecordId = rid;
    ASSERT_OK(collection_internal::insertDocument(opCtx, *coll, stmt, nullptr /* opDebug */));

    wuow.commit();
}

/**
 * Checks if a document exists at a specific recordId.
 */
bool documentExistsAtRecordId(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const RecordId& rid) {
    AutoGetCollection coll(opCtx, nss, MODE_IS);
    if (!coll) {
        return false;
    }
    auto cursor = coll->getCursor(opCtx);
    auto record = cursor->seekExact(rid);
    if (record.has_value()) {
        record->data.makeOwned();
    }
    return record.has_value();
}

/**
 * Test fixture for delete oplog entries with recordId.
 */
class DeleteWithRecordIdTest : public OplogApplierImplTest {
protected:
    void setUp() override {
        OplogApplierImplTest::setUp();
        _nss = NamespaceString::createNamespaceString_forTest("test.deleteRecordId");
        _uuid = createCollectionWithRecordIdsReplicated(_opCtx.get(), _nss);
    }

    NamespaceString _nss;
    UUID _uuid = UUID::gen();
};

template <typename T, bool enable>
class SetSteadyStateConstraints : public T {
protected:
    void setUp() override {
        T::setUp();
        _constraintsEnabled = oplogApplicationEnforcesSteadyStateConstraints.load();
        oplogApplicationEnforcesSteadyStateConstraints.store(enable);
    }

    void tearDown() override {
        oplogApplicationEnforcesSteadyStateConstraints.store(_constraintsEnabled);
        T::tearDown();
    }

private:
    bool _constraintsEnabled;
};

typedef SetSteadyStateConstraints<DeleteWithRecordIdTest, false>
    DeleteWithRecordIdTestDisableSteadyStateConstraints;
typedef SetSteadyStateConstraints<DeleteWithRecordIdTest, true>
    DeleteWithRecordIdTestEnableSteadyStateConstraints;

// =============================================================================
// Tests for kSecondary mode (steady state replication)
// =============================================================================

TEST_F(DeleteWithRecordIdTestDisableSteadyStateConstraints, SuccessInSecondaryMode) {
    // Insert a document at a known recordId.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Create and apply the delete oplog entry with recordId.
    auto op = makeDeleteOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, BSON("_id" << 1), rid);
    ASSERT_OK(runOpSteadyState(op));

    // Verify the document was deleted.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

TEST_F(DeleteWithRecordIdTestDisableSteadyStateConstraints,
       RecordIdNotFoundInSecondaryModeSucceeds) {
    // Create a recordId that doesn't exist in the collection.
    const RecordId nonExistentRid(999);

    // Verify the recordId doesn't have a document.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, nonExistentRid));

    // Create and apply the delete oplog entry with the non-existent recordId.
    // With constraints disabled, this should succeed (noop).
    auto op = makeDeleteOplogEntryWithRecordId(
        nextOpTime(), _nss, _uuid, BSON("_id" << 999), nonExistentRid);
    ASSERT_OK(runOpSteadyState(op));
}

TEST_F(DeleteWithRecordIdTestEnableSteadyStateConstraints, RecordIdNotFoundInSecondaryModeFails) {
    // Create a recordId that doesn't exist in the collection.
    const RecordId nonExistentRid(999);

    // Verify the recordId doesn't have a document.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, nonExistentRid));

    // Create and apply the delete oplog entry with the non-existent recordId.
    // With constraints disabled, this should succeed (noop).
    auto op = makeDeleteOplogEntryWithRecordId(
        nextOpTime(), _nss, _uuid, BSON("_id" << 999), nonExistentRid);
    ASSERT_NOT_OK(runOpSteadyState(op));
}

TEST_F(DeleteWithRecordIdTestDisableSteadyStateConstraints, IdMismatch) {
    // Insert a document at a known recordId with _id = 1.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Create a delete oplog entry that references the same recordId but with a different _id.
    // This simulates data corruption where the record at the given rid has a different _id.
    auto op = makeDeleteOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, BSON("_id" << 999), rid);

    // In kSecondary mode with steady state constraints disabled, this should succeed but the record
    // should not be deleted.
    ASSERT_OK(runOpSteadyState(op));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

TEST_F(DeleteWithRecordIdTestEnableSteadyStateConstraints, IdMismatchFails) {
    // Insert a document at a known recordId with _id = 1.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Create a delete oplog entry that references the same recordId but with a different _id.
    // This simulates data corruption where the record at the given rid has a different _id.
    auto op = makeDeleteOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, BSON("_id" << 999), rid);

    // In kSecondary mode with steady state constraints enabled, this should trigger a uassert and
    // the record should not be deleted.
    ASSERT_NOT_OK(runOpSteadyState(op));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

// =============================================================================
// Tests for kInitialSync mode
// =============================================================================

TEST_F(DeleteWithRecordIdTest, DeleteWithRecordIdSuccessInInitialSyncMode) {
    // Insert a document at a known recordId.
    const RecordId rid(2);
    const BSONObj doc = BSON("_id" << 2 << "x" << 200);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Create and apply the delete oplog entry with recordId in initial sync mode.
    auto op = makeDeleteOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, BSON("_id" << 2), rid);
    ASSERT_OK(runOpInitialSync(op));

    // Verify the document was deleted.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

TEST_F(DeleteWithRecordIdTest, DeleteWithRecordIdNotFoundInInitialSyncMode) {
    // Create a recordId that doesn't exist in the collection.
    const RecordId nonExistentRid(888);

    // Verify the recordId doesn't have a document.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, nonExistentRid));

    // Create and apply the delete oplog entry with the non-existent recordId.
    // In initial sync mode, this should succeed (noop) since we're more lenient.
    auto op = makeDeleteOplogEntryWithRecordId(
        nextOpTime(), _nss, _uuid, BSON("_id" << 888), nonExistentRid);
    ASSERT_OK(runOpInitialSync(op));
}

TEST_F(DeleteWithRecordIdTest, DeleteWithRecordIdMismatchIdInInitialSyncModeSucceeds) {
    // Insert a document at a known recordId with _id = 3.
    const RecordId rid(3);
    const BSONObj doc = BSON("_id" << 3 << "x" << 300);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Create a delete oplog entry that references the same recordId but with a different _id.
    // In initial sync mode, _id mismatches are ignored (the operation returns OK without
    // performing the delete).
    auto op = makeDeleteOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, BSON("_id" << 777), rid);
    ASSERT_OK(runOpInitialSync(op));

    // In initial sync mode, the _id mismatch is ignored and the function returns Status::OK()
    // without deleting the document.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

// =============================================================================
// Additional edge case tests
// =============================================================================

TEST_F(DeleteWithRecordIdTestDisableSteadyStateConstraints,
       DeleteWithRecordIdMultipleDocumentsInSecondaryMode) {
    // Insert multiple documents.
    const RecordId rid1(10);
    const RecordId rid2(20);
    const RecordId rid3(30);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 10 << "x" << 1), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 20 << "x" << 2), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 30 << "x" << 3), rid3);

    // Verify all documents exist.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid1));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid2));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid3));

    // Delete the middle document using its recordId.
    auto op = makeDeleteOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, BSON("_id" << 20), rid2);
    ASSERT_OK(runOpSteadyState(op));

    // Verify only the targeted document was deleted.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid1));
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid2));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid3));
}

TEST_F(DeleteWithRecordIdTest, DeleteWithRecordIdMultipleDocumentsInInitialSyncMode) {
    // Insert multiple documents.
    const RecordId rid1(100);
    const RecordId rid2(200);
    const RecordId rid3(300);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 100 << "a" << 1), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 200 << "a" << 2), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 300 << "a" << 3), rid3);

    // Verify all documents exist.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid1));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid2));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid3));

    // Delete the first document using its recordId.
    auto op = makeDeleteOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, BSON("_id" << 100), rid1);
    ASSERT_OK(runOpInitialSync(op));

    // Verify only the targeted document was deleted.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid1));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid2));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid3));
}

// =============================================================================
// Tests for kApplyOpsCmd mode (applyOps command)
// =============================================================================

/**
 * Test fixture for delete oplog entries in applyOps mode on a recordIdsReplicated collection.
 */
class ApplyOpsDeleteTest : public DeleteWithRecordIdTest {
protected:
    Status runOpApplyOpsCmd(const OplogEntry& op) {
        return _applyOplogEntryOrGroupedInsertsWrapper(
            _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kApplyOpsCmd);
    }
};

TEST_F(ApplyOpsDeleteTest, DeleteByIdSucceeds) {
    // Insert a document at a known recordId.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Create and apply a delete oplog entry WITHOUT recordId in applyOps mode.
    // In kApplyOpsCmd mode, deletes without recordId are allowed even on recordIdsReplicated
    // collections.
    auto op = makeDeleteOplogEntry(nextOpTime(), _nss, _uuid, BSON("_id" << 1));
    ASSERT_OK(runOpApplyOpsCmd(op));

    // Verify the document was deleted.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

TEST_F(ApplyOpsDeleteTest, ApplyOpsDeleteByIdMultipleDocumentsSucceeds) {
    // Insert multiple documents at known recordIds.
    const RecordId rid1(10);
    const RecordId rid2(20);
    const RecordId rid3(30);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 10 << "value" << "a"), rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 20 << "value" << "b"), rid2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 30 << "value" << "c"), rid3);

    // Verify all documents exist.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid1));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid2));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid3));

    // Delete the middle document without recordId in applyOps mode.
    auto op = makeDeleteOplogEntry(nextOpTime(), _nss, _uuid, BSON("_id" << 20));
    ASSERT_OK(runOpApplyOpsCmd(op));

    // Verify only the targeted document was deleted.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid1));
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid2));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid3));
}

TEST_F(ApplyOpsDeleteTest, ApplyOpsDeleteByIdDocumentNotFoundSucceeds) {
    // Insert a document at a known recordId.
    const RecordId rid(1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid);

    // Try to delete a document that doesn't exist in applyOps mode.
    // This should succeed as a no-op.
    auto op = makeDeleteOplogEntry(nextOpTime(), _nss, _uuid, BSON("_id" << 999));
    ASSERT_OK(runOpApplyOpsCmd(op));

    // The existing document should still be there.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

using ApplyOpsDeleteDeathTest = ApplyOpsDeleteTest;
DEATH_TEST_F(ApplyOpsDeleteDeathTest, ApplyOpsDeleteByNullRecordId, "7835000") {
    // Insert a document at a known recordId.
    const RecordId rid(1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid);

    // Try to delete a document by null RecordId in applyOps mode.
    // This should trigger a tassert.
    auto op =
        makeDeleteOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, BSON("_id" << 1), RecordId());
    std::ignore = runOpApplyOpsCmd(op);
}

// =============================================================================
// Tests for change stream pre-images with recordId deletes
// =============================================================================

/**
 * Test fixture for delete oplog entries with recordId on collections with change stream pre-images.
 */
class DeleteWithRecordIdAndPreImagesTest : public OplogApplierImplTest {
protected:
    void setUp() override {
        OplogApplierImplTest::setUp();

        // Setup the pre-images collection.
        ChangeStreamPreImagesCollectionManager::get(_opCtx.get())
            .createPreImagesCollection(_opCtx.get());

        _nss = NamespaceString::createNamespaceString_forTest("test.deleteRecordIdPreImages");
        _uuid = createCollectionWithRecordIdsReplicatedAndPreImages(_opCtx.get(), _nss);
    }

    NamespaceString _nss;
    UUID _uuid = UUID::gen();
};

TEST_F(DeleteWithRecordIdAndPreImagesTest,
       DeleteByRecordIdInSecondaryModeRecordsChangeStreamPreImage) {
    // Insert a document at a known recordId.
    const RecordId rid(1);
    const BSONObj document = BSON("_id" << 1 << "x" << 100 << "data" << "original");
    insertDocumentAtRecordId(_opCtx.get(), _nss, document, rid);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Create a delete oplog entry with recordId.
    OpTime opTime = [opCtx = _opCtx.get()] {
        WriteUnitOfWork wuow{opCtx};
        ScopeGuard guard{[&wuow] {
            wuow.commit();
        }};
        return repl::getNextOpTime(opCtx);
    }();
    auto op = makeDeleteOplogEntryWithRecordId(opTime, _nss, _uuid, BSON("_id" << 1), rid);

    // Apply the delete oplog entry in secondary mode.
    ASSERT_OK(runOpSteadyState(op));

    // Verify the document was deleted.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Verify the pre-image was recorded.
    ChangeStreamPreImageId preImageId{_uuid, op.getOpTime().getTimestamp(), 0};
    BSONObj preImageDocumentKey = BSON("_id" << preImageId.toBSON());
    auto preImageLoadResult =
        getStorageInterface()->findById(_opCtx.get(),
                                        NamespaceString::kChangeStreamPreImagesNamespace,
                                        preImageDocumentKey.firstElement());
    ASSERT_OK(preImageLoadResult);

    // Verify that the pre-image document contains the correct original document.
    const auto preImageDocument =
        ChangeStreamPreImage::parse(preImageLoadResult.getValue(), IDLParserContext{"test"});
    ASSERT_BSONOBJ_EQ(preImageDocument.getPreImage(), document);
    ASSERT_EQUALS(preImageDocument.getOperationTime(), op.getWallClockTime());
}

}  // namespace
}  // namespace repl
}  // namespace mongo

