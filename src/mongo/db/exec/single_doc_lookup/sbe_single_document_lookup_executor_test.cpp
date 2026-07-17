// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/single_doc_lookup/sbe_single_document_lookup_executor.h"

#include "mongo/bson/json.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/single_doc_lookup/collection_acquirer.h"
#include "mongo/db/exec/single_doc_lookup/mock_local_lookup_eligibility.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats_test_util.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/shard_role_transaction_resources_stasher_for_pipeline.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace std::literals::string_view_literals;

namespace mongo::exec::agg {
namespace {

using LookupResult = SingleDocumentLookupExecutor::LookupResult;
using otel::metrics::OtelMetricsCapturer;

const NamespaceString kNss =
    NamespaceString::createNamespaceString_forTest("SbeSingleDocumentLookupExecutorTest.testColl");
const NamespaceString kShardedNss = NamespaceString::createNamespaceString_forTest(
    "SbeSingleDocumentLookupExecutorTest.shardedColl");

// Walks an SBE plan stats tree and collects every stage's stageType string in pre-order.
void collectStageTypes(const sbe::PlanStageStats* stats, std::vector<std::string>& out) {
    out.emplace_back(std::string{stats->common.stageType});
    for (const auto& child : stats->children) {
        collectStageTypes(child.get(), out);
    }
}

std::vector<std::string> planStageTypes(const sbe::PlanStage* root) {
    auto stats = root->getStats(/*includeDebugInfo=*/true);
    std::vector<std::string> out;
    collectStageTypes(stats.get(), out);
    return out;
}

std::string joinStageTypes(const std::vector<std::string>& types) {
    std::string s;
    for (size_t i = 0; i < types.size(); ++i) {
        if (i) {
            s += ",";
        }
        s += types[i];
    }
    return s;
}

bool contains(const std::vector<std::string>& v, std::string_view needle) {
    return std::find(v.begin(), v.end(), needle) != v.end();
}

class SbeSingleDocumentLookupExecutorTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _expCtx = make_intrusive<ExpressionContextForTest>(operationContext(), kNss);
    }

    void createCollection() {
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions()));
    }

    void createClusteredCollection() {
        CollectionOptions opts;
        opts.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
        ASSERT_OK(storageInterface()->createCollection(operationContext(), kNss, opts));
    }

    void createCollectionWithCaseInsensitiveCollation() {
        CollectionOptions opts;
        opts.collation = fromjson("{locale: 'en', strength: 2}");
        ASSERT_OK(storageInterface()->createCollection(operationContext(), kNss, opts));
    }

    void createClusteredCollectionWithCaseInsensitiveCollation() {
        CollectionOptions opts;
        opts.collation = fromjson("{locale: 'en', strength: 2}");
        opts.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
        ASSERT_OK(storageInterface()->createCollection(operationContext(), kNss, opts));
    }

    void insertDocuments(std::vector<BSONObj> docs) {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), kNss, AcquisitionPrerequisites::OperationType::kWrite),
            MODE_IX);
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(Helpers::insert(operationContext(), coll.getCollectionPtr(), docs));
        wuow.commit();
    }

    LookupResult lookup(SingleDocumentLookupExecutor* strategy,
                        const BSONObj& documentKey,
                        boost::optional<UUID> uuid = boost::none) {
        return strategy->performLookup(
            _expCtx, kNss, uuid, Document(documentKey), boost::optional<Timestamp>());
    }

    UUID currentUuid() {
        auto uuid =
            CollectionCatalog::get(operationContext())->lookupUUIDByNSS(operationContext(), kNss);
        ASSERT_TRUE(uuid);
        return *uuid;
    }

    const Collection* getCollection() {
        return CollectionCatalog::get(operationContext())
            ->lookupCollectionByNamespace(operationContext(), kNss);
    }

    long long idIndexAccesses() {
        const auto& stats = CollectionIndexUsageTrackerDecoration::getUsageStats(getCollection());
        auto it = stats.find("_id_");
        return it == stats.end() ? 0 : it->second->accesses.load();
    }

    long long collectionScans() {
        return CollectionIndexUsageTrackerDecoration::getCollectionScanStats(getCollection())
            .collectionScans;
    }

    // storageInterface()->dropCollection() leaves 'dropOpTime' null and expects the registered
    // OpObserver to allocate one by actually writing an oplog entry, which the test OpObserver
    // does not do. UnreplicatedWritesBlock makes the drop take the "oplog disabled for this
    // write" path instead (see ReplicationCoordinator::isOplogDisabledFor), which needs no optime.
    void dropCollection() {
        repl::UnreplicatedWritesBlock noRepl(operationContext());
        ASSERT_OK(storageInterface()->dropCollection(operationContext(), kNss));
    }

    // Simulates a secondary index build's effect on plan-cache validity: takes a MODE_X lock (the
    // same acquisition a real DDL/index op would need) and bumps the collection's
    // PlanCacheInvalidatorVersion without otherwise changing the collection's identity (UUID).
    void bumpPlanCacheInvalidatorVersion() {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), kNss, AcquisitionPrerequisites::OperationType::kWrite),
            MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer(operationContext(), &coll);
        auto* writableColl = writer.getWritableCollection(operationContext());
        CollectionQueryInfo::get(writableColl).rebuildIndexData(operationContext(), writableColl);
        wuow.commit();
    }

    std::unique_ptr<CollectionAcquirer> makeOnDemandAcquirer() {
        return std::make_unique<OnDemandCollectionAcquirer>();
    }

    SbeSingleDocumentLookupExecutor makeStrategy() {
        return SbeSingleDocumentLookupExecutor(makeOnDemandAcquirer(),
                                               std::make_unique<AlwaysLocalEligibility>());
    }

    // Same as makeStrategy(), but wired to the real process-global SBE cell so metrics tests can
    // observe recording, and parameterized over eligibility so the not-handled path can be driven
    // too.
    SbeSingleDocumentLookupExecutor makeStrategyWithRealRecorder(
        std::unique_ptr<LocalLookupEligibility> eligibility) {
        return SbeSingleDocumentLookupExecutor(
            makeOnDemandAcquirer(),
            std::move(eligibility),
            exec::SingleDocumentLookupStatsRecorder::makeUpdateLookupSbeRecorder());
    }

    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

// --- Functional lookups -----------------------------------------------------------------------

TEST_F(SbeSingleDocumentLookupExecutorTest, FindDocumentById) {
    createCollection();
    insertDocuments({fromjson("{_id: 1, x: 'hello'}"), fromjson("{_id: 2, x: 'world'}")});

    auto strategy = makeStrategy();
    auto result = lookup(&strategy, fromjson("{_id: 1}"));

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_TRUE(result.document.has_value());
    ASSERT_BSONOBJ_EQ(result.document->toBson(), fromjson("{_id: 1, x: 'hello'}"));
}

