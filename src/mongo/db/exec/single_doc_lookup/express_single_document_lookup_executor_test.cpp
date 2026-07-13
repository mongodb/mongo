// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/single_doc_lookup/express_single_document_lookup_executor.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/single_doc_lookup/collection_acquirer.h"
#include "mongo/db/exec/single_doc_lookup/mock_local_lookup_eligibility.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats_test_util.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/router_role/routing_cache/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo::exec::agg {
namespace {
using namespace std::literals::string_view_literals;

using LookupResult = SingleDocumentLookupExecutor::LookupResult;
using otel::metrics::OtelMetricsCapturer;

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("test", "express_lookup");
const NamespaceString kShardedTestNss =
    NamespaceString::createNamespaceString_forTest("test", "express_lookup_sharded");

/**
 * Test acquirer whose acquireCollection() runs an injected callback that throws. Lets the exception
 * matrix be driven without a sharded fixture.
 */
class ThrowingCollectionAcquirer final : public CollectionAcquirer {
public:
    explicit ThrowingCollectionAcquirer(std::function<void()> thrower)
        : _thrower(std::move(thrower)) {}

    Handle acquireCollection(OperationContext*,
                             const NamespaceString&,
                             boost::optional<UUID>) override {
        _thrower();
        MONGO_UNREACHABLE;
    }

private:
    std::function<void()> _thrower;
};

class ExpressLookupTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), kTestNss, CollectionOptions()));
    }

    boost::intrusive_ptr<ExpressionContext> makeExpCtx() {
        return make_intrusive<ExpressionContextForTest>(operationContext(), kTestNss);
    }

    void insert(BSONObj doc) {
        std::vector<BSONObj> docs{doc};
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), kTestNss, AcquisitionPrerequisites::OperationType::kWrite),
            MODE_IX);
        WriteUnitOfWork wuow{operationContext()};
        ASSERT_OK(Helpers::insert(operationContext(), coll.getCollectionPtr(), docs));
        wuow.commit();
    }

    // Express executor over the real collection (OnDemand acquirer) with the given eligibility.
    ExpressSingleDocumentLookupExecutor makeExecutor(
        std::unique_ptr<LocalLookupEligibility> eligibility) {
        return ExpressSingleDocumentLookupExecutor(std::make_unique<OnDemandCollectionAcquirer>(),
                                                   std::move(eligibility));
    }

    // Same as makeExecutor(), but wired to the real process-global express cell so metrics tests
    // can observe recording. Other tests use makeExecutor()'s boost::none default so they don't
    // have an incidental side effect on the process-global counters.
    ExpressSingleDocumentLookupExecutor makeExecutorWithRealRecorder(
        std::unique_ptr<LocalLookupEligibility> eligibility) {
        return ExpressSingleDocumentLookupExecutor(
            std::make_unique<OnDemandCollectionAcquirer>(),
            std::move(eligibility),
            exec::SingleDocumentLookupStatsRecorder::makeUpdateLookupExpressRecorder());
    }

    LookupResult lookup(ExpressSingleDocumentLookupExecutor& exec, const Document& documentKey) {
        return exec.performLookup(
            makeExpCtx(), kTestNss, /*collectionUUID*/ boost::none, documentKey, boost::none);
    }
};

using ExpressLookupDeathTest = ExpressLookupTest;

// --- Constructor invariants -------------------------------------------------------------------

DEATH_TEST_REGEX_F(ExpressLookupDeathTest, TassertsOnNullAcquirer, "Tripwire assertion.*12841300") {
    ExpressSingleDocumentLookupExecutor(nullptr, MockLocalLookupEligibility::makeAlwaysLocal());
}

DEATH_TEST_REGEX_F(ExpressLookupDeathTest,
                   TassertsOnNullEligibility,
                   "Tripwire assertion.*12841301") {
    ExpressSingleDocumentLookupExecutor(std::make_unique<OnDemandCollectionAcquirer>(), nullptr);
}

// --- Eligibility gating (C1.6 / C3.1) ---------------------------------------------------------

