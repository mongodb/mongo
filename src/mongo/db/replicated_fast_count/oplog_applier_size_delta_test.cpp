// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Tests for the oplog application invariant that verifies the replicated size delta (m.sz) in
 * delete and update oplog entries matches the actual size change applied on a secondary.
 */

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

// ---------------------------------------------------------------------------
// Shared fixture
// ---------------------------------------------------------------------------

class SizeDeltaTest : public OplogApplierImplTest {
protected:
    void setUp() override {
        OplogApplierImplTest::setUp();
        _nss = NamespaceString::createNamespaceString_forTest("test.sizeDelta");
        createCollection(_opCtx.get(), _nss, {});
        _uuid = getCollectionUUID(_opCtx.get(), _nss);
    }

    NamespaceString _nss;
    UUID _uuid = UUID::gen();
    unittest::ServerParameterGuard _recordIdsFlag{"featureFlagRecordIdsReplicated", true};
    unittest::ServerParameterGuard _fastCountFlag{"featureFlagReplicatedFastCount", true};
};

typedef SetSteadyStateConstraints<SizeDeltaTest, false> SizeDeltaTestDisable;

// ---------------------------------------------------------------------------
// Delete tests
// ---------------------------------------------------------------------------

TEST_F(SizeDeltaTestDisable, DeleteCorrectSizeDeltaInSecondaryModeSucceeds) {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    auto op = makeDeleteOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), _nss, _uuid, BSON("_id" << 1), rid, -doc.objsize());
    ASSERT_OK(runOpSteadyState(op));
    EXPECT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

TEST_F(SizeDeltaTestDisable, DeleteAbsentSizeMetadataIsSkippedInSecondaryMode) {
    // Entries without m.sz (e.g. from older primaries) must apply without any size check.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    auto op = makeDeleteOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, BSON("_id" << 1), rid);
    ASSERT_OK(runOpSteadyState(op));
    EXPECT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

TEST_F(SizeDeltaTestDisable, DeletePresentSizeMetadataWithAbsentSzIsSkippedInSecondaryMode) {
    // A present SingleOpSizeMetadata whose sz is absent must apply without any size check.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    auto op =
        makeDeleteOplogEntryWithRecordIdWithoutSz(nextOpTime(), _nss, _uuid, BSON("_id" << 1), rid);
    ASSERT_OK(runOpSteadyState(op));
    EXPECT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

TEST_F(SizeDeltaTest, DeleteWrongSizeDeltaInInitialSyncModeIsIgnored) {
    // The size check must be skipped entirely during initial sync.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    auto op = makeDeleteOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), _nss, _uuid, BSON("_id" << 1), rid, -doc.objsize() + 999);
    ASSERT_OK(runOpInitialSync(op));
    EXPECT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

TEST_F(SizeDeltaTestDisable, DeleteWrongSizeDeltaInSecondaryModeReturnsError) {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Off-by-one to simulate a primary/secondary size divergence.
    auto op = makeDeleteOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), _nss, _uuid, BSON("_id" << 1), rid, -doc.objsize() + 1);
    EXPECT_EQ(runOpSteadyState(op).code(), 12380200);
}

// ---------------------------------------------------------------------------
// Update tests
// ---------------------------------------------------------------------------

TEST_F(SizeDeltaTestDisable, UpdateCorrectSizeDeltaWhenSizeUnchangedSucceeds) {
    // Replacing an int field with another int (same size): delta = 0.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    const BSONObj afterDoc = BSON("_id" << 1 << "x" << 200);
    const int sizeDelta = afterDoc.objsize() - doc.objsize();  // 0

    auto op = makeUpdateOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), _nss, BSON("_id" << 1), BSON("$set" << BSON("x" << 200)), rid, sizeDelta);
    ASSERT_OK(runOpSteadyState(op));

    auto updated = documentAtRecordId(_opCtx.get(), _nss, rid);
    ASSERT_TRUE(updated.has_value());
    ASSERT_BSONOBJ_EQ(afterDoc, updated.value());
}

TEST_F(SizeDeltaTestDisable, UpdateCorrectSizeDeltaWhenSizeGrowsSucceeds) {
    // Adding a new field increases the document size.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    const BSONObj afterDoc = BSON("_id" << 1 << "y" << 1);
    const int sizeDelta = afterDoc.objsize() - doc.objsize();

    auto op = makeUpdateOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), _nss, BSON("_id" << 1), BSON("$set" << BSON("y" << 1)), rid, sizeDelta);
    ASSERT_OK(runOpSteadyState(op));

    auto updated = documentAtRecordId(_opCtx.get(), _nss, rid);
    ASSERT_TRUE(updated.has_value());
    ASSERT_BSONOBJ_EQ(afterDoc, updated.value());
}

TEST_F(SizeDeltaTestDisable, UpdateAbsentSizeMetadataIsSkippedInSecondaryMode) {
    // Entries without m.sz must apply without any size check.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    auto op = makeUpdateOplogEntryWithRecordId(
        nextOpTime(), _nss, BSON("_id" << 1), BSON("$set" << BSON("x" << 200)), rid);
    ASSERT_OK(runOpSteadyState(op));
}

TEST_F(SizeDeltaTestDisable, UpdatePresentSizeMetadataWithAbsentSzIsSkippedInSecondaryMode) {
    // A present SingleOpSizeMetadata whose sz is absent must apply without any size check.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    auto op = makeUpdateOplogEntryWithRecordIdWithoutSz(
        nextOpTime(), _nss, BSON("_id" << 1), BSON("$set" << BSON("x" << 200)), rid);
    ASSERT_OK(runOpSteadyState(op));
}

TEST_F(SizeDeltaTest, UpdateWrongSizeDeltaInInitialSyncModeIsIgnored) {
    // The size check must be skipped entirely during initial sync.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    auto op = makeUpdateOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), _nss, BSON("_id" << 1), BSON("$set" << BSON("x" << 200)), rid, 999);
    ASSERT_OK(runOpInitialSync(op));
}

TEST_F(SizeDeltaTestDisable, UpdateWrongSizeDeltaInSecondaryModeReturnsError) {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // The real delta is 0 (int -> int, same size), but we claim 42 to simulate divergence.
    auto op = makeUpdateOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), _nss, BSON("_id" << 1), BSON("$set" << BSON("x" << 200)), rid, 42);
    EXPECT_EQ(runOpSteadyState(op).code(), 12380201);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