TEST_F(SbeSingleDocumentLookupExecutorTest, NotFoundForMissingDocument) {
    createCollection();
    insertDocuments({fromjson("{_id: 1}")});

    auto strategy = makeStrategy();
    auto result = lookup(&strategy, fromjson("{_id: 999}"));

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);
    ASSERT_FALSE(result.document.has_value());
}

TEST_F(SbeSingleDocumentLookupExecutorTest, NotFoundForMissingCollection) {
    auto strategy = makeStrategy();
    auto result = lookup(&strategy, fromjson("{_id: 1}"));

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);
}

TEST_F(SbeSingleDocumentLookupExecutorTest, NotFoundForEmptyCollection) {
    createCollection();

    auto strategy = makeStrategy();
    auto result = lookup(&strategy, fromjson("{_id: 1}"));

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);
}

TEST_F(SbeSingleDocumentLookupExecutorTest, CachedPlanReuse) {
    createCollection();
    insertDocuments({fromjson("{_id: 1, val: 'a'}"),
                     fromjson("{_id: 2, val: 'b'}"),
                     fromjson("{_id: 3, val: 'c'}")});

    auto strategy = makeStrategy();

    auto r1 = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(r1.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(r1.document->toBson(), fromjson("{_id: 1, val: 'a'}"));

    auto r2 = lookup(&strategy, fromjson("{_id: 2}"));
    ASSERT_EQ(r2.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(r2.document->toBson(), fromjson("{_id: 2, val: 'b'}"));

    auto r3 = lookup(&strategy, fromjson("{_id: 999}"));
    ASSERT_EQ(r3.status, LookupResult::HandledStatus::kDocumentNotFound);

    auto r4 = lookup(&strategy, fromjson("{_id: 3}"));
    ASSERT_EQ(r4.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(r4.document->toBson(), fromjson("{_id: 3, val: 'c'}"));
}

// Stress test for cached-executor reuse: many lookups by different _ids against the same strategy
// instance, with revisits, misses, and out-of-order keys. Asserts every lookup returns the correct
// document AND that the SBE plan tree pointer is identical across every call (the planner ran once
// and rebinding does the work on subsequent calls). This is the property the strategy exists for.
TEST_F(SbeSingleDocumentLookupExecutorTest, RepeatedLookupsReuseExecutorAndReturnCorrectDocs) {
    createCollection();
    // A non-trivial collection size so a stale-bound bug or a fallback to COLLSCAN would be
    // observable (a 3-doc test cannot distinguish a working IXSCAN from a working scan).
    constexpr int kNumDocs = 100;
    std::vector<BSONObj> docs;
    docs.reserve(kNumDocs);
    for (int i = 1; i <= kNumDocs; ++i) {
        docs.push_back(BSON("_id" << i << "val" << i * 10));
    }
    insertDocuments(std::move(docs));

    auto strategy = makeStrategy();

    auto r0 = lookup(&strategy, fromjson("{_id: 42}"));
    ASSERT_EQ(r0.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(r0.document->toBson(), BSON("_id" << 42 << "val" << 420));
    const auto* originalPlanRoot = strategy.getCachedPlanRoot_forTest();
    ASSERT_TRUE(originalPlanRoot);

    const std::vector<int> keys{
        1,
        kNumDocs,
        50,
        7,
        99,
        2,
        50,
        7,
        25,
        25,
        kNumDocs,
        1,
        88,
        13,
        88,
    };
    for (int k : keys) {
        auto r = lookup(&strategy, BSON("_id" << k));
        ASSERT_EQ(r.status, LookupResult::HandledStatus::kDocumentFound) << "key=" << k;
        ASSERT_BSONOBJ_EQ(r.document->toBson(), BSON("_id" << k << "val" << k * 10));
        // Executor identity: same plan root means PreparedExecutor::make ran exactly once and
        // rebind handled the rest. If this fires the strategy is rebuilding the SBE tree per call,
        // defeating the caching.
        ASSERT_EQ(strategy.getCachedPlanRoot_forTest(), originalPlanRoot) << "key=" << k;
    }

    auto miss = lookup(&strategy, fromjson("{_id: 9999}"));
    ASSERT_EQ(miss.status, LookupResult::HandledStatus::kDocumentNotFound);
    ASSERT_EQ(strategy.getCachedPlanRoot_forTest(), originalPlanRoot);

    auto afterMiss = lookup(&strategy, fromjson("{_id: 17}"));
    ASSERT_EQ(afterMiss.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(afterMiss.document->toBson(), BSON("_id" << 17 << "val" << 170));
    ASSERT_EQ(strategy.getCachedPlanRoot_forTest(), originalPlanRoot);

    for (int k = 1; k <= kNumDocs; ++k) {
        auto r = lookup(&strategy, BSON("_id" << k));
        ASSERT_EQ(r.status, LookupResult::HandledStatus::kDocumentFound) << "key=" << k;
        ASSERT_BSONOBJ_EQ(r.document->toBson(), BSON("_id" << k << "val" << k * 10));
    }
    ASSERT_EQ(strategy.getCachedPlanRoot_forTest(), originalPlanRoot);
}

// Same stress test against a clustered collection, where the planner produces a bounded scan rather
// than an IXSCAN on _id_. The rebind path for clustered collections writes RecordId bounds slots;
// this guards that pathway against stale bounds, executor rebuild, and miss-then-hit corruption.
TEST_F(SbeSingleDocumentLookupExecutorTest, RepeatedLookupsOnClusteredCollectionReuseExecutor) {
    createClusteredCollection();
    constexpr int kNumDocs = 100;
    std::vector<BSONObj> docs;
    docs.reserve(kNumDocs);
    for (int i = 1; i <= kNumDocs; ++i) {
        docs.push_back(BSON("_id" << i << "payload" << i + 1000));
    }
    insertDocuments(std::move(docs));

    auto strategy = makeStrategy();

    auto r0 = lookup(&strategy, fromjson("{_id: 42}"));
    ASSERT_EQ(r0.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(r0.document->toBson(), BSON("_id" << 42 << "payload" << 1042));
    const auto* originalPlanRoot = strategy.getCachedPlanRoot_forTest();
    ASSERT_TRUE(originalPlanRoot);

    const std::vector<int> keys{
        1,
        kNumDocs,
        50,
        7,
        99,
        2,
        50,
        7,
        25,
        25,
        kNumDocs,
        1,
        88,
        13,
        88,
    };
    for (int k : keys) {
        auto r = lookup(&strategy, BSON("_id" << k));
        ASSERT_EQ(r.status, LookupResult::HandledStatus::kDocumentFound) << "key=" << k;
        ASSERT_BSONOBJ_EQ(r.document->toBson(), BSON("_id" << k << "payload" << k + 1000));
        ASSERT_EQ(strategy.getCachedPlanRoot_forTest(), originalPlanRoot) << "key=" << k;
    }
}

// Clustered collection with a compound BSON _id, mimicking config.system.preimages's shape
// (ChangeStreamPreImageId: nsUUID + ts + applyOpsIndex). Each document carries a unique 'payload'
// so a body-truncation or stale-cached-doc bug on reuse would be observable.
TEST_F(SbeSingleDocumentLookupExecutorTest, ClusteredCompoundIdLookupsReturnFullDocument) {
    createClusteredCollection();

    struct Entry {
        BSONObj id;
        BSONObj fullDoc;
    };
    std::vector<Entry> entries;
    auto uuidA = UUID::gen();
    auto uuidB = UUID::gen();
    auto makeId = [&](const UUID& nsUUID, int ts, int applyIdx) {
        return BSON("nsUUID" << nsUUID.toBSON().firstElement() << "ts" << Timestamp(ts, 1)
                             << "applyOpsIndex" << applyIdx);
    };
    auto makeDoc = [&](const BSONObj& id, std::string_view payload) {
        return BSON("_id" << id << "payload" << payload << "extra" << BSON_ARRAY(1 << 2 << 3)
                          << "size" << static_cast<int>(payload.size()));
    };

    for (int i = 0; i < 4; ++i) {
        auto id = makeId(uuidA, 100 + i, 0);
        entries.push_back({id.getOwned(), makeDoc(id, std::string(8 + i * 5, 'a')).getOwned()});
    }
    for (int i = 0; i < 4; ++i) {
        auto id = makeId(uuidB, 200 + i, i);
        entries.push_back({id.getOwned(), makeDoc(id, std::string(12 + i * 4, 'b')).getOwned()});
    }

    std::vector<BSONObj> docs;
    docs.reserve(entries.size());
    for (const auto& e : entries) {
        docs.push_back(e.fullDoc);
    }
    insertDocuments(std::move(docs));

    auto strategy = makeStrategy();

    auto first = lookup(&strategy, BSON("_id" << entries[0].id));
    ASSERT_EQ(first.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(first.document->toBson(), entries[0].fullDoc);

    const auto* originalPlanRoot = strategy.getCachedPlanRoot_forTest();
    ASSERT_TRUE(originalPlanRoot);

    for (size_t i = 1; i < entries.size(); ++i) {
        auto r = lookup(&strategy, BSON("_id" << entries[i].id));
        ASSERT_EQ(r.status, LookupResult::HandledStatus::kDocumentFound)
            << "lookup #" << i << "; _id=" << entries[i].id.toString();
        ASSERT_BSONOBJ_EQ(r.document->toBson(), entries[i].fullDoc);
        ASSERT_EQ(strategy.getCachedPlanRoot_forTest(), originalPlanRoot)
            << "Plan must be reused across lookups, not rebuilt";
    }

    auto revisit = lookup(&strategy, BSON("_id" << entries[0].id));
    ASSERT_EQ(revisit.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(revisit.document->toBson(), entries[0].fullDoc);
}

// --- Plan shape -------------------------------------------------------------------------------

TEST_F(SbeSingleDocumentLookupExecutorTest, MultiFieldDocumentKeyHandledViaIxscanNoFilter) {
    // Sharded-style document key {_id, sk}: the strategy strips everything except _id before
    // handing the filter to the planner. The produced plan is a single-leaf _id IXSCAN with no
    // residual filter.
    createCollection();
    insertDocuments(
        {fromjson("{_id: 1, sk: 'a', data: 100}"), fromjson("{_id: 2, sk: 'b', data: 200}")});

    auto strategy = makeStrategy();
    auto result = lookup(&strategy, fromjson("{_id: 1, sk: 'a'}"));

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), fromjson("{_id: 1, sk: 'a', data: 100}"));

    const auto* root = strategy.getCachedPlanRoot_forTest();
    ASSERT_TRUE(root);
    auto types = planStageTypes(root);
    ASSERT_TRUE(contains(types, "ixseek") || contains(types, "ixscan"))
        << "Expected an SBE index scan stage, got: " << joinStageTypes(types);
    ASSERT_FALSE(contains(types, "filter"))
        << "Expected no residual filter (strategy strips shard-key fields), got: "
        << joinStageTypes(types);
}

// On a regular (non-clustered) collection an _id-equality lookup must produce a plan with an index
// scan stage, never a bare record-store scan (the parallel-poc COLLSCAN regression).
TEST_F(SbeSingleDocumentLookupExecutorTest, RegularCollectionUsesIndexScan) {
    createCollection();
    insertDocuments(
        {fromjson("{_id: 1, x: 'a'}"), fromjson("{_id: 2, x: 'b'}"), fromjson("{_id: 3, x: 'c'}")});

    auto strategy = makeStrategy();
    auto result = lookup(&strategy, fromjson("{_id: 2}"));
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), fromjson("{_id: 2, x: 'b'}"));

    const auto* root = strategy.getCachedPlanRoot_forTest();
    ASSERT_TRUE(root) << "Expected the SBE strategy to have built a cached plan";
    auto types = planStageTypes(root);

    ASSERT_TRUE(contains(types, "ixseek") || contains(types, "ixscan"))
        << "Expected an SBE index scan stage, got plan: " << joinStageTypes(types);
    ASSERT_FALSE(contains(types, "scan"))
        << "Plan must not contain a record-store scan on a non-clustered collection: "
        << joinStageTypes(types);
}

// On a clustered collection the planner emits a bounded STAGE_COLLSCAN. SBE compiles that to a
// "scan" stage that reads only the bounded range. Asserted indirectly via numReads.
TEST_F(SbeSingleDocumentLookupExecutorTest, ClusteredCollectionUsesBoundedScan) {
    createClusteredCollection();
    std::vector<BSONObj> docs;
    for (int i = 1; i <= 50; ++i) {
        docs.push_back(BSON("_id" << i << "x" << i));
    }
    insertDocuments(std::move(docs));

    auto strategy = makeStrategy();
    auto result = lookup(&strategy, fromjson("{_id: 25}"));
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), BSON("_id" << 25 << "x" << 25));

    const auto* root = strategy.getCachedPlanRoot_forTest();
    ASSERT_TRUE(root);

    auto stats = root->getStats(/*includeDebugInfo=*/true);
    std::function<const sbe::PlanStageStats*(const sbe::PlanStageStats*)> findScan;
    findScan = [&](const sbe::PlanStageStats* s) -> const sbe::PlanStageStats* {
        if (s->common.stageType == "scan"sv) {
            return s;
        }
        for (const auto& child : s->children) {
            if (auto found = findScan(child.get())) {
                return found;
            }
        }
        return nullptr;
    };
    const auto* scanStats = findScan(stats.get());
    ASSERT_TRUE(scanStats) << "Expected a scan stage in the clustered-collection plan";

    const auto* specific = dynamic_cast<const sbe::ScanStats*>(scanStats->specific.get());
    ASSERT_TRUE(specific) << "Expected ScanStats on the scan stage";
    ASSERT_LT(specific->numReads, 10u)
        << "Clustered _id lookup must be bounded; got numReads=" << specific->numReads;
}

// --- $indexStats recording ---------------------------------------------------------------------

TEST_F(SbeSingleDocumentLookupExecutorTest, IxscanLookupRecordsIdIndexUsage) {
    createCollection();
    insertDocuments({fromjson("{_id: 1, x: 'a'}")});

    auto strategy = makeStrategy();
    ASSERT_EQ(idIndexAccesses(), 0);

    auto result = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_EQ(idIndexAccesses(), 1);
    // An _id ixscan is not a collection scan.
    ASSERT_EQ(collectionScans(), 0);
}

TEST_F(SbeSingleDocumentLookupExecutorTest, IxscanLookupRecordsUsageEvenWhenNotFound) {
    createCollection();
    insertDocuments({fromjson("{_id: 1, x: 'a'}")});

    auto strategy = makeStrategy();
    auto result = lookup(&strategy, fromjson("{_id: 999}"));
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);

    // $indexStats counts accesses, not just hits.
    ASSERT_EQ(idIndexAccesses(), 1);
}

TEST_F(SbeSingleDocumentLookupExecutorTest, RepeatedIxscanLookupsAccumulateIdIndexUsage) {
    createCollection();
    insertDocuments({fromjson("{_id: 1, x: 'a'}"), fromjson("{_id: 2, x: 'b'}")});

    auto strategy = makeStrategy();
    lookup(&strategy, fromjson("{_id: 1}"));
    lookup(&strategy, fromjson("{_id: 2}"));
    lookup(&strategy, fromjson("{_id: 999}"));

    ASSERT_EQ(idIndexAccesses(), 3);
}

// A clustered _id lookup runs as a bounded scan over the clustered collection, which has no _id_
// index registered in the usage tracker. Express reports neither an index nor a collection scan for
// this shape (EXPRESS_CLUSTERED_IXSCAN), so SBE must record nothing to stay consistent.
TEST_F(SbeSingleDocumentLookupExecutorTest, ClusteredLookupRecordsNothing) {
    createClusteredCollection();
    insertDocuments({fromjson("{_id: 1, x: 'a'}")});

    auto strategy = makeStrategy();
    const auto scansBefore = collectionScans();

    auto result = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);

    ASSERT_EQ(collectionScans(), scansBefore);
    ASSERT_EQ(idIndexAccesses(), 0);
}