TEST_F(ExpressLookupTest, UnknownEligibilityReturnsNotHandled) {
    insert(BSON("_id" << 1));
    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysUnknown());
    auto result = lookup(exec, Document{{"_id", 1}});
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kNotHandled);
}

// --- Happy paths (C1.1 / C1.3 / C3.2) ---------------------------------------------------------

// An existing _id is found and the full document returned. This fixture's collection is unsharded,
// so this also covers the shard-filter no-op: Express always requests the acquisition's shard
// filter, but an unsharded acquisition has none to apply (getShardingFilter() would tassert), so
// the document is returned unfiltered. Orphan dropping on a truly sharded collection is exercised
// end-to-end by the $_internalSearchIdLookup orphan tests with the feature flag on.
TEST_F(ExpressLookupTest, FindsExistingDocument) {
    insert(BSON("_id" << 1 << "value" << "hello"));
    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysLocal());
    auto result = lookup(exec, Document{{"_id", 1}});

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_TRUE(result.document.has_value());
    ASSERT_BSONOBJ_EQ(result.document->toBson(), BSON("_id" << 1 << "value" << "hello"));
}

TEST_F(ExpressLookupTest, AbsentDocumentReturnsHandledNotFound) {
    insert(BSON("_id" << 1));
    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysLocal());
    auto result = lookup(exec, Document{{"_id", 404}});
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);
}

// --- _id-only seek behaviour (C1.7 / C3.5) ----------------------------------------------------

TEST_F(ExpressLookupTest, DocumentKeyWithoutIdReturnsNotHandled) {
    insert(BSON("_id" << 1 << "sk" << 5));
    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysLocal());
    auto result = lookup(exec, Document{{"sk", 5}});
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kNotHandled);
}

TEST_F(ExpressLookupTest, MultiFieldDocumentKeyMatchesById) {
    insert(BSON("_id" << 1 << "sk" << 5));
    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysLocal());
    // Sharded-shape documentKey {_id, sk}: must be served locally by matching _id alone.
    auto result = lookup(exec, Document{{"_id", 1}, {"sk", 5}});

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), BSON("_id" << 1 << "sk" << 5));
}

TEST_F(ExpressLookupTest, IdOnlyLookupIgnoresStaleShardKeyInDocumentKey) {
    insert(BSON("_id" << 1 << "sk" << 1));
    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysLocal());
    // documentKey carries a stale shard-key value; the _id-only seek returns the on-disk document.
    auto result = lookup(exec, Document{{"_id", 1}, {"sk", 999}});

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), BSON("_id" << 1 << "sk" << 1));
}

// --- Missing collection -----------------------------------------------------------------------

TEST_F(ExpressLookupTest, MissingCollectionReturnsHandledNotFound) {
    auto otherNss = NamespaceString::createNamespaceString_forTest("test", "does_not_exist");
    ExpressSingleDocumentLookupExecutor exec(std::make_unique<OnDemandCollectionAcquirer>(),
                                             MockLocalLookupEligibility::makeAlwaysLocal());
    auto result =
        exec.performLookup(makeExpCtx(), otherNss, boost::none, Document{{"_id", 1}}, boost::none);
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);
}

// --- Exception matrix (C1.4 / C3.3) -----------------------------------------------------------

TEST_F(ExpressLookupTest, NamespaceNotFoundReturnsHandledNotFound) {
    ExpressSingleDocumentLookupExecutor exec(std::make_unique<ThrowingCollectionAcquirer>([] {
                                                 uasserted(ErrorCodes::NamespaceNotFound,
                                                           "ns gone");
                                             }),
                                             MockLocalLookupEligibility::makeAlwaysLocal());
    auto result = lookup(exec, Document{{"_id", 1}});
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);
}

TEST_F(ExpressLookupTest, ShardCannotRefreshDueToLocksHeldPropagates) {
    ExpressSingleDocumentLookupExecutor exec(
        std::make_unique<ThrowingCollectionAcquirer>(
            [] { uasserted(ShardCannotRefreshDueToLocksHeldInfo(kTestNss), "locks held"); }),
        MockLocalLookupEligibility::makeAlwaysLocal());
    ASSERT_THROWS_CODE(lookup(exec, Document{{"_id", 1}}),
                       DBException,
                       ErrorCodes::ShardCannotRefreshDueToLocksHeld);
}

