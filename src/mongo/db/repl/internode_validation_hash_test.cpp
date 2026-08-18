// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/internode_validation_hash_utils.h"
#include "mongo/db/repl/internode_validation_metrics.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/hex.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo {
namespace repl {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

// The log emitted for a mismatch when 'continuousInternodeValidationFatalOnMismatch' is disabled.
constexpr int32_t kMismatchLogId = 12882800;
// The 'fieldLevelDiff' logged when the two documents the diff is taken over are identical.
constexpr std::string_view kEmptyFieldLevelDiff = "{}";
// The 'fieldLevelDiff' logged for deletes, where no diff can be derived.
constexpr std::string_view kNoFieldLevelDiff = "<not derivable>";

/**
 * Returns the hash-mismatch counter that mismatches of 'opType' feed.
 */
otel::metrics::MetricName mismatchCounterName(OpTypeEnum opType) {
    switch (opType) {
        case OpTypeEnum::kInsert:
            return MetricNames::kInternodeConsistencyHashMismatchInsert;
        case OpTypeEnum::kUpdate:
            return MetricNames::kInternodeConsistencyHashMismatchUpdate;
        case OpTypeEnum::kDelete:
            return MetricNames::kInternodeConsistencyHashMismatchDelete;
        default:
            MONGO_UNREACHABLE;
    }
}

TEST(InternodeValidationMetricsTest, MismatchCountersStartAtZero) {
    OtelMetricsCapturer capturer;
    if (!OtelMetricsCapturer::canReadMetrics()) {
        return;
    }

    for (const auto& name : {MetricNames::kInternodeConsistencyHashMismatchInsert,
                             MetricNames::kInternodeConsistencyHashMismatchUpdate,
                             MetricNames::kInternodeConsistencyHashMismatchDelete}) {
        EXPECT_EQ(capturer.readInt64Counter(name), 0);
    }
}

TEST(InternodeValidationMetricsTest, MismatchCountersIncrementPerOpType) {
    OtelMetricsCapturer capturer;
    if (!OtelMetricsCapturer::canReadMetrics()) {
        return;
    }

    incrementDocumentHashMismatchCount(OpTypeEnum::kInsert);
    incrementDocumentHashMismatchCount(OpTypeEnum::kInsert);
    incrementDocumentHashMismatchCount(OpTypeEnum::kUpdate);
    incrementDocumentHashMismatchCount(OpTypeEnum::kDelete);

    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kInternodeConsistencyHashMismatchInsert), 2);
    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kInternodeConsistencyHashMismatchUpdate), 1);
    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kInternodeConsistencyHashMismatchDelete), 1);
}

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

    /**
     * Asserts that '_nss' holds exactly 'expected' at 'rid'.
     */
    void assertDocumentIs(const RecordId& rid, const BSONObj& expected) {
        ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
        ASSERT_BSONOBJ_EQ(expected, *documentAtRecordId(_opCtx.get(), _nss, rid));
    }

    /**
     * Asserts that '_nss' holds no document at 'rid'.
     */
    void assertNoDocumentAt(const RecordId& rid) {
        EXPECT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    }

    /**
     * Returns 'hash' with a bit flipped, standing in for a hash this node disagrees with.
     */
    static int64_t corrupt(int64_t hash) {
        return hash ^ 0x1;
    }

    /**
     * One non-fatal mismatch log line, as expected by assertOnlyLogsMismatches().
     */
    struct ExpectedMismatch {
        int64_t expectedHash;
        int64_t actualHash;
        std::string_view opType;
        std::string fieldLevelDiff;
    };

    /**
     * The mismatch expected from an insert of 'doc' whose hash was corrupted.
     */
    static ExpectedMismatch insertMismatch(const BSONObj& doc) {
        const int64_t actualHash = computeDocValidationHash(doc);
        return {corrupt(actualHash), actualHash, "i", std::string{kEmptyFieldLevelDiff}};
    }

    /**
     * The mismatch expected from a delete of 'doc' whose hash was corrupted.
     */
    static ExpectedMismatch deleteMismatch(const BSONObj& doc) {
        const int64_t actualHash = computeDocValidationHash(doc);
        return {corrupt(actualHash), actualHash, "d", std::string{kNoFieldLevelDiff}};
    }

    /**
     * The mismatch expected from an update whose hash was corrupted. 'preImage' is what this node
     * held, which is what the logged field-level diff is taken against.
     */
    static ExpectedMismatch updateMismatch(const BSONObj& preImage, const BSONObj& postImage) {
        const int64_t actualHash = computeUpdateValidationHash(preImage, postImage);
        return {corrupt(actualHash),
                actualHash,
                "u",
                doc_diff::computeInlineDiff(preImage, postImage)->toString()};
    }

    /**
     * Returns the hash-mismatch counter for 'opType'.
     */
    static int64_t readMismatchCounter(OtelMetricsCapturer& capturer, OpTypeEnum opType) {
        return capturer.readInt64Counter(mismatchCounterName(opType));
    }

    /**
     * Asserts that the per-op-type hash-mismatch counters account for exactly the mismatches in
     * 'expected'.
     */
    static void assertMismatchCounters(OtelMetricsCapturer& capturer,
                                       const std::vector<ExpectedMismatch>& expected) {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            return;
        }

        for (const auto opType : {OpTypeEnum::kInsert, OpTypeEnum::kUpdate, OpTypeEnum::kDelete}) {
            const std::string_view serializedOpType = idl::serialize(opType);
            const int64_t mismatchesOfOpType =
                std::count_if(expected.begin(), expected.end(), [&](const ExpectedMismatch& m) {
                    return m.opType == serializedOpType;
                });
            EXPECT_EQ(readMismatchCounter(capturer, opType), mismatchesOfOpType)
                << "opType: " << serializedOpType;
        }
    }

    /**
     * Applies 'applyOps' with log capture active and asserts that it succeeded having reported
     * exactly one mismatch log line per entry of 'expected', and no others. Also asserts that the
     * per-op-type hash-mismatch counters agree with 'expected'.
     */
    template <typename ApplyFn>
    void assertOnlyLogsMismatches(ApplyFn&& applyOps,
                                  const std::vector<ExpectedMismatch>& expected) {
        // Constructing the capturer resets the counters, so it has to come before 'applyOps'.
        OtelMetricsCapturer capturer;
        unittest::LogCaptureGuard logs;
        ASSERT_OK(applyOps());
        logs.stop();

        for (const auto& mismatch : expected) {
            EXPECT_EQ(logs.countBSONContainingSubset(
                          BSON("id" << kMismatchLogId << "attr"
                                    << BSON("expectedHash" << mismatch.expectedHash << "actualHash"
                                                           << mismatch.actualHash << "opType"
                                                           << mismatch.opType << "fieldLevelDiff"
                                                           << mismatch.fieldLevelDiff))),
                      1)
                << "opType: " << mismatch.opType << ", expectedHash: " << mismatch.expectedHash;
        }
        EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << kMismatchLogId)), expected.size());

        assertMismatchCounters(capturer, expected);
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