// --- SlotBinder fast-path engagement --------------------------------------------------------

TEST_F(SbeSingleDocumentLookupExecutorTest, IdEqualityEngagesSlotBinderFastPath) {
    // Shape (1): non-clustered _id IXSCAN. One kIxscanKeyPair binder.
    createCollection();
    insertDocuments(
        {fromjson("{_id: 1, x: 1}"), fromjson("{_id: 2, x: 2}"), fromjson("{_id: 3, x: 3}")});

    auto strategy = makeStrategy();

    for (auto&& [i, expected] :
         std::vector<std::pair<int, BSONObj>>{{1, fromjson("{_id: 1, x: 1}")},
                                              {2, fromjson("{_id: 2, x: 2}")},
                                              {3, fromjson("{_id: 3, x: 3}")}}) {
        auto r = lookup(&strategy, BSON("_id" << i));
        ASSERT_EQ(r.status, LookupResult::HandledStatus::kDocumentFound);
        ASSERT_BSONOBJ_EQ(r.document->toBson(), expected);
    }
}

TEST_F(SbeSingleDocumentLookupExecutorTest, ClusteredScalarIdEngagesSlotBinder) {
    // Shape (2) scalar variant: clustered collection with integer _id. The bounded scan's min/max
    // RecordId slots are populated by a kClusteredRecordIdPair binder via keyForElem.
    createClusteredCollection();
    insertDocuments({fromjson("{_id: 1, x: 1}"), fromjson("{_id: 2, x: 2}")});

    auto strategy = makeStrategy();

    auto r1 = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(r1.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(r1.document->toBson(), fromjson("{_id: 1, x: 1}"));

    auto r2 = lookup(&strategy, fromjson("{_id: 2}"));
    ASSERT_EQ(r2.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(r2.document->toBson(), fromjson("{_id: 2, x: 2}"));
}

TEST_F(SbeSingleDocumentLookupExecutorTest, ClusteredCompoundIdEngagesSlotBinder) {
    // Shape (2) compound BSON _id variant (mimics config.system.preimages). keyForElem handles
    // object-valued _ids the same way it handles scalars.
    createClusteredCollection();
    auto uuidA = UUID::gen();
    auto id1 = BSON("nsUUID" << uuidA.toBSON().firstElement() << "ts" << Timestamp(100, 1)
                             << "applyOpsIndex" << 0);
    auto id2 = BSON("nsUUID" << uuidA.toBSON().firstElement() << "ts" << Timestamp(101, 1)
                             << "applyOpsIndex" << 0);
    insertDocuments(
        {BSON("_id" << id1 << "body" << "first"), BSON("_id" << id2 << "body" << "second")});

    auto strategy = makeStrategy();

    auto r1 = lookup(&strategy, BSON("_id" << id1));
    ASSERT_EQ(r1.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(r1.document->toBson(), BSON("_id" << id1 << "body" << "first"));

    auto r2 = lookup(&strategy, BSON("_id" << id2));
    ASSERT_EQ(r2.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(r2.document->toBson(), BSON("_id" << id2 << "body" << "second"));
}

// --- Collation ---------------------------------------------------------------------------------
//
// For the IXSCAN shape, the strategy adopts the collection's default collation onto the query (so
// the planner selects the collation-aware _id index) and SlotBinder encodes collation comparison
// keys, so it resolves the lookup via the _id index. The clustered shape still declines a
// non-simple collation: its RecordId encoding can't apply collation keys, so it falls back rather
// than compute wrong seek bounds.

// A non-simple default collation makes the _id index collation-aware. The fast path adopts that
// collation (so the planner selects the _id index) and encodes collation comparison keys for the
// seek, resolving the lookup via the _id index. A different-cased _id still matches, and it is
// recorded as _id index usage in $indexStats.
TEST_F(SbeSingleDocumentLookupExecutorTest, NonSimpleCollationEngagesIxscanFastPath) {
    createCollectionWithCaseInsensitiveCollation();
    insertDocuments({fromjson("{_id: 'abc', x: 1}")});

    auto strategy = makeStrategy();
    // Look up with a different case; the case-insensitive _id index must still resolve it.
    auto result = lookup(&strategy, fromjson("{_id: 'ABC'}"));

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), fromjson("{_id: 'abc', x: 1}"));
    ASSERT_TRUE(strategy.getCachedPlanRoot_forTest());
    ASSERT_EQ(idIndexAccesses(), 1);
    ASSERT_EQ(collectionScans(), 0);
}

TEST_F(SbeSingleDocumentLookupExecutorTest, NonSimpleCollationDeclinesClusteredFastPath) {
    createClusteredCollectionWithCaseInsensitiveCollation();
    insertDocuments({fromjson("{_id: 'abc', x: 1}")});

    auto strategy = makeStrategy();
    auto result = lookup(&strategy, fromjson("{_id: 'abc'}"));

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kNotHandled);
    ASSERT_FALSE(strategy.getCachedPlanRoot_forTest())
        << "A declined plan shape must not be cached";
}

// A simple (or absent) collation is unaffected: the fast path still engages for a string _id.
TEST_F(SbeSingleDocumentLookupExecutorTest, SimpleCollationStillEngagesIxscanFastPath) {
    createCollection();
    insertDocuments({fromjson("{_id: 'abc', x: 1}"), fromjson("{_id: 'ABC', x: 2}")});

    auto strategy = makeStrategy();
    auto result = lookup(&strategy, fromjson("{_id: 'abc'}"));

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), fromjson("{_id: 'abc', x: 1}"));
    ASSERT_TRUE(strategy.getCachedPlanRoot_forTest());
}