TEST_F(ExpressLookupTest, UnhandledExceptionPropagates) {
    ExpressSingleDocumentLookupExecutor exec(std::make_unique<ThrowingCollectionAcquirer>([] {
                                                 uasserted(ErrorCodes::InternalError, "boom");
                                             }),
                                             MockLocalLookupEligibility::makeAlwaysLocal());
    ASSERT_THROWS_CODE(lookup(exec, Document{{"_id", 1}}), DBException, ErrorCodes::InternalError);
}

// CAR errors (StaleConfig etc.) propagate out of the body: the eligibility owns refresh + retry
// inside run(). The non-routing AlwaysLocal impl has no route() wrapper, so the error escapes
// performLookup; the routing-aware impl's refresh + retry is covered by
// ShardedEligibilityRouteTest.
TEST_F(ExpressLookupTest, StaleConfigPropagates) {
    ExpressSingleDocumentLookupExecutor exec(
        std::make_unique<ThrowingCollectionAcquirer>([] {
            uasserted(StaleConfigInfo(
                          kTestNss, ShardVersion::UNTRACKED(), boost::none, ShardId("shard0")),
                      "stale config");
        }),
        MockLocalLookupEligibility::makeAlwaysLocal());

    ASSERT_THROWS_CODE(lookup(exec, Document{{"_id", 1}}), DBException, ErrorCodes::StaleConfig);
}

// --- _id shapes and collation -----------------------------------------------------------------

TEST_F(ExpressLookupTest, ObjectValuedIdWithOperatorShapeMatchesByEquality) {
    // A DBRef-shaped _id: its first field name starts with '$'. The lookup must match it by
    // equality rather than misparsing it as a query operator.
    const BSONObj dbRefId = BSON("$ref" << "other" << "$id" << 1);
    insert(BSON("_id" << dbRefId));
    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysLocal());

    auto result = lookup(exec, Document{{"_id", dbRefId}});
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), BSON("_id" << dbRefId));
}

TEST_F(ExpressLookupTest, IdLookupUsesCollectionDefaultCollation) {
    // A collection whose _id index is encoded under a case-insensitive default collation. The
    // lookup must adopt that collation, or the _id seek computes simple-collation bounds and
    // misses.
    const auto ciNss = NamespaceString::createNamespaceString_forTest("test", "ci_collation");
    CollectionOptions options;
    options.collation = BSON("locale" << "en" << "strength" << 2);
    ASSERT_OK(storageInterface()->createCollection(operationContext(), ciNss, options));
    {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), ciNss, AcquisitionPrerequisites::OperationType::kWrite),
            MODE_IX);
        WriteUnitOfWork wuow{operationContext()};
        std::vector<BSONObj> docs{BSON("_id" << "abc")};
        ASSERT_OK(Helpers::insert(operationContext(), coll.getCollectionPtr(), docs));
        wuow.commit();
    }

    ExpressSingleDocumentLookupExecutor exec(std::make_unique<OnDemandCollectionAcquirer>(),
                                             MockLocalLookupEligibility::makeAlwaysLocal());
    // Same _id value with different case; under the collection's case-insensitive collation it must
    // still resolve to the stored document.
    auto result =
        exec.performLookup(make_intrusive<ExpressionContextForTest>(operationContext(), ciNss),
                           ciNss,
                           boost::none,
                           Document{{"_id", "ABC"sv}},
                           boost::none);
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), BSON("_id" << "abc"));
}

// --- Metrics recording ------------------------------------------------------------------------

TEST_F(ExpressLookupTest, FoundDocumentRecordsFoundAndLatency) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    insert(BSON("_id" << 1 << "value" << "hello"));
    auto exec = makeExecutorWithRealRecorder(MockLocalLookupEligibility::makeAlwaysLocal());

    const auto before = snapshotExpressCell(capturer);
    auto result = lookup(exec, Document{{"_id", 1}});
    const auto after = snapshotExpressCell(capturer);

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_EQ(after.found, before.found + 1);
    ASSERT_EQ(after.notFound, before.notFound);
    ASSERT_EQ(after.notHandled, before.notHandled);
    ASSERT_EQ(after.latencyCount, before.latencyCount + 1);
}

