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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>

namespace mongo {
namespace repl {
namespace {

/**
 * Test fixture for update oplog entries with recordId.
 */
class UpdateWithRecordIdTest : public OplogApplierImplTest {
protected:
    void setUp() override {
        OplogApplierImplTest::setUp();
        _nss = NamespaceString::createNamespaceString_forTest("test.updateRecordId");
        createCollection(_opCtx.get(), _nss, {});
        _uuid = getCollectionUUID(_opCtx.get(), _nss);
    }

    NamespaceString _nss;
    UUID _uuid = UUID::gen();
    RAIIServerParameterControllerForTest featureFlagController =
        RAIIServerParameterControllerForTest("featureFlagRecordIdsReplicated", true);
};

typedef SetSteadyStateConstraints<UpdateWithRecordIdTest, false>
    UpdateWithRecordIdTestDisableSteadyStateConstraints;
typedef SetSteadyStateConstraints<UpdateWithRecordIdTest, true>
    UpdateWithRecordIdTestEnableSteadyStateConstraints;

// =============================================================================
// Tests for kSecondary mode (steady state replication)
// =============================================================================

TEST_F(UpdateWithRecordIdTestEnableSteadyStateConstraints, SuccessInSecondaryMode) {
    // Insert a document at a known recordId.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Create and apply the update oplog entry with recordId.
    auto op = makeUpdateOplogEntryWithRecordId(
        nextOpTime(), _nss, BSON("_id" << 1), BSON("$set" << BSON("x" << 200)), rid);
    ASSERT_OK(runOpSteadyState(op));

    // Verify the document was updated.
    auto updatedDoc = documentAtRecordId(_opCtx.get(), _nss, rid);
    ASSERT_TRUE(updatedDoc.has_value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "x" << 200), updatedDoc.value());
}

TEST_F(UpdateWithRecordIdTestEnableSteadyStateConstraints,
       SuccessWithV2DeltaUpdateInSecondaryMode) {
    // Insert a document at a known recordId with multiple fields.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100 << "y" << 200 << "z" << 300);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Create and apply the update oplog entry with recordId using v2 delta format.
    // Delta format uses doc_diff sections like kUpdateSectionFieldName ("u") for updates.
    auto deltaUpdate = update_oplog_entry::makeDeltaOplogEntry(
        BSON(doc_diff::kUpdateSectionFieldName << BSON("x" << 150 << "y" << 250)));

    auto op =
        makeUpdateOplogEntryWithRecordId(nextOpTime(), _nss, BSON("_id" << 1), deltaUpdate, rid);
    ASSERT_OK(runOpSteadyState(op));

    // Verify the document was updated with only the specified fields changed.
    auto updatedDoc = documentAtRecordId(_opCtx.get(), _nss, rid);
    ASSERT_TRUE(updatedDoc.has_value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "x" << 150 << "y" << 250 << "z" << 300),
                      updatedDoc.value());
}

TEST_F(UpdateWithRecordIdTestEnableSteadyStateConstraints,
       SuccessWithV2DeltaDeleteFieldInSecondaryMode) {
    // Insert a document at a known recordId with multiple fields.
    const RecordId rid(2);
    const BSONObj doc = BSON("_id" << 2 << "keep" << 1 << "remove" << 2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Create and apply the update oplog entry with recordId using v2 delta format
    // to delete a field. kDeleteSectionFieldName ("d") is used for field deletions.
    auto deltaUpdate = update_oplog_entry::makeDeltaOplogEntry(
        BSON(doc_diff::kDeleteSectionFieldName << BSON("remove" << false)));

    auto op =
        makeUpdateOplogEntryWithRecordId(nextOpTime(), _nss, BSON("_id" << 2), deltaUpdate, rid);
    ASSERT_OK(runOpSteadyState(op));

    // Verify the field was deleted from the document.
    auto updatedDoc = documentAtRecordId(_opCtx.get(), _nss, rid);
    ASSERT_TRUE(updatedDoc.has_value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2 << "keep" << 1), updatedDoc.value());
}

TEST_F(UpdateWithRecordIdTestDisableSteadyStateConstraints,
       RecordIdNotFoundInSecondaryModeSucceeds) {
    // Create a recordId that doesn't exist in the collection.
    const RecordId nonExistentRid(999);

    // Verify the recordId doesn't have a document.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, nonExistentRid));

    // TODO SERVER-118695 Application should succeed
    // Create and apply the update oplog entry with the non-existent recordId. Oplog entries with a
    // RecordId will never send an upsert request even when disabling steady state constraints sets
    // alwaysUpsert:true.
    auto op = makeUpdateOplogEntryWithRecordId(
        nextOpTime(), _nss, BSON("_id" << 999), BSON("$set" << BSON("a" << 1)), nonExistentRid);
    ASSERT_EQ(runOpSteadyState(op), ErrorCodes::UpdateOperationFailed);
}

TEST_F(UpdateWithRecordIdTestEnableSteadyStateConstraints, RecordIdNotFoundInSecondaryModeFails) {
    // Create a recordId that doesn't exist in the collection.
    const RecordId nonExistentRid(999);

    // Verify the recordId doesn't have a document.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, nonExistentRid));

    // Create and apply the update oplog entry with the non-existent recordId.
    // With constraints enabled, the recordId path is used (no upsert) and it should fail
    // since the document isn't found.
    auto op = makeUpdateOplogEntryWithRecordId(
        nextOpTime(), _nss, BSON("_id" << 999), BSON("$set" << BSON("a" << 1)), nonExistentRid);
    ASSERT_NOT_OK(runOpSteadyState(op));
}

TEST_F(UpdateWithRecordIdTestDisableSteadyStateConstraints, IdMismatchFails) {
    // Insert a document at a known recordId with _id = 1.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Create an update oplog entry that references the same recordId but with a different _id.
    // With constraints disabled and alwaysUpsert=true, the code falls back to _id-based lookup.
    // Since _id=999 doesn't exist, this will upsert a new document.
    auto op = makeUpdateOplogEntryWithRecordId(
        nextOpTime(), _nss, BSON("_id" << 999), BSON("$set" << BSON("y" << 1)), rid);

    ASSERT_EQ(runOpSteadyState(op).code(), 7834902);
    // Original document should remain unchanged since upsert path was used.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    auto existingDoc = documentAtRecordId(_opCtx.get(), _nss, rid);
    ASSERT_BSONOBJ_EQ(doc, existingDoc.value());
}

TEST_F(UpdateWithRecordIdTestEnableSteadyStateConstraints, IdMismatchFails) {
    // Insert a document at a known recordId with _id = 1.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Create an update oplog entry that references the same recordId but with a different _id.
    // This simulates data corruption where the record at the given rid has a different _id.
    auto op = makeUpdateOplogEntryWithRecordId(
        nextOpTime(), _nss, BSON("_id" << 999), BSON("$set" << BSON("y" << 1)), rid);

    // In kSecondary mode with steady state constraints enabled, this should trigger a uassert
    // and the record should not be updated.
    ASSERT_EQ(runOpSteadyState(op).code(), 7834902);
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    auto unchangedDoc = documentAtRecordId(_opCtx.get(), _nss, rid);
    ASSERT_BSONOBJ_EQ(doc, unchangedDoc.value());
}

TEST_F(UpdateWithRecordIdTestEnableSteadyStateConstraints,
       UpdateWithRecordIdMultipleDocumentsInSecondaryMode) {
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

    // Update the middle document using its recordId.
    auto op = makeUpdateOplogEntryWithRecordId(
        nextOpTime(), _nss, BSON("_id" << 20), BSON("$set" << BSON("x" << 200)), rid2);
    ASSERT_OK(runOpSteadyState(op));

    // Verify only the targeted document was updated.
    auto doc1 = documentAtRecordId(_opCtx.get(), _nss, rid1);
    auto doc2 = documentAtRecordId(_opCtx.get(), _nss, rid2);
    auto doc3 = documentAtRecordId(_opCtx.get(), _nss, rid3);
    ASSERT_BSONOBJ_EQ(BSON("_id" << 10 << "x" << 1), doc1.value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 20 << "x" << 200), doc2.value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 30 << "x" << 3), doc3.value());
}