// --- Lifecycle: releaseResources --------------------------------------------------------------

TEST_F(SbeSingleDocumentLookupExecutorTest, ReleaseResourcesDropsAllCachedState) {
    createCollection();
    insertDocuments({fromjson("{_id: 1, x: 'a'}"), fromjson("{_id: 2, x: 'b'}")});

    auto strategy = makeStrategy();
    auto r1 = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(r1.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_TRUE(strategy.holdsAttachedCatalogState_forTest());
    const auto rebuildCountBefore = strategy.getPlanRebuildCount_forTest();
    ASSERT_EQ(rebuildCountBefore, 1U);

    strategy.releaseResources();
    ASSERT_FALSE(strategy.holdsAttachedCatalogState_forTest());

    // releaseResources() unconditionally tears the plan down along with the acquisition, since the
    // plan's cached collection pointer would otherwise outlive the acquisition it was built
    // against. The next lookup rebuilds from scratch.
    auto r2 = lookup(&strategy, fromjson("{_id: 2}"));
    ASSERT_EQ(r2.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(r2.document->toBson(), fromjson("{_id: 2, x: 'b'}"));
    ASSERT_EQ(strategy.getPlanRebuildCount_forTest(), rebuildCountBefore + 1);
    ASSERT_TRUE(strategy.holdsAttachedCatalogState_forTest());
}

TEST_F(SbeSingleDocumentLookupExecutorTest, ReleaseResourcesIsIdempotent) {
    createCollection();
    insertDocuments({fromjson("{_id: 1}")});

    auto strategy = makeStrategy();
    lookup(&strategy, fromjson("{_id: 1}"));

    strategy.releaseResources();
    strategy.releaseResources();
    ASSERT_FALSE(strategy.holdsAttachedCatalogState_forTest());

    auto result = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
}

TEST_F(SbeSingleDocumentLookupExecutorTest, ReleaseResourcesBeforeAnyLookupIsANoop) {
    auto strategy = makeStrategy();
    ASSERT_FALSE(strategy.holdsAttachedCatalogState_forTest());
    strategy.releaseResources();
    ASSERT_FALSE(strategy.holdsAttachedCatalogState_forTest());
}

// --- releaseResources() tears down every acquirer kind uniformly -------------------------------

// releaseResources() always fully tears down cached state, regardless of which CollectionAcquirer
// backs the strategy: even a PreAcquiredCollectionAcquirer-backed strategy, whose Handle keeps
// working against the same upfront-acquired, stasher-owned CollectionAcquisition, rebuilds a fresh
// plan on the next performLookup rather than reusing the one from before releaseResources().
TEST_F(SbeSingleDocumentLookupExecutorTest,
       PreAcquiredAcquirerAlsoRebuildsPlanAfterReleaseResources) {
    createCollection();
    insertDocuments({fromjson("{_id: 1, x: 'a'}"), fromjson("{_id: 2, x: 'b'}")});

    auto acquisition =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), kNss, AcquisitionPrerequisites::kRead),
                          MODE_IS);
    auto stasher = make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
    stashTransactionResourcesFromOperationContext(operationContext(), stasher.get());

    SbeSingleDocumentLookupExecutor strategy(
        std::make_unique<PreAcquiredCollectionAcquirer>(stasher, std::move(acquisition)),
        std::make_unique<AlwaysLocalEligibility>());

    auto r1 = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(r1.status, LookupResult::HandledStatus::kDocumentFound);
    const auto rebuildCountBefore = strategy.getPlanRebuildCount_forTest();
    ASSERT_EQ(rebuildCountBefore, 1U);

    strategy.releaseResources();
    ASSERT_FALSE(strategy.holdsAttachedCatalogState_forTest());

    auto r2 = lookup(&strategy, fromjson("{_id: 2}"));
    ASSERT_EQ(r2.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(r2.document->toBson(), fromjson("{_id: 2, x: 'b'}"));
    ASSERT_EQ(strategy.getPlanRebuildCount_forTest(), rebuildCountBefore + 1)
        << "releaseResources() must tear down the plan for every acquirer kind, not just OnDemand";
}