// A hash this node agrees with leaves the mismatch counters alone.
TEST_F(VerifyValidationHashTest, MatchingHashLeavesMismatchCountersAtZero) {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);

    OplogEntry op = makeInsertOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, _uuid, doc, rid, computeDocValidationHash(doc));

    OtelMetricsCapturer capturer;
    ASSERT_OK(runOpSteadyState(op));

    assertMismatchCounters(capturer, {});
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

// Per-collection validation needs its own feature flag. The fixture already enables the
// per-document one.
TEST_F(VerifyValidationHashTest, PerCollectionDisabledWithoutItsOwnFeatureFlag) {
    EXPECT_FALSE(isContinuousInternodeValidationPerCollectionEnabled(_opCtx.get()));

    unittest::ServerParameterGuard enablePerCollection(
        "featureFlagContinuousInternodeValidationPerCollection", true);
    EXPECT_TRUE(isContinuousInternodeValidationPerCollectionEnabled(_opCtx.get()));
}

// The collection hash is accumulated from the per-document hashes, so it is off whenever
// per-document validation is off, whatever its own flag says.
TEST_F(VerifyValidationHashTest, PerCollectionDisabledWithoutPerDocument) {
    unittest::ServerParameterGuard enablePerCollection(
        "featureFlagContinuousInternodeValidationPerCollection", true);
    unittest::ServerParameterGuard disablePerDocument(
        "featureFlagContinuousInternodeValidationPerDocument", false);

    EXPECT_FALSE(isContinuousInternodeValidationPerCollectionEnabled(_opCtx.get()));
}

template <typename Base>
class FatalOnMismatchTest : public Base {
protected:
    unittest::ServerParameterGuard _fatalOnMismatch{"continuousInternodeValidationFatalOnMismatch",
                                                    true};
};

template <typename Base>
class LogOnlyMismatchTest : public Base {
protected:
    unittest::ServerParameterGuard _fatalOnMismatch{"continuousInternodeValidationFatalOnMismatch",
                                                    false};
};

using VerifyValidationHashDeathTest = FatalOnMismatchTest<VerifyValidationHashTest>;
using VerifyValidationHashLogOnlyTest = LogOnlyMismatchTest<VerifyValidationHashTest>;

DEATH_TEST_F(VerifyValidationHashDeathTest, MismatchedHashFasserts, "12851600") {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    const int64_t wrongHash = computeDocValidationHash(doc) ^ 0x1;

    OplogEntry op =
        makeInsertOplogEntryWithRecordIdAndHash(nextOpTime(), _nss, _uuid, doc, rid, wrongHash);
    std::ignore = runOpSteadyState(op);
}

TEST_F(VerifyValidationHashTest, FatalOnMismatchDefaultsToEnabled) {
    EXPECT_TRUE(continuousInternodeValidationFatalOnMismatch.load());
}

// With 'continuousInternodeValidationFatalOnMismatch' disabled the mismatch is only logged, and
// oplog application proceeds.
TEST_F(VerifyValidationHashLogOnlyTest, InsertMismatchedHashOnlyLogs) {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);

    OplogEntry op = makeInsertOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, _uuid, doc, rid, corrupt(computeDocValidationHash(doc)));

    assertOnlyLogsMismatches([&] { return runOpSteadyState(op); }, {insertMismatch(doc)});

    assertDocumentIs(rid, doc);
}

// A mismatch on a delete is logged and the document is still removed.
TEST_F(VerifyValidationHashLogOnlyTest, DeleteMismatchedHashOnlyLogs) {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    OplogEntry op = makeDeleteOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, _uuid, BSON("_id" << 1), rid, corrupt(computeDocValidationHash(doc)));

    assertOnlyLogsMismatches([&] { return runOpSteadyState(op); }, {deleteMismatch(doc)});

    assertNoDocumentAt(rid);
}

// Each op type bumps only its own mismatch counter.
TEST_F(VerifyValidationHashLogOnlyTest, InsertMismatchIncrementsOnlyInsertCounter) {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);

    OplogEntry op = makeInsertOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, _uuid, doc, rid, corrupt(computeDocValidationHash(doc)));

    OtelMetricsCapturer capturer;
    if (!OtelMetricsCapturer::canReadMetrics()) {
        return;
    }
    ASSERT_OK(runOpSteadyState(op));

    EXPECT_EQ(readMismatchCounter(capturer, OpTypeEnum::kInsert), 1);
    EXPECT_EQ(readMismatchCounter(capturer, OpTypeEnum::kUpdate), 0);
    EXPECT_EQ(readMismatchCounter(capturer, OpTypeEnum::kDelete), 0);
}

TEST_F(VerifyValidationHashLogOnlyTest, UpdateMismatchIncrementsOnlyUpdateCounter) {
    const RecordId rid(1);
    const BSONObj preImage = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage, rid);

    const BSONObj postImage = BSON("_id" << 1 << "x" << 200);
    OplogEntry op = makeUpdateOplogEntryWithRecordIdAndHash(
        nextOpTime(),
        _nss,
        BSON("_id" << 1),
        postImage,
        rid,
        corrupt(computeUpdateValidationHash(preImage, postImage)));

    OtelMetricsCapturer capturer;
    if (!OtelMetricsCapturer::canReadMetrics()) {
        return;
    }
    ASSERT_OK(runOpSteadyState(op));

    EXPECT_EQ(readMismatchCounter(capturer, OpTypeEnum::kUpdate), 1);
    EXPECT_EQ(readMismatchCounter(capturer, OpTypeEnum::kInsert), 0);
    EXPECT_EQ(readMismatchCounter(capturer, OpTypeEnum::kDelete), 0);
}

TEST_F(VerifyValidationHashLogOnlyTest, DeleteMismatchIncrementsOnlyDeleteCounter) {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    OplogEntry op = makeDeleteOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, _uuid, BSON("_id" << 1), rid, corrupt(computeDocValidationHash(doc)));

    OtelMetricsCapturer capturer;
    if (!OtelMetricsCapturer::canReadMetrics()) {
        return;
    }
    ASSERT_OK(runOpSteadyState(op));

    EXPECT_EQ(readMismatchCounter(capturer, OpTypeEnum::kDelete), 1);
    EXPECT_EQ(readMismatchCounter(capturer, OpTypeEnum::kInsert), 0);
    EXPECT_EQ(readMismatchCounter(capturer, OpTypeEnum::kUpdate), 0);
}