// =============================================================================
// Tests for kInitialSync mode
// =============================================================================

TEST_F(UpdateWithRecordIdTest, UpdateWithRecordIdSuccessInInitialSyncMode) {
    // Insert a document at a known recordId.
    const RecordId rid(2);
    const BSONObj doc = BSON("_id" << 2 << "x" << 200);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Create and apply the update oplog entry with recordId in initial sync mode.
    auto op = makeUpdateOplogEntryWithRecordId(
        nextOpTime(), _nss, BSON("_id" << 2), BSON("$set" << BSON("x" << 300)), rid);
    ASSERT_OK(runOpInitialSync(op));

    // Verify the document was updated.
    auto updatedDoc = documentAtRecordId(_opCtx.get(), _nss, rid);
    ASSERT_TRUE(updatedDoc.has_value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2 << "x" << 300), updatedDoc.value());
}

TEST_F(UpdateWithRecordIdTest, UpdateWithRecordIdNotFoundInInitialSyncMode) {
    // Create a recordId that doesn't exist in the collection.
    const RecordId nonExistentRid(888);

    // Verify the recordId doesn't have a document.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, nonExistentRid));

    // Create and apply the update oplog entry with the non-existent recordId.
    // In initial sync mode, this should succeed (noop) since we're more lenient.
    auto op = makeUpdateOplogEntryWithRecordId(
        nextOpTime(), _nss, BSON("_id" << 888), BSON("$set" << BSON("a" << 1)), nonExistentRid);
    ASSERT_OK(runOpInitialSync(op));
}

