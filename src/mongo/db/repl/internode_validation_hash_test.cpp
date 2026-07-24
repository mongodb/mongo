// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/internode_validation_hash_utils.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace repl {
namespace {

/**
 * End-to-end tests that exercise the per-document hash check.
 */
class VerifyValidationHashTest : public OplogApplierImplTest {
protected:
    void setUp() override {
        OplogApplierImplTest::setUp();
        _nss = NamespaceString::createNamespaceString_forTest("test.verifyValidationHash");
        createCollection(_opCtx.get(), _nss, {});
        _uuid = getCollectionUUID(_opCtx.get(), _nss);

        // An unsupported collection (without replicated record IDs). With
        // featureFlagRecordIdsReplicated enabled, collections default to recordIdsReplicated:true,
        // so we must force it off at creation time to get a genuinely unsupported collection.
        _plainNss = NamespaceString::createNamespaceString_forTest("test.plainCollection");
        {
            FailPointEnableBlock overrideRecordIds("overrideRecordIdsReplicatedFalse");
            createCollection(_opCtx.get(), _plainNss, {});
        }
    }

    /**
     * Helper function used for testing shouldVerifyValidationHash. It constructs a minimal insert
     * oplog entry with the given hash (if any) and calls shouldVerifyValidationHash, passing a
     * CollectionPtr acquired from the catalog so its areRecordIdsReplicated() predicate is
     * evaluated against the acquired collection's actual metadata.
     */
    bool shouldVerify(OperationContext* opCtx,
                      const NamespaceString& nss,
                      OplogApplication::Mode mode,
                      boost::optional<int64_t> h) {
        const RecordId rid(1);
        const BSONObj doc = BSON("_id" << 1);
        const OplogEntry op = h
            ? makeInsertOplogEntryWithRecordIdAndHash(nextOpTime(), nss, _uuid, doc, rid, *h)
            : makeInsertOplogEntryWithRecordId(nextOpTime(), nss, _uuid, doc, rid);

        auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead),
            MODE_IS);
        return shouldVerifyValidationHash(opCtx, coll.getCollectionPtr(), mode, op);
    }

    NamespaceString _nss;
    NamespaceString _plainNss;
    UUID _uuid = UUID::gen();

    unittest::ServerParameterGuard _recordIdsReplicatedFlag{"featureFlagRecordIdsReplicated", true};
    unittest::ServerParameterGuard _validationFlag{
        "featureFlagContinuousInternodeValidationPerDocument", true};

    /**
     * Exercises the grouped inserts path in the oplog applier. It takes a vector of
     * ApplierOperation objects and applies them together as a grouped insert in steady-state
     * secondary mode, using the oplog applier wrapper function.
     */
    Status applyGroupedInsertsSteadyState(std::vector<ApplierOperation>& ops) {
        return _applyOplogEntryOrGroupedInsertsWrapper(
            _opCtx.get(),
            OplogEntryOrGroupedInserts(ops.begin(), ops.end()),
            OplogApplication::Mode::kSecondary);
    }
};

TEST_F(VerifyValidationHashTest, MatchingHashAppliesCleanly) {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    const int64_t hash = computeDocValidationHash(doc);

    OplogEntry op =
        makeInsertOplogEntryWithRecordIdAndHash(nextOpTime(), _nss, _uuid, doc, rid, hash);
    ASSERT_OK(runOpSteadyState(op));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    ASSERT_BSONOBJ_EQ(doc, *documentAtRecordId(_opCtx.get(), _nss, rid));
}

// shouldVerifyValidationHash returns true with steady-state secondary, feature enabled, supported
// collection, hash present.
TEST_F(VerifyValidationHashTest, TrueWhenAllConditionsMet) {
    EXPECT_TRUE(shouldVerify(_opCtx.get(), _nss, OplogApplication::Mode::kSecondary, int64_t{123}));
}