// --- Lifecycle: DDL between lookups in the same batch window -----------------------------------

TEST_F(SbeSingleDocumentLookupExecutorTest, UuidMismatchMidBatchReportsNotFound) {
    createCollection();
    insertDocuments({fromjson("{_id: 1, x: 'old'}")});
    const UUID actualUuid = currentUuid();
    const UUID staleUuid = UUID::gen();
    ASSERT_NE(actualUuid, staleUuid);

    auto strategy = makeStrategy();
    auto r1 = lookup(&strategy, fromjson("{_id: 1}"), actualUuid);
    ASSERT_EQ(r1.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_TRUE(strategy.holdsAttachedCatalogState_forTest());

    // Same batch window, no release in between: the cached acquisition is reused, and the next
    // event's UUID (as an older, since-superseded oplog entry would carry after a rename+recreate)
    // no longer matches it. Must report not-found rather than reading the wrong collection, and
    // must drop the cache rather than wedge it on the stale acquisition.
    auto r2 = lookup(&strategy, fromjson("{_id: 1}"), staleUuid);
    ASSERT_EQ(r2.status, LookupResult::HandledStatus::kDocumentNotFound);
    ASSERT_FALSE(strategy.holdsAttachedCatalogState_forTest());

    // The strategy recovers on the next lookup against the collection's real, current UUID.
    auto r3 = lookup(&strategy, fromjson("{_id: 1}"), actualUuid);
    ASSERT_EQ(r3.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(r3.document->toBson(), fromjson("{_id: 1, x: 'old'}"));
}

// Dropping the collection right after releaseResources() used to trip the
// ConsistentCollection::checkNoCollectionsInUse debug invariant, because releaseResources() only
// saved the plan and dropped the executor's own _collectionAcquisition bookkeeping, while
// sbe::ScanStageBase::_coll -- the actual CollectionAcquisition the scan stage holds, set once in
// prepare() -- is a second, independent reference that saveState() never clears. Since
// releaseResources() now fully tears the plan down along with the acquisition, that stale
// reference can no longer outlive the acquisition it was built against.
TEST_F(SbeSingleDocumentLookupExecutorTest, ReleaseResourcesThenDropCollectionIsSafe) {
    createCollection();
    insertDocuments({fromjson("{_id: 1}")});

    auto strategy = makeStrategy();
    auto r1 = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(r1.status, LookupResult::HandledStatus::kDocumentFound);

    strategy.releaseResources();
    dropCollection();

    auto r2 = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(r2.status, LookupResult::HandledStatus::kDocumentNotFound);
}

// Same root cause as above: any catalog-mutating operation on the same OperationContext after only
// releaseResources() -- not just a drop, even an index-metadata rebuild taking a normal MODE_X
// collection lock -- used to hit the still-live sbe::ScanStageBase::_coll reference in the cached
// plan. Now that releaseResources() always tears the plan down, isExecutorInvalidated()'s
// version-check branch is reachable: the next lookup detects the bumped
// PlanCacheInvalidatorVersion and rebuilds.
TEST_F(SbeSingleDocumentLookupExecutorTest, PlanCacheInvalidationMidBatchTriggersReplan) {
    createCollection();
    insertDocuments({fromjson("{_id: 1, x: 'a'}"), fromjson("{_id: 2, x: 'b'}")});

    auto strategy = makeStrategy();
    auto r1 = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(r1.status, LookupResult::HandledStatus::kDocumentFound);
    const auto rebuildCountBefore = strategy.getPlanRebuildCount_forTest();
    ASSERT_EQ(rebuildCountBefore, 1U);

    strategy.releaseResources();
    bumpPlanCacheInvalidatorVersion();

    auto r2 = lookup(&strategy, fromjson("{_id: 2}"));
    ASSERT_EQ(r2.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(r2.document->toBson(), fromjson("{_id: 2, x: 'b'}"));
    ASSERT_EQ(strategy.getPlanRebuildCount_forTest(), rebuildCountBefore + 1);
}

// --- Plan summary stats sink -------------------------------------------------------------------

TEST_F(SbeSingleDocumentLookupExecutorTest,
       PlanSummaryStatsSinkAccumulatesWithoutRebuildAcrossReleaseBoundary) {
    createCollection();
    insertDocuments({fromjson("{_id: 1}"), fromjson("{_id: 2}"), fromjson("{_id: 3}")});

    auto strategy = makeStrategy();
    PlanSummaryStats sink;
    strategy.setPlanSummaryStatsSink(&sink);

    // The sink only folds in at teardown (releaseResources()/resetPlan()), not after every
    // lookup, so it stays untouched mid-window.
    lookup(&strategy, fromjson("{_id: 1}"));
    lookup(&strategy, fromjson("{_id: 2}"));
    ASSERT_EQ(sink.totalKeysExamined, 0U);
    ASSERT_EQ(sink.totalDocsExamined, 0U);

    strategy.releaseResources();
    ASSERT_GT(sink.totalKeysExamined, 0U);
    ASSERT_GT(sink.totalDocsExamined, 0U);
    const auto afterFirstWindow = sink.totalKeysExamined;

    // releaseResources() tore the plan down, so this second window rebuilds a fresh
    // PreparedExecutor with its own cumulative SBE counters starting back at 0. The sink itself is
    // external state owned by the caller, so its total keeps accumulating across the rebuild.
    lookup(&strategy, fromjson("{_id: 3}"));
    strategy.releaseResources();

    ASSERT_GT(sink.totalKeysExamined, afterFirstWindow);
}

// Regression test for the delta baseline being scoped to PreparedExecutor rather than to the
// strategy: forcing a plan rebuild between two windows must not underflow the sink's cumulative
// total (the new PreparedExecutor's lastAccumulatedKeysExamined must start at 0, not carry over
// the previous plan's high-water mark).
TEST_F(SbeSingleDocumentLookupExecutorTest, PlanSummaryStatsSinkSurvivesPlanRebuild) {
    createCollection();
    insertDocuments({fromjson("{_id: 1}"), fromjson("{_id: 2}")});

    auto strategy = makeStrategy();
    PlanSummaryStats sink;
    strategy.setPlanSummaryStatsSink(&sink);

    lookup(&strategy, fromjson("{_id: 1}"));
    strategy.releaseResources();
    const auto afterFirstWindow = sink.totalKeysExamined;
    ASSERT_GT(afterFirstWindow, 0U);

    bumpPlanCacheInvalidatorVersion();

    lookup(&strategy, fromjson("{_id: 2}"));
    strategy.releaseResources();

    ASSERT_GT(sink.totalKeysExamined, afterFirstWindow);
}

// --- Metrics recording --------------------------------------------------------------------------

TEST_F(SbeSingleDocumentLookupExecutorTest, FoundDocumentRecordsFoundAndLatency) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    createCollection();
    insertDocuments({fromjson("{_id: 1}")});
    auto strategy = makeStrategyWithRealRecorder(std::make_unique<AlwaysLocalEligibility>());

    const auto before = snapshotSbeCell(capturer);
    auto result = lookup(&strategy, fromjson("{_id: 1}"));
    const auto after = snapshotSbeCell(capturer);

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
    ASSERT_EQ(after.found, before.found + 1);
    ASSERT_EQ(after.notFound, before.notFound);
    ASSERT_EQ(after.notHandled, before.notHandled);
    ASSERT_EQ(after.latencyCount, before.latencyCount + 1);
}

TEST_F(SbeSingleDocumentLookupExecutorTest, AbsentDocumentRecordsNotFoundAndLatency) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    createCollection();
    insertDocuments({fromjson("{_id: 1}")});
    auto strategy = makeStrategyWithRealRecorder(std::make_unique<AlwaysLocalEligibility>());

    const auto before = snapshotSbeCell(capturer);
    auto result = lookup(&strategy, fromjson("{_id: 999}"));
    const auto after = snapshotSbeCell(capturer);

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);
    ASSERT_EQ(after.found, before.found);
    ASSERT_EQ(after.notFound, before.notFound + 1);
    ASSERT_EQ(after.notHandled, before.notHandled);
    ASSERT_EQ(after.latencyCount, before.latencyCount + 1);
}