TEST_F(UpdateWithRecordIdTest, UpdateWithRecordIdMismatchIdInInitialSyncModeSucceeds) {
    // Insert a document at a known recordId with _id = 3.
    const RecordId rid(3);
    const BSONObj doc = BSON("_id" << 3 << "x" << 300);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Create an update oplog entry that references the same recordId but with a different _id.
    // In initial sync mode, _id mismatches are ignored (the operation returns OK without
    // performing the update).
    auto op = makeUpdateOplogEntryWithRecordId(
        nextOpTime(), _nss, BSON("_id" << 777), BSON("$set" << BSON("y" << 1)), rid);
    ASSERT_OK(runOpInitialSync(op));

    // In initial sync mode, the _id mismatch is ignored and the function returns Status::OK()
    // without updating the document.
    auto unchangedDoc = documentAtRecordId(_opCtx.get(), _nss, rid);
    ASSERT_TRUE(unchangedDoc.has_value());
    ASSERT_BSONOBJ_EQ(doc, unchangedDoc.value());
}

TEST_F(UpdateWithRecordIdTest, UpdateWithRecordIdMultipleDocumentsInInitialSyncMode) {
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

    // Update the first document using its recordId.
    auto op = makeUpdateOplogEntryWithRecordId(
        nextOpTime(), _nss, BSON("_id" << 100), BSON("$set" << BSON("a" << 100)), rid1);
    ASSERT_OK(runOpInitialSync(op));

    // Verify only the targeted document was updated.
    auto doc1 = documentAtRecordId(_opCtx.get(), _nss, rid1);
    auto doc2 = documentAtRecordId(_opCtx.get(), _nss, rid2);
    auto doc3 = documentAtRecordId(_opCtx.get(), _nss, rid3);
    ASSERT_BSONOBJ_EQ(BSON("_id" << 100 << "a" << 100), doc1.value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 200 << "a" << 2), doc2.value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 300 << "a" << 3), doc3.value());
}