// Repeated mismatches on the same op type accumulate.
TEST_F(VerifyValidationHashLogOnlyTest, RepeatedInsertMismatchesAccumulate) {
    OtelMetricsCapturer capturer;
    if (!OtelMetricsCapturer::canReadMetrics()) {
        return;
    }

    for (int i = 1; i <= 3; ++i) {
        const RecordId rid(i);
        const BSONObj doc = BSON("_id" << i << "x" << 100);
        OplogEntry op = makeInsertOplogEntryWithRecordIdAndHash(
            nextOpTime(), _nss, _uuid, doc, rid, corrupt(computeDocValidationHash(doc)));
        ASSERT_OK(runOpSteadyState(op));

        EXPECT_EQ(readMismatchCounter(capturer, OpTypeEnum::kInsert), i);
        EXPECT_EQ(readMismatchCounter(capturer, OpTypeEnum::kUpdate), 0);
        EXPECT_EQ(readMismatchCounter(capturer, OpTypeEnum::kDelete), 0);
    }
}

TEST_F(VerifyValidationHashTest, InsertWithNestedArraysMatchingHashAppliesCleanly) {
    const RecordId rid(1);
    const BSONObj doc =
        BSON("_id" << 1 << "tags" << BSON_ARRAY("a" << "b") << "items"
                   << BSON_ARRAY(BSON("sku" << 1 << "qty" << 5)
                                 << BSON("sku" << 2 << "qty" << BSON_ARRAY(1 << 2))));
    const int64_t hash = computeDocValidationHash(doc);

    OplogEntry op =
        makeInsertOplogEntryWithRecordIdAndHash(nextOpTime(), _nss, _uuid, doc, rid, hash);
    ASSERT_OK(runOpSteadyState(op));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    ASSERT_BSONOBJ_EQ(doc, *documentAtRecordId(_opCtx.get(), _nss, rid));
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

// Every mismatch in a grouped insert is reported, not just the first one, and the batch still
// commits.
TEST_F(VerifyValidationHashLogOnlyTest, GroupedInsertsMismatchOnEveryEntryOnlyLogs) {
    const RecordId rid1(1);
    const RecordId rid2(2);
    const BSONObj doc1 = BSON("_id" << 1 << "x" << 100);
    const BSONObj doc2 = BSON("_id" << 2 << "x" << 200);

    OplogEntry op1 = makeInsertOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, _uuid, doc1, rid1, corrupt(computeDocValidationHash(doc1)));
    OplogEntry op2 = makeInsertOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, _uuid, doc2, rid2, corrupt(computeDocValidationHash(doc2)));
    std::vector<ApplierOperation> ops = {ApplierOperation{&op1}, ApplierOperation{&op2}};

    assertOnlyLogsMismatches([&] { return applyGroupedInsertsSteadyState(ops); },
                             {insertMismatch(doc1), insertMismatch(doc2)});

    assertDocumentIs(rid1, doc1);
    assertDocumentIs(rid2, doc2);
}

TEST_F(VerifyValidationHashTest, DeleteMatchingHashAppliesCleanly) {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    const int64_t hash = computeDocValidationHash(doc);
    OplogEntry op = makeDeleteOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, _uuid, BSON("_id" << 1), rid, hash);
    ASSERT_OK(runOpSteadyState(op));

    EXPECT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
}

DEATH_TEST_F(VerifyValidationHashDeathTest, DeleteMismatchedHashFasserts, "12851600") {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    const int64_t wrongHash = computeDocValidationHash(doc) ^ 0x1;
    OplogEntry op = makeDeleteOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, _uuid, BSON("_id" << 1), rid, wrongHash);
    std::ignore = runOpSteadyState(op);
}

// Builds the $v:2 delta update that the primary would have logged for the given pre-image ->
// post-image transition.
BSONObj makeDelta(const BSONObj& preImage, const BSONObj& postImage) {
    return update_oplog_entry::makeDeltaOplogEntry(
        doc_diff::computeOplogDiff_forTest(preImage, postImage));
}

TEST_F(VerifyValidationHashTest, UpdateMatchingHashAppliesCleanly) {
    const RecordId rid(1);
    const BSONObj preImage = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage, rid);

    const BSONObj query = BSON("_id" << 1);
    const BSONObj postImage = BSON("_id" << 1 << "x" << 200);
    const int64_t hash = computeUpdateValidationHash(preImage, postImage);

    OplogEntry op =
        makeUpdateOplogEntryWithRecordIdAndHash(nextOpTime(), _nss, query, postImage, rid, hash);
    ASSERT_OK(runOpSteadyState(op));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    ASSERT_BSONOBJ_EQ(postImage, *documentAtRecordId(_opCtx.get(), _nss, rid));
}

// The update hash covers both images, so two updates that converge on the same post-image from
// different pre-images should hash differently.
TEST_F(VerifyValidationHashTest, UpdateHashDependsOnPreImage) {
    const BSONObj postImage = BSON("_id" << 1 << "x" << 200);
    const BSONObj preImage1 = BSON("_id" << 1 << "x" << 100);
    const BSONObj preImage2 = BSON("_id" << 1 << "x" << 150);

    EXPECT_NE(computeUpdateValidationHash(preImage1, postImage),
              computeUpdateValidationHash(preImage2, postImage));
    EXPECT_NE(computeUpdateValidationHash(preImage1, postImage),
              computeDocValidationHash(postImage));
}

DEATH_TEST_F(VerifyValidationHashDeathTest, UpdateMismatchedHashFasserts, "12851600") {
    const RecordId rid(1);
    const BSONObj preImage = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage, rid);

    const BSONObj query = BSON("_id" << 1);
    const BSONObj postImage = BSON("_id" << 1 << "x" << 200);
    const int64_t wrongHash = computeUpdateValidationHash(preImage, postImage) ^ 0x1;

    OplogEntry op = makeUpdateOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, query, postImage, rid, wrongHash);
    std::ignore = runOpSteadyState(op);
}

// A mismatch on an update is logged, and the post-image is still applied. The logged field-level
// diff is taken between the pre-image and what this node stored.
TEST_F(VerifyValidationHashLogOnlyTest, UpdateMismatchedHashOnlyLogs) {
    const RecordId rid(1);
    const BSONObj preImage = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage, rid);

    const BSONObj postImage = BSON("_id" << 1 << "x" << 200);
    OplogEntry op = makeUpdateOplogEntryWithRecordIdAndHash(
        nextOpTime(),
        _nss,
        BSON("_id" << 1),
        postImage,
        rid,
        corrupt(computeUpdateValidationHash(preImage, postImage)));

    assertOnlyLogsMismatches([&] { return runOpSteadyState(op); },
                             {updateMismatch(preImage, postImage)});

    assertDocumentIs(rid, postImage);
}

