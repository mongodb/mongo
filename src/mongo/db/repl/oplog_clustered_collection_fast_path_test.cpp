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

// Tests for the oplog-apply fast path for collections clustered on _id. For such collections the
// RecordId is deterministically derivable from the _id field, so oplog application can derive the
// RecordId and operate on the record store directly instead of going through the query system,
// exactly as the recordIdsReplicated fast path does. The fast path is gated on the persistence
// provider and otherwise on the FCV-gated featureFlagClusteredCollectionOplogApplyFastPath.

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional.hpp>

namespace mongo {
namespace repl {
namespace {

// The error code raised by updateObjectByRid via assertOnInconsistentDocuments when the derived
// RecordId is not found in secondary mode. Observing this code proves the fast path was taken: the
// query path would instead report ErrorCodes::UpdateOperationFailed for a missing document.
constexpr int kRecordNotFoundInFastPathCode = 11902401;

class ClusteredCollectionFastPathTest : public OplogApplierImplTest {
protected:
    void setUp() override {
        OplogApplierImplTest::setUp();
        _nss = NamespaceString::createNamespaceString_forTest("test.clusteredFastPath");
        CollectionOptions options;
        options.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
        createCollection(_opCtx.get(), _nss, options);
    }

    // Inserts a document through the normal oplog insert path so its RecordId is derived from _id.
    void insertDoc(const BSONObj& doc) {
        ASSERT_OK(runOpSteadyState(makeInsertDocumentOplogEntry(nextOpTime(), _nss, doc)));
    }

    // Computes the RecordId a clustered-on-_id collection would assign to a document with the given
    // _id element.
    static RecordId ridForId(const BSONObj& idObj) {
        return record_id_helpers::keyForElem(idObj.firstElement());
    }

    NamespaceString _nss;
    // Enable the fast path via the feature flag (the default attached persistence provider does not
    // mandate it).
    unittest::ServerParameterGuard _fastPathFlag{"featureFlagClusteredCollectionOplogApplyFastPath",
                                                 true};
};

using ClusteredCollectionFastPathTestEnableSteadyStateConstraints =
    SetSteadyStateConstraints<ClusteredCollectionFastPathTest, true>;

// A clustered collection with the fast path disabled: the feature flag is forced off so the
// gating falls back to the (attached) persistence provider, which does not mandate the fast path.
class ClusteredCollectionFastPathDisabledTest : public ClusteredCollectionFastPathTest {
protected:
    unittest::ServerParameterGuard _fastPathFlagOff{
        "featureFlagClusteredCollectionOplogApplyFastPath", false};
};

using ClusteredCollectionFastPathDisabledTestEnableSteadyStateConstraints =
    SetSteadyStateConstraints<ClusteredCollectionFastPathDisabledTest, true>;

// =============================================================================
// Update fast path
// =============================================================================

TEST_F(ClusteredCollectionFastPathTest, UpdateAppliesViaFastPath) {
    insertDoc(BSON("_id" << 1 << "x" << 100));

    auto op = makeUpdateDocumentOplogEntry(
        nextOpTime(), _nss, BSON("_id" << 1) /* o2 */, BSON("$set" << BSON("x" << 200)) /* o */);
    ASSERT_OK(runOpSteadyState(op));

    // The document should be updated and remain at the RecordId derived from its _id.
    auto updated = documentAtRecordId(_opCtx.get(), _nss, ridForId(BSON("_id" << 1)));
    ASSERT_TRUE(updated.has_value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "x" << 200), updated.value());
}

TEST_F(ClusteredCollectionFastPathTest, UpdateWithDeltaAppliesViaFastPath) {
    insertDoc(BSON("_id" << 7 << "a" << 1 << "b" << 2));

    auto deltaUpdate = update_oplog_entry::makeDeltaOplogEntry(
        BSON(doc_diff::kUpdateSectionFieldName << BSON("a" << 50)));
    auto op = makeUpdateDocumentOplogEntry(nextOpTime(), _nss, BSON("_id" << 7), deltaUpdate);
    ASSERT_OK(runOpSteadyState(op));

    auto updated = documentAtRecordId(_opCtx.get(), _nss, ridForId(BSON("_id" << 7)));
    ASSERT_TRUE(updated.has_value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 7 << "a" << 50 << "b" << 2), updated.value());
}

// Covers all _id types valid for a clustered collection, including OID (the time-series bucket
// case, where buckets are internally clustered on an OID _id).
TEST_F(ClusteredCollectionFastPathTest, UpdateAppliesViaFastPathForAllIdTypes) {
    const OID oid = OID::gen();
    const std::vector<BSONObj> ids = {
        BSON("_id" << 42),                                      // int32
        BSON("_id" << 9000000000LL),                            // int64
        BSON("_id" << "stringId"),                              // string
        BSON("_id" << oid),                                     // OID (time-series bucket case)
        BSON("_id" << BSON("nested" << 1 << "more" << "two")),  // subdocument
    };

    for (const auto& idObj : ids) {
        BSONObjBuilder docBuilder;
        docBuilder.appendElements(idObj);
        docBuilder.append("v", 1);
        insertDoc(docBuilder.obj());

        auto op =
            makeUpdateDocumentOplogEntry(nextOpTime(), _nss, idObj, BSON("$set" << BSON("v" << 2)));
        ASSERT_OK(runOpSteadyState(op));

        auto updated = documentAtRecordId(_opCtx.get(), _nss, ridForId(idObj));
        ASSERT_TRUE(updated.has_value()) << "missing doc for _id " << idObj.toString();
        ASSERT_EQ(2, updated.value()["v"].numberInt());
    }
}

// A missing document on a clustered collection update is reported through the fast path's
// inconsistent-document handler (proving the fast path engaged) rather than the query path.
TEST_F(ClusteredCollectionFastPathTestEnableSteadyStateConstraints,
       MissingDocUpdateInSecondaryModeReportsViaFastPath) {
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, ridForId(BSON("_id" << 1234))));

    auto op = makeUpdateDocumentOplogEntry(
        nextOpTime(), _nss, BSON("_id" << 1234), BSON("$set" << BSON("x" << 1)));
    ASSERT_EQ(kRecordNotFoundInFastPathCode, runOpSteadyState(op).code());
}