TEST_F(UpdateWithRecordIdTest, FcbisScenarioWithMismatchedRecordIdsInInitialSyncMode) {
    // Scenario: Document is cloned via FCBIS at rid: 2 with {_id: 1, a: 2}.
    // Then the following oplogs are applied in initial sync mode:
    // 1. update {_id: 1, rid: 1, a: 3} - targets rid: 1 which doesn't exist (no-op)
    // 2. delete {_id: 1, rid: 1} - targets rid: 1 which doesn't exist (no-op)
    // 3. insert {_id: 1, rid: 2, a: 2} - targets rid: 2 which already has a document (skipped)

    // Step 1: Simulate FCBIS clone - insert document at rid: 2.
    const RecordId clonedRid(2);
    const BSONObj clonedDoc = BSON("_id" << 1 << "a" << 2);
    insertDocumentAtRecordId(_opCtx.get(), _nss, clonedDoc, clonedRid);

    // Verify the cloned document exists at rid: 2.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, clonedRid));

    // Step 2: Apply update oplog entry targeting rid: 1 (should be no-op since rid: 1 doesn't
    // exist).
    const RecordId originalRid(1);
    auto updateOp = makeUpdateOplogEntryWithRecordId(
        nextOpTime(), _nss, BSON("_id" << 1), BSON("$set" << BSON("a" << 3)), originalRid);
    ASSERT_OK(runOpInitialSync(updateOp));

    // Verify rid: 1 still doesn't exist and rid: 2 document is unchanged.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, originalRid));
    auto docAfterUpdate = documentAtRecordId(_opCtx.get(), _nss, clonedRid);
    ASSERT_TRUE(docAfterUpdate.has_value());
    ASSERT_BSONOBJ_EQ(clonedDoc, docAfterUpdate.value());

    // Step 3: Apply delete oplog entry targeting rid: 1 (should be no-op since rid: 1 doesn't
    // exist).
    auto deleteOp =
        makeDeleteOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, BSON("_id" << 1), originalRid);
    ASSERT_OK(runOpInitialSync(deleteOp));

    // Verify rid: 1 still doesn't exist and rid: 2 document is unchanged.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, originalRid));
    auto docAfterDelete = documentAtRecordId(_opCtx.get(), _nss, clonedRid);
    ASSERT_TRUE(docAfterDelete.has_value());
    ASSERT_BSONOBJ_EQ(clonedDoc, docAfterDelete.value());

    // Step 4: Apply insert oplog entry targeting rid: 2. Since the document already exists at
    // rid: 2 (from FCBIS clone), this insert is skipped during initial sync (DuplicateKey errors
    // are handled by returning Status::OK()).
    auto insertOp = makeInsertOplogEntryWithRecordId(
        nextOpTime(), _nss, _uuid, BSON("_id" << 1 << "a" << 2), clonedRid);
    ASSERT_OK(runOpInitialSync(insertOp));

    // Verify the final state: document at rid: 2 should still be the cloned document.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, originalRid));
    auto finalDoc = documentAtRecordId(_opCtx.get(), _nss, clonedRid);
    ASSERT_TRUE(finalDoc.has_value());
    ASSERT_BSONOBJ_EQ(clonedDoc, finalDoc.value());
}

// =============================================================================
// Tests for kApplyOpsCmd mode (applyOps command)
// =============================================================================

/**
 * Test fixture for update oplog entries in applyOps mode on a recordIdsReplicated collection.
 */
class ApplyOpsUpdateTest : public UpdateWithRecordIdTest {
protected:
    Status runOpApplyOpsCmd(const OplogEntry& op) {
        return _applyOplogEntryOrGroupedInsertsWrapper(
            _opCtx.get(), ApplierOperation{&op}, OplogApplication::Mode::kApplyOpsCmd);
    }
};