TEST_F(SbeSingleDocumentLookupExecutorTest, UnknownDecisionRecordsNotHandledWithNoLatency) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    createCollection();
    insertDocuments({fromjson("{_id: 1}")});
    auto strategy = makeStrategyWithRealRecorder(MockLocalLookupEligibility::makeAlwaysUnknown());

    const auto before = snapshotSbeCell(capturer);
    auto result = lookup(&strategy, fromjson("{_id: 1}"));
    const auto after = snapshotSbeCell(capturer);

    ASSERT_EQ(result.status, LookupResult::HandledStatus::kNotHandled);
    ASSERT_EQ(after.found, before.found);
    ASSERT_EQ(after.notFound, before.notFound);
    ASSERT_EQ(after.notHandled, before.notHandled + 1);
    // A declined lookup carries no meaningful latency.
    ASSERT_EQ(after.latencyCount, before.latencyCount);
}

// --- Shard filtering -----------------------------------------------------------------------
//
// Exercises the acquisition's real shard filter against a physically-present orphan and inspects
// the actual compiled plan, rather than asserting on an eligibility's checksShardKeyOwnership()
// flag in isolation: proves the planner actually includes (or omits) the shard-filter stage, and
// that doing so actually drops orphans, not just that the flag it reads is set correctly. The
// behavioral tests mirror express_single_document_lookup_executor_test.cpp's
// ExpressLookupShardFilteringTest; the plan-inspection ones are SBE-only, since Express has no
// compiled plan tree to introspect.