TEST_F(ExpressLookupTest, AbsentDocumentRecordsNotFound) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    insert(BSON("_id" << 1));
    auto exec = makeExecutorWithRealRecorder(MockLocalLookupEligibility::makeAlwaysLocal());

    const auto before = snapshotExpressCell(capturer);
    auto result = lookup(exec, Document{{"_id", 404}});
    const auto after = snapshotExpressCell(capturer);

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);
    ASSERT_EQ(after.found, before.found);
    ASSERT_EQ(after.notFound, before.notFound + 1);
    ASSERT_EQ(after.notHandled, before.notHandled);
    ASSERT_EQ(after.latencyCount, before.latencyCount + 1);
}

TEST_F(ExpressLookupTest, UnknownDecisionRecordsNotHandledWithNoLatency) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    insert(BSON("_id" << 1));
    auto exec = makeExecutorWithRealRecorder(MockLocalLookupEligibility::makeAlwaysUnknown());

    const auto before = snapshotExpressCell(capturer);
    auto result = lookup(exec, Document{{"_id", 1}});
    const auto after = snapshotExpressCell(capturer);

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kNotHandled);
    ASSERT_EQ(after.found, before.found);
    ASSERT_EQ(after.notFound, before.notFound);
    ASSERT_EQ(after.notHandled, before.notHandled + 1);
    // A declined lookup carries no meaningful latency.
    ASSERT_EQ(after.latencyCount, before.latencyCount);
}

// --- Shard filtering -----------------------------------------------------------------------
//
// Exercises the acquisition's real shard filter against a physically-present orphan, rather than
// asserting on an eligibility's checksShardKeyOwnership() flag in isolation: proves the executor
// actually applies (or skips) shard filtering, not just that the flag it reads is set correctly.
// Mirrored by the behavioral tests in sbe_single_document_lookup_executor_test.cpp's
// SbeSingleDocumentLookupExecutorShardFilteringTest, which additionally inspects the compiled plan
// for the shard-filter stage (not possible here: Express has no plan tree to introspect).

class ExpressLookupShardFilteringTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();
        _client = std::make_unique<DBDirectClient>(operationContext());
        _client->createCollection(kShardedTestNss);
    }

    boost::intrusive_ptr<ExpressionContext> makeExpCtx() {
        return make_intrusive<ExpressionContextForTest>(operationContext(), kShardedTestNss);
    }

    // Shard key "sk", split at 'splitPoint': documents with sk < splitPoint are owned by this
    // shard, sk >= splitPoint are orphans physically present here but owned by "otherShard".
    CollectionMetadata setupShardingMetadata(int splitPoint) {
        OperationContext* opCtx = operationContext();
        const UUID uuid = [&] {
            AutoGetCollection autoColl(opCtx, kShardedTestNss, MODE_IS);
            return autoColl->uuid();
        }();

        const ShardKeyPattern shardKeyPattern(BSON("sk" << 1));
        const KeyPattern keyPattern = shardKeyPattern.getKeyPattern();
        const OID epoch = OID::gen();
        const Timestamp timestamp(1, 1);
        ChunkVersion version({epoch, timestamp}, {1, 0});

        ChunkType ownedChunk(uuid,
                             ChunkRange{keyPattern.globalMin(), BSON("sk" << splitPoint)},
                             version,
                             kMyShardName);
        version.incMinor();
        ChunkType orphanChunk(uuid,
                              ChunkRange{BSON("sk" << splitPoint), keyPattern.globalMax()},
                              version,
                              ShardId("otherShard"));

        auto rt = RoutingTableHistory::makeNew(kShardedTestNss,
                                               uuid,
                                               keyPattern,
                                               false /* unsplittable */,
                                               nullptr,
                                               false,
                                               epoch,
                                               timestamp,
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               true /* allowMigrations */,
                                               {ownedChunk, orphanChunk});
        CurrentChunkManager cm(makeStandaloneRoutingTableHistory(std::move(rt)));
        CollectionMetadata metadata(std::move(cm), kMyShardName);

        auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, kShardedTestNss);
        scopedCsr->setCollectionMetadata(opCtx, metadata);
        return metadata;
    }

    ExpressSingleDocumentLookupExecutor makeExecutor(
        std::unique_ptr<LocalLookupEligibility> eligibility) {
        return ExpressSingleDocumentLookupExecutor(std::make_unique<OnDemandCollectionAcquirer>(),
                                                   std::move(eligibility));
    }

    LookupResult lookup(ExpressSingleDocumentLookupExecutor& exec, const Document& documentKey) {
        return exec.performLookup(
            makeExpCtx(), kShardedTestNss, boost::none, documentKey, boost::none);
    }

    std::unique_ptr<DBDirectClient> _client;
};