TEST_F(ApplyOpsUpdateTest, UpdateByIdSucceeds) {
    // Insert a document at a known recordId.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Create and apply an update oplog entry WITHOUT recordId in applyOps mode.
    // In kApplyOpsCmd mode, updates without recordId are allowed even on recordIdsReplicated
    // collections.
    auto op = makeUpdateDocumentOplogEntry(
        nextOpTime(), _nss, BSON("_id" << 1), BSON("$set" << BSON("x" << 500)));
    ASSERT_OK(runOpApplyOpsCmd(op));

    // Verify the document was updated.
    auto updatedDoc = documentAtRecordId(_opCtx.get(), _nss, rid);
    ASSERT_TRUE(updatedDoc.has_value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "x" << 500), updatedDoc.value());
}

TEST_F(ApplyOpsUpdateTest, ApplyOpsUpdateByIdMultipleDocumentsSucceeds) {
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

    // Update the middle document without recordId in applyOps mode.
    auto op = makeUpdateDocumentOplogEntry(
        nextOpTime(), _nss, BSON("_id" << 20), BSON("$set" << BSON("value" << "updated")));
    ASSERT_OK(runOpApplyOpsCmd(op));

    // Verify only the targeted document was updated.
    auto doc1 = documentAtRecordId(_opCtx.get(), _nss, rid1);
    auto doc2 = documentAtRecordId(_opCtx.get(), _nss, rid2);
    auto doc3 = documentAtRecordId(_opCtx.get(), _nss, rid3);
    ASSERT_BSONOBJ_EQ(BSON("_id" << 10 << "value" << "a"), doc1.value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 20 << "value" << "updated"), doc2.value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 30 << "value" << "c"), doc3.value());
}

TEST_F(ApplyOpsUpdateTest, ApplyOpsUpdateByIdDocumentNotFoundFails) {
    // Insert a document at a known recordId.
    const RecordId rid(1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid);

    // Try to update a document that doesn't exist in applyOps mode.
    // This should succeed as a no-op.
    auto op = makeUpdateDocumentOplogEntry(
        nextOpTime(), _nss, BSON("_id" << 999), BSON("$set" << BSON("x" << 1)));
    // The applyOps command will set `alwaysUpsert` to false, causing an update with no matched
    // documents to return an error status.
    ASSERT_NOT_OK(runOpApplyOpsCmd(op));

    // The existing document should still be there and unchanged.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    auto unchangedDoc = documentAtRecordId(_opCtx.get(), _nss, rid);
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "x" << 100), unchangedDoc.value());
}

TEST_F(ApplyOpsUpdateTest, ApplyOpsUpdateWithUpsertSucceeds) {
    // Insert a document at a known recordId.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Create and apply an update oplog entry with upsert:true but WITHOUT a recordId.
    // This should succeed because the upsert flag is only disallowed when combined with a recordId.
    auto op = makeUpdateOplogEntryWithUpsert(
        nextOpTime(), _nss, BSON("_id" << 1), BSON("$set" << BSON("x" << 600)));
    ASSERT_OK(runOpApplyOpsCmd(op));

    // Verify the document was updated.
    auto updatedDoc = documentAtRecordId(_opCtx.get(), _nss, rid);
    ASSERT_TRUE(updatedDoc.has_value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "x" << 600), updatedDoc.value());
}

using ApplyOpsUpdateDeathTest = ApplyOpsUpdateTest;
DEATH_TEST_F(ApplyOpsUpdateDeathTest, ApplyOpsUpdateWithUpsertAndRecordIdFails, "7834905") {
    // Insert a document at a known recordId.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Create an update oplog entry with both upsert:true AND a recordId.
    // This should fail with error 7834905 because oplog entries with upsert:true
    // are not allowed to also contain a RecordId.
    auto op = makeUpdateOplogEntryWithUpsertAndRecordId(
        nextOpTime(), _nss, BSON("_id" << 1), BSON("$set" << BSON("x" << 700)), rid);
    std::ignore = runOpApplyOpsCmd(op);
}