// The log-only counterpart of 'UpdateWithDeltaDivergentPreImageFasserts'. The divergence is only
// visible through the hash, so the node records it and then converges on the primary's post-image,
// leaving no trace of the divergence on disk.
TEST_F(VerifyValidationHashLogOnlyTest, UpdateWithDeltaDivergentPreImageOnlyLogs) {
    const RecordId rid(1);
    const BSONObj localPreImage = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, localPreImage, rid);

    const BSONObj primaryPreImage = BSON("_id" << 1 << "x" << 150);
    const BSONObj sharedPostImage = BSON("_id" << 1 << "x" << 200);
    const int64_t primaryHash = computeUpdateValidationHash(primaryPreImage, sharedPostImage);

    OplogEntry op =
        makeUpdateOplogEntryWithRecordIdAndHash(nextOpTime(),
                                                _nss,
                                                BSON("_id" << 1),
                                                makeDelta(primaryPreImage, sharedPostImage),
                                                rid,
                                                primaryHash);

    // The primary's hash is not this node's hash with a bit flipped, so the expectation cannot come
    // from updateMismatch().
    assertOnlyLogsMismatches(
        [&] { return runOpSteadyState(op); },
        {{primaryHash,
          computeUpdateValidationHash(localPreImage, sharedPostImage),
          "u",
          doc_diff::computeInlineDiff(localPreImage, sharedPostImage)->toString()}});

    assertDocumentIs(rid, sharedPostImage);
}

TEST_F(VerifyValidationHashTest, UpdateWrongHashInInitialSyncModeIsIgnored) {
    const RecordId rid(1);
    const BSONObj preImage = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage, rid);

    const BSONObj postImage = BSON("_id" << 1 << "x" << 200);
    const int64_t wrongHash = computeUpdateValidationHash(preImage, postImage) ^ 0x1;

    OplogEntry op = makeUpdateOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, BSON("_id" << 1), postImage, rid, wrongHash);
    ASSERT_OK(runOpInitialSync(op));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    ASSERT_BSONOBJ_EQ(postImage, *documentAtRecordId(_opCtx.get(), _nss, rid));
}

// A $v:2 delta update, where the secondary derives the post-image itself, applies cleanly. Both
// images are the same size, so the update is applied through the in-place path with
// updateDocumentWithDamages().
TEST_F(VerifyValidationHashTest, UpdateWithDeltaMatchingHashAppliesCleanly) {
    const RecordId rid(1);
    const BSONObj preImage = BSON("_id" << 1 << "x" << 100 << "y" << 7);
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage, rid);

    const BSONObj query = BSON("_id" << 1);
    const BSONObj expectedPostImage = BSON("_id" << 1 << "x" << 200 << "y" << 7);
    const int64_t hash = computeUpdateValidationHash(preImage, expectedPostImage);

    OplogEntry op = makeUpdateOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, query, makeDelta(preImage, expectedPostImage), rid, hash);
    ASSERT_OK(runOpSteadyState(op));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    ASSERT_BSONOBJ_EQ(expectedPostImage, *documentAtRecordId(_opCtx.get(), _nss, rid));
}

// This test exercises the non-in-place path for updates. We check that a delta whose insert section
// adds a new field is verified correctly.
TEST_F(VerifyValidationHashTest, UpdateAddingFieldWithDeltaMatchingHashAppliesCleanly) {
    const RecordId rid(1);
    const BSONObj preImage = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage, rid);

    const BSONObj expectedPostImage = BSON("_id" << 1 << "x" << 100 << "added" << "new field");
    const int64_t hash = computeUpdateValidationHash(preImage, expectedPostImage);

    OplogEntry op = makeUpdateOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, BSON("_id" << 1), makeDelta(preImage, expectedPostImage), rid, hash);
    ASSERT_OK(runOpSteadyState(op));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    ASSERT_BSONOBJ_EQ(expectedPostImage, *documentAtRecordId(_opCtx.get(), _nss, rid));
}

// This test exercises the non-in-place path for updates. We check that a delta whose delete
// section removes a field is verified correctly.
TEST_F(VerifyValidationHashTest, UpdateRemovingFieldWithDeltaMatchingHashAppliesCleanly) {
    const RecordId rid(1);
    const BSONObj preImage = BSON("_id" << 1 << "x" << 100 << "removed" << "field");
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage, rid);

    const BSONObj expectedPostImage = BSON("_id" << 1 << "x" << 100);
    const int64_t hash = computeUpdateValidationHash(preImage, expectedPostImage);

    OplogEntry op = makeUpdateOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, BSON("_id" << 1), makeDelta(preImage, expectedPostImage), rid, hash);
    ASSERT_OK(runOpSteadyState(op));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    ASSERT_BSONOBJ_EQ(expectedPostImage, *documentAtRecordId(_opCtx.get(), _nss, rid));
}

// The divergence check on the non-in-place path. The two nodes' pre-images differ only in the field
// the delta overwrites, and the delta changes the document's size, so this converges on a shared
// post-image via updateDocument() rather than updateDocumentWithDamages().
DEATH_TEST_F(VerifyValidationHashDeathTest,
             UpdateGrowingDocumentDivergentPreImageFasserts,
             "12851600") {
    const RecordId rid(1);
    const BSONObj localPreImage = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, localPreImage, rid);

    const BSONObj primaryPreImage = BSON("_id" << 1 << "x" << 150);
    const BSONObj sharedPostImage = BSON("_id" << 1 << "x" << "grown into a string");
    ASSERT_GT(sharedPostImage.objsize(), localPreImage.objsize());
    const int64_t primaryHash = computeUpdateValidationHash(primaryPreImage, sharedPostImage);

    OplogEntry op =
        makeUpdateOplogEntryWithRecordIdAndHash(nextOpTime(),
                                                _nss,
                                                BSON("_id" << 1),
                                                makeDelta(primaryPreImage, sharedPostImage),
                                                rid,
                                                primaryHash);
    std::ignore = runOpSteadyState(op);
}

// This node's pre-image diverges from the primary's, but applying the delta yields the same
// post-image. The pre-image's contribution to the hash is the only thing that catches the
// divergence.
DEATH_TEST_F(VerifyValidationHashDeathTest, UpdateWithDeltaDivergentPreImageFasserts, "12851600") {
    const RecordId rid(1);
    // What this node has.
    const BSONObj localPreImage = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, localPreImage, rid);

    // What the primary had when it computed the hash. Setting x to 200 from either pre-image yields
    // the same post-image.
    const BSONObj primaryPreImage = BSON("_id" << 1 << "x" << 150);
    const BSONObj sharedPostImage = BSON("_id" << 1 << "x" << 200);

    const BSONObj query = BSON("_id" << 1);
    const int64_t primaryHash = computeUpdateValidationHash(primaryPreImage, sharedPostImage);

    OplogEntry op = makeUpdateOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, query, makeDelta(primaryPreImage, sharedPostImage), rid, primaryHash);
    std::ignore = runOpSteadyState(op);
}