class SbeSingleDocumentLookupExecutorShardFilteringTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();
        _client = std::make_unique<DBDirectClient>(operationContext());
        _client->createCollection(kShardedNss);
        _expCtx = make_intrusive<ExpressionContextForTest>(operationContext(), kShardedNss);
    }

    // Shard key 'shardKeyField' split at 'splitPoint': values < splitPoint are owned, >= are
    // orphans owned by "otherShard". Pass "_id" to mirror an _id-sharded collection.
    CollectionMetadata setupShardingMetadata(int splitPoint,
                                             const std::string& shardKeyField = "sk") {
        OperationContext* opCtx = operationContext();
        const UUID uuid = [&] {
            AutoGetCollection autoColl(opCtx, kShardedNss, MODE_IS);
            return autoColl->uuid();
        }();

        const ShardKeyPattern shardKeyPattern(BSON(shardKeyField << 1));
        const KeyPattern keyPattern = shardKeyPattern.getKeyPattern();
        const OID epoch = OID::gen();
        const Timestamp timestamp(1, 1);
        ChunkVersion version({epoch, timestamp}, {1, 0});

        ChunkType ownedChunk(uuid,
                             ChunkRange{keyPattern.globalMin(), BSON(shardKeyField << splitPoint)},
                             version,
                             kMyShardName);
        version.incMinor();
        ChunkType orphanChunk(uuid,
                              ChunkRange{BSON(shardKeyField << splitPoint), keyPattern.globalMax()},
                              version,
                              ShardId("otherShard"));

        auto rt = RoutingTableHistory::makeNew(kShardedNss,
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

        auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, kShardedNss);
        scopedCsr->setCollectionMetadata(opCtx, metadata);
        return metadata;
    }

    // SBE strategy over the real sharded collection (OnDemand acquirer) with the given eligibility.
    SbeSingleDocumentLookupExecutor makeStrategy(
        std::unique_ptr<LocalLookupEligibility> eligibility) {
        return SbeSingleDocumentLookupExecutor(std::make_unique<OnDemandCollectionAcquirer>(),
                                               std::move(eligibility));
    }

    LookupResult lookup(SingleDocumentLookupExecutor* strategy, const BSONObj& documentKey) {
        return strategy->performLookup(
            _expCtx, kShardedNss, boost::none, Document(documentKey), boost::optional<Timestamp>());
    }

    std::unique_ptr<DBDirectClient> _client;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

// AlwaysLocal-shaped eligibility (checksShardKeyOwnership() == false, see
// AlwaysLocalDoesNotCheckShardKeyOwnership in local_lookup_eligibility_test.cpp): the planner must
// include the shard-filter stage.
TEST_F(SbeSingleDocumentLookupExecutorShardFilteringTest,
       EligibilityWithoutOwnershipCheckIncludesShardFilterStage) {
    auto metadata = setupShardingMetadata(/*splitPoint*/ 10);
    _client->insert(kShardedNss, BSON("_id" << 1 << "sk" << 1));  // owned: sk < 10

    ScopedSetShardRole scopedSetShardRole{
        operationContext(), kShardedNss, ShardVersionFactory::make(metadata), boost::none};

    auto strategy = makeStrategy(std::make_unique<AlwaysLocalEligibility>());
    auto result = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);

    const auto* root = strategy.getCachedPlanRoot_forTest();
    ASSERT_TRUE(root);
    auto types = planStageTypes(root);
    ASSERT_TRUE(contains(types, "filter"))
        << "Expected a shard-filter stage since the eligibility does not check ownership, got: "
        << joinStageTypes(types);
}

// Same eligibility shape, but the looked-up document is an orphan: the included shard-filter stage
// actually drops it. Mirrors ExpressLookupShardFilteringTest's
// EligibilityWithoutOwnershipCheckAppliesShardFilterAndDropsOrphan.
TEST_F(SbeSingleDocumentLookupExecutorShardFilteringTest,
       EligibilityWithoutOwnershipCheckAppliesShardFilterAndDropsOrphan) {
    auto metadata = setupShardingMetadata(/*splitPoint*/ 10);
    _client->insert(kShardedNss, BSON("_id" << 1 << "sk" << 99));  // orphan: sk >= 10

    ScopedSetShardRole scopedSetShardRole{
        operationContext(), kShardedNss, ShardVersionFactory::make(metadata), boost::none};

    auto strategy = makeStrategy(std::make_unique<AlwaysLocalEligibility>());
    auto result = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);
}