DEATH_TEST_F(ApplyOpsUpdateDeathTest, ApplyOpsUpdateByNullRecordId, "7835000") {
    // Insert a document at a known recordId.
    const RecordId rid(1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, BSON("_id" << 1 << "x" << 100), rid);

    // Try to update a document by null RecordId in applyOps mode.
    // This should trigger a tassert because updateObjectByRid doesn't support null recordId.
    auto op = makeUpdateOplogEntryWithRecordId(
        nextOpTime(), _nss, BSON("_id" << 1), BSON("$set" << BSON("x" << 200)), RecordId());
    std::ignore = runOpApplyOpsCmd(op);
}

// =============================================================================
// Tests for change stream pre-images with recordId updates
// =============================================================================

/**
 * Test fixture for update oplog entries with recordId on collections with change stream pre-images.
 */
class UpdateWithRecordIdAndPreImagesTest : public OplogApplierImplTest {
protected:
    void setUp() override {
        OplogApplierImplTest::setUp();

        // Setup the pre-images collection.
        ChangeStreamPreImagesCollectionManager::get(_opCtx.get())
            .createPreImagesCollection(_opCtx.get());

        _nss = NamespaceString::createNamespaceString_forTest("test.updateRecordIdPreImages");
        createCollectionWithPreImages(_opCtx.get(), _nss);
        _uuid = getCollectionUUID(_opCtx.get(), _nss);
    }

    NamespaceString _nss;
    UUID _uuid = UUID::gen();
    RAIIServerParameterControllerForTest featureFlagController =
        RAIIServerParameterControllerForTest("featureFlagRecordIdsReplicated", true);
};

TEST_F(UpdateWithRecordIdAndPreImagesTest,
       UpdateByRecordIdInSecondaryModeRecordsChangeStreamPreImage) {
    // Insert a document at a known recordId.
    const RecordId rid(1);
    const BSONObj document = BSON("_id" << 1 << "x" << 100 << "data" << "original");
    insertDocumentAtRecordId(_opCtx.get(), _nss, document, rid);

    // Verify the document exists.
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));

    // Create an update oplog entry with recordId.
    OpTime opTime = [opCtx = _opCtx.get()] {
        WriteUnitOfWork wuow{opCtx};
        ScopeGuard guard{[&wuow] {
            wuow.commit();
        }};
        return repl::getNextOpTime(opCtx);
    }();
    auto op = makeUpdateOplogEntryWithRecordId(
        opTime, _nss, BSON("_id" << 1), BSON("$set" << BSON("x" << 200)), rid);

    // Apply the update oplog entry in secondary mode.
    ASSERT_OK(runOpSteadyState(op));

    // Verify the document was updated.
    auto updatedDoc = documentAtRecordId(_opCtx.get(), _nss, rid);
    ASSERT_TRUE(updatedDoc.has_value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "x" << 200 << "data" << "original"), updatedDoc.value());

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

using UpdateWithRecordIdAndPreImagesDeathTest = UpdateWithRecordIdAndPreImagesTest;
DEATH_TEST_F(UpdateWithRecordIdAndPreImagesDeathTest,
             UpdateByRecordIdWithPreImagesDocumentNotFoundDies,
             "invariant") {
    // Do NOT insert a document at the target recordId - it should not exist.
    const RecordId nonExistentRid(999);

    // Verify the recordId doesn't have a document.
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, nonExistentRid));

    // Create an update oplog entry with the non-existent recordId.
    OpTime opTime = [opCtx = _opCtx.get()] {
        WriteUnitOfWork wuow{opCtx};
        ScopeGuard guard{[&wuow] {
            wuow.commit();
        }};
        return repl::getNextOpTime(opCtx);
    }();
    auto op = makeUpdateOplogEntryWithRecordId(
        opTime, _nss, BSON("_id" << 999), BSON("$set" << BSON("x" << 200)), nonExistentRid);

    // Apply the update oplog entry in secondary mode. Since change stream pre-images are enabled
    // and the document doesn't exist, we cannot retrieve the pre-image and the invariant will fail.
    std::ignore = runOpSteadyState(op);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