// shouldVerifyValidationHash returns false when the feature flag is disabled.
TEST_F(VerifyValidationHashTest, FalseWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard disableValidation(
        "featureFlagContinuousInternodeValidationPerDocument", false);
    EXPECT_FALSE(
        shouldVerify(_opCtx.get(), _nss, OplogApplication::Mode::kSecondary, int64_t{123}));
}

// shouldVerifyValidationHash returns false when the hash is absent.
TEST_F(VerifyValidationHashTest, FalseWhenHashAbsent) {
    EXPECT_FALSE(shouldVerify(_opCtx.get(), _nss, OplogApplication::Mode::kSecondary, boost::none));
}

// shouldVerifyValidationHash returns false for non-steady-state secondary modes.
TEST_F(VerifyValidationHashTest, FalseForNonSecondaryModes) {
    for (const auto mode : {OplogApplication::Mode::kInitialSync,
                            OplogApplication::Mode::kUnstableRecovering,
                            OplogApplication::Mode::kStableRecovering,
                            OplogApplication::Mode::kApplyOpsCmd}) {
        EXPECT_FALSE(shouldVerify(_opCtx.get(), _nss, mode, int64_t{123}))
            << OplogApplication::modeToString(mode);
    }
}

// shouldVerifyValidationHash returns false for an unsupported (non-recordIdsReplicated) collection.
TEST_F(VerifyValidationHashTest, FalseForUnsupportedCollection) {
    EXPECT_FALSE(
        shouldVerify(_opCtx.get(), _plainNss, OplogApplication::Mode::kSecondary, int64_t{123}));
}

using VerifyValidationHashDeathTest = VerifyValidationHashTest;

DEATH_TEST_F(VerifyValidationHashDeathTest, MismatchedHashFasserts, "12851600") {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    const int64_t wrongHash = computeDocValidationHash(doc) ^ 0x1;

    OplogEntry op =
        makeInsertOplogEntryWithRecordIdAndHash(nextOpTime(), _nss, _uuid, doc, rid, wrongHash);
    std::ignore = runOpSteadyState(op);
}

TEST_F(VerifyValidationHashTest, GroupedInsertsMatchingHashAppliesCleanly) {
    const RecordId rid1(1);
    const RecordId rid2(2);
    const BSONObj doc1 = BSON("_id" << 1 << "x" << 100);
    const BSONObj doc2 = BSON("_id" << 2 << "x" << 200);

    OplogEntry op1 = makeInsertOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, _uuid, doc1, rid1, computeDocValidationHash(doc1));
    OplogEntry op2 = makeInsertOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, _uuid, doc2, rid2, computeDocValidationHash(doc2));
    std::vector<ApplierOperation> ops = {ApplierOperation{&op1}, ApplierOperation{&op2}};
    ASSERT_OK(applyGroupedInsertsSteadyState(ops));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid1));
    ASSERT_BSONOBJ_EQ(doc1, *documentAtRecordId(_opCtx.get(), _nss, rid1));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid2));
    ASSERT_BSONOBJ_EQ(doc2, *documentAtRecordId(_opCtx.get(), _nss, rid2));
}

DEATH_TEST_F(VerifyValidationHashDeathTest,
             GroupedInsertsMismatchOnSecondEntryFasserts,
             "12851600") {
    const RecordId rid1(1);
    const RecordId rid2(2);
    const BSONObj doc1 = BSON("_id" << 1 << "x" << 100);
    const BSONObj doc2 = BSON("_id" << 2 << "x" << 200);

    OplogEntry op1 = makeInsertOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, _uuid, doc1, rid1, computeDocValidationHash(doc1));
    OplogEntry op2 = makeInsertOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, _uuid, doc2, rid2, computeDocValidationHash(doc2) ^ 0x1);
    std::vector<ApplierOperation> ops = {ApplierOperation{&op1}, ApplierOperation{&op2}};

    std::ignore = applyGroupedInsertsSteadyState(ops);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