// An update whose images contain arrays and nested subdocuments applies cleanly.
TEST_F(VerifyValidationHashTest, UpdateWithNestedArraysMatchingHashAppliesCleanly) {
    const RecordId rid(1);
    const BSONObj preImage =
        BSON("_id" << 1 << "tags" << BSON_ARRAY("a" << "b") << "items"
                   << BSON_ARRAY(BSON("sku" << 1 << "qty" << 5)
                                 << BSON("sku" << 2 << "qty" << BSON_ARRAY(1 << 2))));
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage, rid);

    const BSONObj postImage =
        BSON("_id" << 1 << "tags" << BSON_ARRAY("a" << "b" << "c") << "items"
                   << BSON_ARRAY(BSON("sku" << 1 << "qty" << 6)
                                 << BSON("sku" << 2 << "qty" << BSON_ARRAY(1 << 2 << 3))));
    const int64_t hash = computeUpdateValidationHash(preImage, postImage);

    OplogEntry op = makeUpdateOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, BSON("_id" << 1), postImage, rid, hash);
    ASSERT_OK(runOpSteadyState(op));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    ASSERT_BSONOBJ_EQ(postImage, *documentAtRecordId(_opCtx.get(), _nss, rid));
}

// A $v:2 delta that rewrites one element of an array in-place.
TEST_F(VerifyValidationHashTest, UpdateWithArrayElementDeltaMatchingHashAppliesCleanly) {
    const RecordId rid(1);
    const BSONObj preImage = BSON("_id" << 1 << "arr" << BSON_ARRAY(10 << 20 << 30));
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage, rid);

    const BSONObj expectedPostImage = BSON("_id" << 1 << "arr" << BSON_ARRAY(10 << 99 << 30));
    const int64_t hash = computeUpdateValidationHash(preImage, expectedPostImage);

    // Rewrites arr[1].
    OplogEntry op = makeUpdateOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, BSON("_id" << 1), makeDelta(preImage, expectedPostImage), rid, hash);
    ASSERT_OK(runOpSteadyState(op));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    ASSERT_BSONOBJ_EQ(expectedPostImage, *documentAtRecordId(_opCtx.get(), _nss, rid));
}

// A $v:2 delta that grows an array, which changes the document size and so cannot be applied
// in-place.
TEST_F(VerifyValidationHashTest, UpdateGrowingArrayWithDeltaMatchingHashAppliesCleanly) {
    const RecordId rid(1);
    const BSONObj preImage = BSON("_id" << 1 << "arr" << BSON_ARRAY(10 << 20));
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage, rid);

    const BSONObj expectedPostImage =
        BSON("_id" << 1 << "arr" << BSON_ARRAY(10 << 20 << "appended"));
    const int64_t hash = computeUpdateValidationHash(preImage, expectedPostImage);

    // Appends a third element.
    OplogEntry op = makeUpdateOplogEntryWithRecordIdAndHash(
        nextOpTime(), _nss, BSON("_id" << 1), makeDelta(preImage, expectedPostImage), rid, hash);
    ASSERT_OK(runOpSteadyState(op));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    ASSERT_BSONOBJ_EQ(expectedPostImage, *documentAtRecordId(_opCtx.get(), _nss, rid));
}

/**
 * Tests that drive CRUD ops through a transaction's applyOps entry rather than as standalone oplog
 * entries.
 */
class ApplyOpsValidationHashTest : public VerifyValidationHashTest {
protected:
    void setUp() override {
        VerifyValidationHashTest::setUp();
        createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
        _workerPool = makeReplWorkerPool();
        _lsid = makeLogicalSessionId(_opCtx.get());
        _oplogApplier = std::make_unique<OplogApplierImpl>(
            nullptr /* executor */,
            nullptr /* oplogBuffer */,
            &noopOplogApplierObserver,
            ReplicationCoordinator::get(_opCtx.get()),
            getConsistencyMarkers(),
            getStorageInterface(),
            OplogApplier::Options(OplogApplication::Mode::kSecondary, false),
            _workerPool.get());
    }

    /**
     * Builds an inner transaction operation carrying a recordId and a document validation hash.
     * 'object2' is the update's _id query and is left unset for inserts and deletes.
     */
    ReplOperation makeInnerOp(OpTypeEnum opType,
                              const BSONObj& object,
                              boost::optional<BSONObj> object2,
                              const RecordId& rid,
                              int64_t hash) {
        ReplOperation op;
        op.setOpType(opType);
        op.setNss(_nss);
        op.setUuid(_uuid);
        op.setObject(object);
        if (object2) {
            op.setObject2(*object2);
        }
        op.setRecordId(rid);

        SingleOpSizeMetadata sizeMetadata;
        sizeMetadata.setH(hash);
        op.setSizeMetadata(OplogEntrySizeMetadata{sizeMetadata});
        return op;
    }

    /**
     * The documents of the mixed-CRUD batch built by makeCorruptedMixedCrudOps().
     */
    struct MixedCrudDocs {
        RecordId updateRid{1};
        RecordId deleteRid{2};
        RecordId insertRid{3};
        BSONObj updatePreImage = BSON("_id" << 1 << "x" << 100);
        BSONObj updatePostImage = BSON("_id" << 1 << "x" << 150);
        BSONObj deleteDoc = BSON("_id" << 2 << "x" << 200);
        BSONObj insertDoc = BSON("_id" << 3 << "arr" << BSON_ARRAY(1 << 2));
    };

    /**
     * Seeds the update's pre-image and the delete's target, then returns an insert, an update and a
     * delete over 'docs', each carrying a hash this node disagrees with.
     */
    std::vector<ReplOperation> makeCorruptedMixedCrudOps(const MixedCrudDocs& docs) {
        insertDocumentAtRecordId(_opCtx.get(), _nss, docs.updatePreImage, docs.updateRid);
        insertDocumentAtRecordId(_opCtx.get(), _nss, docs.deleteDoc, docs.deleteRid);

        return {makeInnerOp(OpTypeEnum::kInsert,
                            docs.insertDoc,
                            boost::none,
                            docs.insertRid,
                            corrupt(computeDocValidationHash(docs.insertDoc))),
                makeInnerOp(OpTypeEnum::kUpdate,
                            docs.updatePostImage,
                            BSON("_id" << 1),
                            docs.updateRid,
                            corrupt(computeUpdateValidationHash(docs.updatePreImage,
                                                                docs.updatePostImage))),
                makeInnerOp(OpTypeEnum::kDelete,
                            BSON("_id" << 2),
                            boost::none,
                            docs.deleteRid,
                            corrupt(computeDocValidationHash(docs.deleteDoc)))};
    }