// Mirrors buildIdLookupExecutor: the shard filter comes from the pre-acquired acquisition, not from
// a per-lookup ScopedSetShardRole.
TEST_F(SbeSingleDocumentLookupExecutorShardFilteringTest,
       PreAcquiredShardVersionedAcquisitionDropsOrphanWithoutPerLookupShardRole) {
    auto metadata = setupShardingMetadata(/*splitPoint*/ 10);
    _client->insert(kShardedNss, BSON("_id" << 1 << "sk" << 99));  // orphan: sk >= 10

    // Acquire under the shard version, then stash so PreAcquired swaps it back on per lookup.
    boost::optional<CollectionAcquisition> acquisition;
    {
        ScopedSetShardRole scopedSetShardRole{
            operationContext(), kShardedNss, ShardVersionFactory::make(metadata), boost::none};
        acquisition.emplace(
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), kShardedNss, AcquisitionPrerequisites::kRead),
                              MODE_IS));
    }
    auto stasher = make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
    stashTransactionResourcesFromOperationContext(operationContext(), stasher.get());

    SbeSingleDocumentLookupExecutor strategy(
        std::make_unique<PreAcquiredCollectionAcquirer>(stasher, std::move(*acquisition)),
        std::make_unique<AlwaysLocalEligibility>());

    auto result = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentNotFound);
}

// The cached plan's shardFilterer slot is set once at build time, so a reused strategy must keep
// dropping orphans across lookups.
TEST_F(SbeSingleDocumentLookupExecutorShardFilteringTest,
       ReusedPlanKeepsDroppingOrphansAcrossBatchLookups) {
    auto metadata = setupShardingMetadata(/*splitPoint*/ 10);
    _client->insert(kShardedNss, BSON("_id" << 1 << "sk" << 1));    // owned:  sk < 10
    _client->insert(kShardedNss, BSON("_id" << 2 << "sk" << 99));   // orphan: sk >= 10
    _client->insert(kShardedNss, BSON("_id" << 3 << "sk" << 100));  // orphan: sk >= 10

    ScopedSetShardRole scopedSetShardRole{
        operationContext(), kShardedNss, ShardVersionFactory::make(metadata), boost::none};

    auto strategy = makeStrategy(std::make_unique<AlwaysLocalEligibility>());

    auto r1 = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(r1.status, LookupResult::HandledStatus::kDocumentFound) << "owned doc must be found";

    auto r2 = lookup(&strategy, fromjson("{_id: 2}"));
    ASSERT_EQ(r2.status, LookupResult::HandledStatus::kDocumentNotFound)
        << "orphan must be dropped on a reused plan (2nd lookup)";

    auto r3 = lookup(&strategy, fromjson("{_id: 3}"));
    ASSERT_EQ(r3.status, LookupResult::HandledStatus::kDocumentNotFound)
        << "orphan must be dropped on a reused plan (3rd lookup)";
}

// When sharded on _id, the scan field and shard key coincide; the filter must still drop orphans.
TEST_F(SbeSingleDocumentLookupExecutorShardFilteringTest,
       IdShardKeyDropsOrphansAcrossBatchLookups) {
    auto metadata = setupShardingMetadata(/*splitPoint*/ 2, /*shardKeyField*/ "_id");
    _client->insert(kShardedNss, BSON("_id" << 1));  // owned:  _id < 2
    _client->insert(kShardedNss, BSON("_id" << 2));  // orphan: _id >= 2
    _client->insert(kShardedNss, BSON("_id" << 3));  // orphan: _id >= 2

    ScopedSetShardRole scopedSetShardRole{
        operationContext(), kShardedNss, ShardVersionFactory::make(metadata), boost::none};

    auto strategy = makeStrategy(std::make_unique<AlwaysLocalEligibility>());

    auto r1 = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(r1.status, LookupResult::HandledStatus::kDocumentFound) << "owned doc must be found";

    auto r2 = lookup(&strategy, fromjson("{_id: 2}"));
    ASSERT_EQ(r2.status, LookupResult::HandledStatus::kDocumentNotFound)
        << "orphan must be dropped when the shard key is _id (2nd lookup)";

    auto r3 = lookup(&strategy, fromjson("{_id: 3}"));
    ASSERT_EQ(r3.status, LookupResult::HandledStatus::kDocumentNotFound)
        << "orphan must be dropped when the shard key is _id (3rd lookup)";
}

// An eligibility whose Local decision already resolved ownership from the shard key: the planner
// must omit the redundant shard-filter stage.
TEST_F(SbeSingleDocumentLookupExecutorShardFilteringTest,
       EligibilityThatChecksOwnershipOmitsShardFilterStage) {
    auto metadata = setupShardingMetadata(/*splitPoint*/ 10);
    _client->insert(kShardedNss, BSON("_id" << 1 << "sk" << 1));  // owned: sk < 10

    ScopedSetShardRole scopedSetShardRole{
        operationContext(), kShardedNss, ShardVersionFactory::make(metadata), boost::none};

    auto eligibility = MockLocalLookupEligibility::makeAlwaysLocal();
    eligibility->setChecksShardKeyOwnership(true);
    auto strategy = makeStrategy(std::move(eligibility));
    auto result = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);

    const auto* root = strategy.getCachedPlanRoot_forTest();
    ASSERT_TRUE(root);
    auto types = planStageTypes(root);
    ASSERT_FALSE(contains(types, "filter"))
        << "Expected no shard-filter stage since the eligibility already checked ownership, got: "
        << joinStageTypes(types);
}

// Same eligibility shape, but the looked-up document is a physically-present orphan: with the
// shard-filter stage omitted, the orphan is returned rather than dropped. Mirrors
// ExpressLookupShardFilteringTest's EligibilityThatChecksOwnershipSkipsShardFilterAndReturnsOrphan.
TEST_F(SbeSingleDocumentLookupExecutorShardFilteringTest,
       EligibilityThatChecksOwnershipSkipsShardFilterAndReturnsOrphan) {
    auto metadata = setupShardingMetadata(/*splitPoint*/ 10);
    _client->insert(kShardedNss, BSON("_id" << 1 << "sk" << 99));  // orphan: sk >= 10

    ScopedSetShardRole scopedSetShardRole{
        operationContext(), kShardedNss, ShardVersionFactory::make(metadata), boost::none};

    auto eligibility = MockLocalLookupEligibility::makeAlwaysLocal();
    eligibility->setChecksShardKeyOwnership(true);
    auto strategy = makeStrategy(std::move(eligibility));
    auto result = lookup(&strategy, fromjson("{_id: 1}"));
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
}

}  // namespace
}  // namespace mongo::exec::agg