// With the feature flag off and the attached persistence provider (which does not mandate the fast
// path), a missing-document update is handled by the query path instead. We observe a different
// (non-fast-path) error code, proving the gating disables the fast path.
TEST_F(ClusteredCollectionFastPathDisabledTestEnableSteadyStateConstraints,
       MissingDocUpdateFallsThroughToQueryPath) {
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, ridForId(BSON("_id" << 1234))));

    auto op = makeUpdateDocumentOplogEntry(
        nextOpTime(), _nss, BSON("_id" << 1234), BSON("$set" << BSON("x" << 1)));
    auto status = runOpSteadyState(op);
    ASSERT_NOT_OK(status);
    ASSERT_NE(kRecordNotFoundInFastPathCode, status.code());
    ASSERT_EQ(ErrorCodes::UpdateOperationFailed, status.code());
}

// An upsert update oplog entry must NOT take the fast path even when it is enabled: the fast path
// suppresses upsert and would fatally assert on the (legitimately) missing document. It instead
// falls through to the upsert-aware query path and inserts the document. This mirrors
// config.transactions, whose updates are generated with upsert:true (see session_update_tracker).
TEST_F(ClusteredCollectionFastPathTest, UpsertOplogEntryFallsThroughAndInserts) {
    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, ridForId(BSON("_id" << 99))));

    auto op = makeUpdateOplogEntryWithUpsert(
        nextOpTime(), _nss, BSON("_id" << 99), BSON("_id" << 99 << "x" << 1));
    ASSERT_OK(runOpSteadyState(op));

    // The document was upserted via the query path and lands at the RecordId derived from its _id.
    auto inserted = documentAtRecordId(_opCtx.get(), _nss, ridForId(BSON("_id" << 99)));
    ASSERT_TRUE(inserted.has_value());
    ASSERT_BSONOBJ_EQ(BSON("_id" << 99 << "x" << 1), inserted.value());
}

// =============================================================================
// Delete fast path
// =============================================================================

TEST_F(ClusteredCollectionFastPathTest, DeleteAppliesViaFastPath) {
    insertDoc(BSON("_id" << 1 << "x" << 100));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, ridForId(BSON("_id" << 1))));

    // For deletes the _id is carried in 'o' (the query selector).
    auto op = makeDeleteDocumentOplogEntry(nextOpTime(), _nss, BSON("_id" << 1));
    ASSERT_OK(runOpSteadyState(op));

    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, ridForId(BSON("_id" << 1))));
}

TEST_F(ClusteredCollectionFastPathTest, DeleteAppliesViaFastPathForOidId) {
    const OID oid = OID::gen();
    insertDoc(BSON("_id" << oid << "x" << 1));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, ridForId(BSON("_id" << oid))));

    auto op = makeDeleteDocumentOplogEntry(nextOpTime(), _nss, BSON("_id" << oid));
    ASSERT_OK(runOpSteadyState(op));

    ASSERT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, ridForId(BSON("_id" << oid))));
}

}  // namespace
}  // namespace repl
}  // namespace mongo