    /**
     * Asserts that every op of a makeCorruptedMixedCrudOps() batch landed on disk.
     */
    void assertMixedCrudOpsApplied(const MixedCrudDocs& docs) {
        assertDocumentIs(docs.updateRid, docs.updatePostImage);
        assertNoDocumentAt(docs.deleteRid);
        assertDocumentIs(docs.insertRid, docs.insertDoc);
    }

    /**
     * The mismatches expected from a makeCorruptedMixedCrudOps() batch.
     */
    std::vector<ExpectedMismatch> mixedCrudMismatches(const MixedCrudDocs& docs) {
        return {insertMismatch(docs.insertDoc),
                updateMismatch(docs.updatePreImage, docs.updatePostImage),
                deleteMismatch(docs.deleteDoc)};
    }

    OperationSessionInfo sessionInfo() const {
        OperationSessionInfo sessionInfo;
        sessionInfo.setSessionId(_lsid);
        sessionInfo.setTxnNumber(_txnNum);
        return sessionInfo;
    }

    /**
     * Applies 'entries' as one oplog batch in steady-state secondary mode. This routes through the
     * real applier.
     */
    Status applyBatch(std::vector<OplogEntry> entries) {
        getStorageInterface()->oplogDiskLocRegister(
            _opCtx.get(), entries.back().getTimestamp(), true);
        return _oplogApplier->applyOplogBatch(_opCtx.get(), std::move(entries)).getStatus();
    }

    std::unique_ptr<ThreadPool> _workerPool;
    std::unique_ptr<OplogApplierImpl> _oplogApplier;
    LogicalSessionId _lsid;
    TxnNumber _txnNum = 1;
};

/**
 * Tests that drive CRUD ops through a transaction's applyOps entry rather than as standalone oplog
 * entries.
 */
class TransactionValidationHashTest : public ApplyOpsValidationHashTest {
protected:
    /**
     * Applies 'ops' as one batch in steady-state secondary mode, wrapped in a single applyOps
     * entry. With 'multiOpType' unset, the entry is a committed unprepared transaction. Setting it
     * makes the entry a batched write (kApplyOpsAppliedSeparately makes ops applied independently
     * of each other, or kApplyOpsAppliedAtomically for an atomic batch).
     */
    Status runTransactionSteadyState(
        std::vector<ReplOperation> ops,
        boost::optional<MultiOplogEntryType> multiOpType = boost::none) {
        const boost::optional<OpTime> prevWriteOpTime =
            multiOpType ? boost::make_optional(OpTime()) : boost::none;
        return applyBatch({makeApplyOpsOplogEntry(nextOpTime(),
                                                  std::move(ops),
                                                  sessionInfo(),
                                                  Date_t::now(),
                                                  {StmtId(0)},
                                                  prevWriteOpTime,
                                                  multiOpType)});
    }
};

using TransactionValidationHashDeathTest = FatalOnMismatchTest<TransactionValidationHashTest>;
using TransactionValidationHashLogOnlyTest = LogOnlyMismatchTest<TransactionValidationHashTest>;

// An insert, an update and a delete batched into one transaction, each carrying its own hash.
TEST_F(TransactionValidationHashTest, TransactionMixedCrudMatchingHashesApplyCleanly) {
    const RecordId updateRid(1);
    const RecordId deleteRid(2);
    const RecordId insertRid(3);

    const BSONObj updatePreImage = BSON("_id" << 1 << "x" << 100);
    const BSONObj deleteDoc = BSON("_id" << 2 << "x" << 200);
    insertDocumentAtRecordId(_opCtx.get(), _nss, updatePreImage, updateRid);
    insertDocumentAtRecordId(_opCtx.get(), _nss, deleteDoc, deleteRid);

    const BSONObj updatePostImage = BSON("_id" << 1 << "x" << 150);
    const BSONObj insertDoc = BSON("_id" << 3 << "arr" << BSON_ARRAY(1 << 2));

    ASSERT_OK(runTransactionSteadyState(
        {makeInnerOp(OpTypeEnum::kInsert,
                     insertDoc,
                     boost::none,
                     insertRid,
                     computeDocValidationHash(insertDoc)),
         makeInnerOp(OpTypeEnum::kUpdate,
                     updatePostImage,
                     BSON("_id" << 1),
                     updateRid,
                     computeUpdateValidationHash(updatePreImage, updatePostImage)),
         makeInnerOp(OpTypeEnum::kDelete,
                     BSON("_id" << 2),
                     boost::none,
                     deleteRid,
                     computeDocValidationHash(deleteDoc))}));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, updateRid));
    ASSERT_BSONOBJ_EQ(updatePostImage, *documentAtRecordId(_opCtx.get(), _nss, updateRid));
    EXPECT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, deleteRid));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, insertRid));
    ASSERT_BSONOBJ_EQ(insertDoc, *documentAtRecordId(_opCtx.get(), _nss, insertRid));
}

DEATH_TEST_F(TransactionValidationHashDeathTest,
             TransactionUpdateMismatchedHashFasserts,
             "12851600") {
    const RecordId rid(1);
    const BSONObj preImage = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage, rid);

    const BSONObj postImage = BSON("_id" << 1 << "x" << 200);
    const int64_t wrongHash = computeUpdateValidationHash(preImage, postImage) ^ 0x1;
    std::ignore = runTransactionSteadyState(
        {makeInnerOp(OpTypeEnum::kUpdate, postImage, BSON("_id" << 1), rid, wrongHash)});
}

DEATH_TEST_F(TransactionValidationHashDeathTest,
             TransactionInsertMismatchedHashFasserts,
             "12851600") {
    const RecordId rid(1);
    const BSONObj insertDoc = BSON("_id" << 1 << "x" << 100);
    const int64_t wrongHash = computeDocValidationHash(insertDoc) ^ 0x1;

    std::ignore = runTransactionSteadyState(
        {makeInnerOp(OpTypeEnum::kInsert, insertDoc, boost::none, rid, wrongHash)});
}

DEATH_TEST_F(TransactionValidationHashDeathTest,
             TransactionDeleteMismatchedHashFasserts,
             "12851600") {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    const int64_t wrongHash = computeDocValidationHash(doc) ^ 0x1;
    std::ignore = runTransactionSteadyState(
        {makeInnerOp(OpTypeEnum::kDelete, BSON("_id" << 1), boost::none, rid, wrongHash)});
}

// A wrong hash on an inner op does not abort the transaction: each mismatch is reported separately
// and every op still commits.
TEST_F(TransactionValidationHashLogOnlyTest, TransactionMixedCrudMismatchedHashesOnlyLog) {
    const MixedCrudDocs docs;
    const std::vector<ReplOperation> ops = makeCorruptedMixedCrudOps(docs);

    assertOnlyLogsMismatches([&] { return runTransactionSteadyState(ops); },
                             mixedCrudMismatches(docs));

    assertMixedCrudOpsApplied(docs);
}

