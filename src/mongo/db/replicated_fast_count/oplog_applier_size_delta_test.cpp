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
#include "mongo/idl/server_parameter_test_controller.h"
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
    RAIIServerParameterControllerForTest _recordIdsFlag{"featureFlagRecordIdsReplicated", true};
    RAIIServerParameterControllerForTest _fastCountFlag{"featureFlagReplicatedFastCount", true};
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
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

TEST_F(SizeDeltaTestDisable, DeleteAbsentSizeMetadataIsSkippedInSecondaryMode) {
    // Entries without m.sz (e.g. from older primaries) must apply without any size check.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    auto op = makeDeleteOplogEntryWithRecordId(nextOpTime(), _nss, _uuid, BSON("_id" << 1), rid);
    ASSERT_OK(runOpSteadyState(op));
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

TEST_F(SizeDeltaTest, DeleteWrongSizeDeltaInInitialSyncModeIsIgnored) {
    // The size check must be skipped entirely during initial sync.
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    auto op = makeDeleteOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), _nss, _uuid, BSON("_id" << 1), rid, -doc.objsize() + 999);
    ASSERT_OK(runOpInitialSync(op));
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

TEST_F(SizeDeltaTestDisable, DeleteWrongSizeDeltaInSecondaryModeReturnsError) {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    // Off-by-one to simulate a primary/secondary size divergence.
    auto op = makeDeleteOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), _nss, _uuid, BSON("_id" << 1), rid, -doc.objsize() + 1);
    ASSERT_EQ(runOpSteadyState(op).code(), 12380200);
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
    ASSERT_EQ(runOpSteadyState(op).code(), 12380201);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