// AlwaysLocal-shaped eligibility (checksShardKeyOwnership() == false, see
// AlwaysLocalDoesNotCheckShardKeyOwnership in local_lookup_eligibility_test.cpp): the executor must
// apply the acquisition's shard filter and drop a physically-present orphan. Mirrors
// SbeSingleDocumentLookupExecutorShardFilteringTest's
// EligibilityWithoutOwnershipCheckAppliesShardFilterAndDropsOrphan.
TEST_F(ExpressLookupShardFilteringTest,
       EligibilityWithoutOwnershipCheckAppliesShardFilterAndDropsOrphan) {
    auto metadata = setupShardingMetadata(/*splitPoint*/ 10);
    _client->insert(kShardedTestNss, BSON("_id" << 1 << "sk" << 99));  // orphan: sk >= 10

    ScopedSetShardRole scopedSetShardRole{
        operationContext(), kShardedTestNss, ShardVersionFactory::make(metadata), boost::none};

    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysLocal());
    auto result = lookup(exec, Document{{"_id", 1}});
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);
}

// Same eligibility shape, but the looked-up document is owned: the shard filter is applied but has
// nothing to drop, so the document is still returned. SBE's analog,
// EligibilityWithoutOwnershipCheckIncludesShardFilterStage, additionally asserts the compiled plan
// includes the shard-filter stage (Express has no plan tree to introspect).
TEST_F(ExpressLookupShardFilteringTest,
       EligibilityWithoutOwnershipCheckReturnsOwnedDocumentUnfiltered) {
    auto metadata = setupShardingMetadata(/*splitPoint*/ 10);
    _client->insert(kShardedTestNss, BSON("_id" << 1 << "sk" << 1));  // owned: sk < 10

    ScopedSetShardRole scopedSetShardRole{
        operationContext(), kShardedTestNss, ShardVersionFactory::make(metadata), boost::none};

    auto exec = makeExecutor(MockLocalLookupEligibility::makeAlwaysLocal());
    auto result = lookup(exec, Document{{"_id", 1}});
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
}

// An eligibility whose Local decision already resolved ownership from the shard key: the executor
// must skip the redundant shard filter, so even a physically-present orphan is returned. Mirrors
// SbeSingleDocumentLookupExecutorShardFilteringTest's
// EligibilityThatChecksOwnershipSkipsShardFilterAndReturnsOrphan.
TEST_F(ExpressLookupShardFilteringTest,
       EligibilityThatChecksOwnershipSkipsShardFilterAndReturnsOrphan) {
    auto metadata = setupShardingMetadata(/*splitPoint*/ 10);
    _client->insert(kShardedTestNss, BSON("_id" << 1 << "sk" << 99));  // orphan: sk >= 10

    ScopedSetShardRole scopedSetShardRole{
        operationContext(), kShardedTestNss, ShardVersionFactory::make(metadata), boost::none};

    auto eligibility = MockLocalLookupEligibility::makeAlwaysLocal();
    eligibility->setChecksShardKeyOwnership(true);
    auto exec = makeExecutor(std::move(eligibility));
    auto result = lookup(exec, Document{{"_id", 1}});
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
}

}  // namespace
}  // namespace mongo::exec::agg