// A batched write tagged kApplyOpsAppliedSeparately. The inner ops keep their own hashes, so the
// check behaves exactly as it does for a transaction's applyOps entry.
TEST_F(TransactionValidationHashTest, BatchedWriteUpdatesMatchingHashesApplyCleanly) {
    const RecordId rid1(1);
    const RecordId rid2(2);
    const BSONObj preImage1 = BSON("_id" << 1 << "x" << 100);
    const BSONObj preImage2 = BSON("_id" << 2 << "x" << 300);
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage1, rid1);
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage2, rid2);

    const BSONObj postImage1 = BSON("_id" << 1 << "x" << 200);
    const BSONObj postImage2 = BSON("_id" << 2 << "x" << 400);

    ASSERT_OK(
        runTransactionSteadyState({makeInnerOp(OpTypeEnum::kUpdate,
                                               postImage1,
                                               BSON("_id" << 1),
                                               rid1,
                                               computeUpdateValidationHash(preImage1, postImage1)),
                                   makeInnerOp(OpTypeEnum::kUpdate,
                                               postImage2,
                                               BSON("_id" << 2),
                                               rid2,
                                               computeUpdateValidationHash(preImage2, postImage2))},
                                  MultiOplogEntryType::kApplyOpsAppliedSeparately));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid1));
    ASSERT_BSONOBJ_EQ(postImage1, *documentAtRecordId(_opCtx.get(), _nss, rid1));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid2));
    ASSERT_BSONOBJ_EQ(postImage2, *documentAtRecordId(_opCtx.get(), _nss, rid2));
}

// A kApplyOpsAppliedAtomically batch whose second op updates the document its first op inserted.
// The update's pre-image is therefore produced earlier within the same batch, so its hash is only
// correct if the ops are applied in order and each one sees the preceding one's write.
TEST_F(TransactionValidationHashTest, BatchedWriteUpdateOfEarlierInsertMatchingHashAppliesCleanly) {
    const RecordId rid(1);
    const BSONObj insertedDoc = BSON("_id" << 1 << "x" << 100);
    const BSONObj postImage = BSON("_id" << 1 << "x" << 200);

    ASSERT_OK(runTransactionSteadyState(
        {makeInnerOp(OpTypeEnum::kInsert,
                     insertedDoc,
                     boost::none,
                     rid,
                     computeDocValidationHash(insertedDoc)),
         makeInnerOp(OpTypeEnum::kUpdate,
                     postImage,
                     BSON("_id" << 1),
                     rid,
                     computeUpdateValidationHash(insertedDoc, postImage))},
        MultiOplogEntryType::kApplyOpsAppliedAtomically));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, rid));
    ASSERT_BSONOBJ_EQ(postImage, *documentAtRecordId(_opCtx.get(), _nss, rid));
}

DEATH_TEST_F(TransactionValidationHashDeathTest, BatchedWriteMismatchedHashFasserts, "12851600") {
    const RecordId rid(1);
    const BSONObj preImage = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage, rid);

    const BSONObj postImage = BSON("_id" << 1 << "x" << 200);
    const int64_t wrongHash = computeUpdateValidationHash(preImage, postImage) ^ 0x1;
    std::ignore = runTransactionSteadyState(
        {makeInnerOp(OpTypeEnum::kUpdate, postImage, BSON("_id" << 1), rid, wrongHash)},
        MultiOplogEntryType::kApplyOpsAppliedSeparately);
}

TEST_F(TransactionValidationHashTest, BatchedWriteInsertAndDeleteMatchingHashesApplyCleanly) {
    const RecordId deleteRid(1);
    const RecordId insertRid(2);

    const BSONObj deleteDoc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, deleteDoc, deleteRid);

    const BSONObj insertDoc = BSON("_id" << 2 << "x" << 200);

    ASSERT_OK(runTransactionSteadyState({makeInnerOp(OpTypeEnum::kInsert,
                                                     insertDoc,
                                                     boost::none,
                                                     insertRid,
                                                     computeDocValidationHash(insertDoc)),
                                         makeInnerOp(OpTypeEnum::kDelete,
                                                     BSON("_id" << 1),
                                                     boost::none,
                                                     deleteRid,
                                                     computeDocValidationHash(deleteDoc))},
                                        MultiOplogEntryType::kApplyOpsAppliedSeparately));

    EXPECT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, deleteRid));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, insertRid));
    ASSERT_BSONOBJ_EQ(insertDoc, *documentAtRecordId(_opCtx.get(), _nss, insertRid));
}

/**
 * For prepared transactions, each inner ops are applied when the prepare entry is applied. Each
 * inner op's hash is checked during the prepare batch.
 */
class PreparedTransactionValidationHashTest : public ApplyOpsValidationHashTest {
protected:
    /**
     * Applies 'ops' as a prepared transaction in steady-state secondary mode. The prepare and the
     * commit must be separate batches: the transaction has to be left in the prepared state before
     * the commit entry arrives, and it is the commit batch that applies the inner ops.
     */
    Status runPreparedTransactionSteadyState(std::vector<ReplOperation> ops) {
        const OplogEntry prepareEntry = makeApplyOpsOplogEntry(nextOpTime(),
                                                               std::move(ops),
                                                               sessionInfo(),
                                                               Date_t::now(),
                                                               {StmtId(0)},
                                                               OpTime(),
                                                               boost::none /* multiOpType */,
                                                               ApplyOpsType::kPrepare);
        const OplogEntry commitEntry = makeCommandOplogEntryWithSessionInfoAndStmtIds(
            nextOpTime(),
            _nss,
            BSON("commitTransaction" << 1 << "commitTimestamp" << prepareEntry.getTimestamp()),
            _lsid,
            _txnNum,
            {StmtId(1)},
            prepareEntry.getOpTime());

        const Status prepareStatus = applyBatch({prepareEntry});
        if (!prepareStatus.isOK()) {
            return prepareStatus;
        }
        return applyBatch({commitEntry});
    }
};

using PreparedTransactionValidationHashDeathTest =
    FatalOnMismatchTest<PreparedTransactionValidationHashTest>;
using PreparedTransactionValidationHashLogOnlyTest =
    LogOnlyMismatchTest<PreparedTransactionValidationHashTest>;

TEST_F(PreparedTransactionValidationHashTest,
       PreparedTransactionMixedCrudMatchingHashesApplyCleanly) {
    const RecordId updateRid(1);
    const RecordId deleteRid(2);
    const RecordId insertRid(3);

    const BSONObj updatePreImage = BSON("_id" << 1 << "x" << 100);
    const BSONObj deleteDoc = BSON("_id" << 2 << "x" << 200);
    insertDocumentAtRecordId(_opCtx.get(), _nss, updatePreImage, updateRid);
    insertDocumentAtRecordId(_opCtx.get(), _nss, deleteDoc, deleteRid);

    const BSONObj updatePostImage = BSON("_id" << 1 << "x" << 200);
    const BSONObj insertDoc = BSON("_id" << 3 << "y" << 5);

    ASSERT_OK(runPreparedTransactionSteadyState(
        {makeInnerOp(OpTypeEnum::kUpdate,
                     updatePostImage,
                     BSON("_id" << 1),
                     updateRid,
                     computeUpdateValidationHash(updatePreImage, updatePostImage)),
         makeInnerOp(OpTypeEnum::kInsert,
                     insertDoc,
                     boost::none,
                     insertRid,
                     computeDocValidationHash(insertDoc)),
         makeInnerOp(OpTypeEnum::kDelete,
                     BSON("_id" << 2),
                     boost::none,
                     deleteRid,
                     computeDocValidationHash(deleteDoc))}));

    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, updateRid));
    ASSERT_BSONOBJ_EQ(updatePostImage, *documentAtRecordId(_opCtx.get(), _nss, updateRid));
    EXPECT_FALSE(documentExistsAtRecordId(_opCtx.get(), _nss, deleteRid));
    ASSERT_TRUE(documentExistsAtRecordId(_opCtx.get(), _nss, insertRid));
    ASSERT_BSONOBJ_EQ(insertDoc, *documentAtRecordId(_opCtx.get(), _nss, insertRid));
}

// A wrong hash on an inner op should be fatal, rather than the check being skipped.
DEATH_TEST_F(PreparedTransactionValidationHashDeathTest,
             PreparedTransactionUpdateMismatchedHashFasserts,
             "12851600") {
    const RecordId rid(1);
    const BSONObj preImage = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, preImage, rid);

    const BSONObj postImage = BSON("_id" << 1 << "x" << 200);
    const int64_t wrongHash = computeUpdateValidationHash(preImage, postImage) ^ 0x1;
    std::ignore = runPreparedTransactionSteadyState(
        {makeInnerOp(OpTypeEnum::kUpdate, postImage, BSON("_id" << 1), rid, wrongHash)});
}

DEATH_TEST_F(PreparedTransactionValidationHashDeathTest,
             PreparedTransactionInsertMismatchedHashFasserts,
             "12851600") {
    const RecordId rid(1);
    const BSONObj insertDoc = BSON("_id" << 1 << "x" << 100);
    const int64_t wrongHash = computeDocValidationHash(insertDoc) ^ 0x1;

    std::ignore = runPreparedTransactionSteadyState(
        {makeInnerOp(OpTypeEnum::kInsert, insertDoc, boost::none, rid, wrongHash)});
}

DEATH_TEST_F(PreparedTransactionValidationHashDeathTest,
             PreparedTransactionDeleteMismatchedHashFasserts,
             "12851600") {
    const RecordId rid(1);
    const BSONObj doc = BSON("_id" << 1 << "x" << 100);
    insertDocumentAtRecordId(_opCtx.get(), _nss, doc, rid);

    const int64_t wrongHash = computeDocValidationHash(doc) ^ 0x1;
    std::ignore = runPreparedTransactionSteadyState(
        {makeInnerOp(OpTypeEnum::kDelete, BSON("_id" << 1), boost::none, rid, wrongHash)});
}

// A wrong hash on an inner op does not prevent the prepared transaction from committing.
TEST_F(PreparedTransactionValidationHashLogOnlyTest,
       PreparedTransactionMixedCrudMismatchedHashesOnlyLog) {
    const MixedCrudDocs docs;
    const std::vector<ReplOperation> ops = makeCorruptedMixedCrudOps(docs);

    assertOnlyLogsMismatches([&] { return runPreparedTransactionSteadyState(ops); },
                             mixedCrudMismatches(docs));

    assertMixedCrudOpsApplied(docs);
}

/**
 * Ensure the xxHash library does not change its output for known inputs, so that the document
 * validation hash is stable across versions. xxHash branches its implementation based on the input
 * size, so we test documents of different sizes to cover all the code paths.
 *
 */
class DocValidationHashKnownAnswerTest : public unittest::Test {
protected:
    /**
     * Asserts that 'doc' has exactly the expected on-disk bytes and hashes to the expected value.
     */
    void assertKnownHash(const BSONObj& doc, std::string_view expectedHex, int64_t expectedHash) {
        EXPECT_EQ(hexblob::encodeLower(std::string_view(doc.objdata(), doc.objsize())),
                  expectedHex);
        EXPECT_EQ(computeDocValidationHash(doc), expectedHash);
    }
};

// Sized to land in xxHash's 0-to-16-byte path.
TEST_F(DocValidationHashKnownAnswerTest, TinyDoc) {
    assertKnownHash(BSON("a" << 1), "0c0000001061000100000000", 8182952154619646941LL);
}

// Sized to land in xxHash's 17-to-128-byte path.
TEST_F(DocValidationHashKnownAnswerTest, SmallDoc) {
    assertKnownHash(BSON("_id" << 1 << "x" << 100 << "s"
                               << "abc"),
                    "20000000105f6964000100000010780064000000027300040000006162630000",
                    -4811079693095661071LL);
}

// Sized to land in xxHash's 129-to-240-byte path.
TEST_F(DocValidationHashKnownAnswerTest, MediumDoc) {
    const BSONObj doc = BSON("_id" << 5 << "pad" << std::string(150, 'z'));
    EXPECT_EQ(doc.objsize(), 174);
    EXPECT_EQ(computeDocValidationHash(doc), 3310436893767801287LL);
}

// Sized to land in xxHash's over-240-byte path.
TEST_F(DocValidationHashKnownAnswerTest, LargeDoc) {
    const BSONObj doc = BSON("_id" << 4 << "pad" << std::string(2576, 'm'));
    EXPECT_EQ(doc.objsize(), 2600);
    EXPECT_EQ(computeDocValidationHash(doc), 7043491338923751776LL);
}

TEST_F(DocValidationHashKnownAnswerTest, UpdateHashIsXorOfDocHashes) {
    const BSONObj preImage = BSON("_id" << 1 << "x" << 100);
    const BSONObj postImage = BSON("_id" << 1 << "x" << 200);
    EXPECT_EQ(computeUpdateValidationHash(preImage, postImage),
              computeDocValidationHash(preImage) ^ computeDocValidationHash(postImage));

    // XOR-ing is symmetric.
    EXPECT_EQ(computeUpdateValidationHash(preImage, postImage),
              computeUpdateValidationHash(postImage, preImage));
    EXPECT_EQ(computeUpdateValidationHash(preImage, preImage), 0);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
