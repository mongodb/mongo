// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/query/stage_builder/classic_stage_builder.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/query/record_id_range.h"
#include "mongo/db/query/record_id_range_list.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_mock.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_mock.h"
#include "mongo/db/shard_role/shard_role_mock.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <string_view>
#include <utility>
#include <vector>

namespace mongo {

const static NamespaceString kNss = NamespaceString::createNamespaceString_forTest("db.dummy");

class ClassicStageBuilderTest : public ServiceContextMongoDTest {
public:
    ClassicStageBuilderTest() : ServiceContextMongoDTest(Options{}.useMockClock(true)) {}

    void setUp() override {
        _opCtx = makeOperationContext();
        _workingSet = std::make_unique<WorkingSet>();

        auto indexCatalog = std::make_unique<IndexCatalogMock>();
        IndexSpec spec;
        spec.version(1).name("a_1").addKeys(BSON("a" << 1));
        auto desc = IndexDescriptor(IndexNames::BTREE, spec.toBSON());
        indexCatalog->createIndexEntry(
            _opCtx.get(), nullptr /*collection*/, std::move(desc), CreateIndexEntryFlags::kNone);
        _collection = std::make_unique<CollectionMock>(UUID::gen(), kNss, std::move(indexCatalog));
    }

    void tearDown() override {
        _opCtx.reset();
        _workingSet.reset();
        _planStageQsnMap.clear();
    }

    /**
     * Converts a 'QuerySolutionNode' to a 'QuerySolution'.
     */
    std::unique_ptr<QuerySolution> makeQuerySolution(std::unique_ptr<QuerySolutionNode> root) {
        auto querySoln = std::make_unique<QuerySolution>();
        querySoln->setRoot(std::move(root));
        return querySoln;
    }

    /**
     * Builds a PlanStage using the given WorkingSet and QuerySolution.
     */
    std::unique_ptr<PlanStage> buildPlanStage(std::unique_ptr<QuerySolution> querySolution,
                                              Collection* collection = nullptr) {
        auto findCommand = std::make_unique<FindCommandRequest>(kNss);
        auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx(), *findCommand).build(),
            .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});

        // The collection holder is guaranteed to be valid for the lifetime of the test. This
        // initialization is safe.
        CollectionPtr collptr =
            CollectionPtr::CollectionPtr_UNSAFE(collection ? collection : _collection.get());
        auto coll =
            shard_role_mock::acquireCollectionMocked(_opCtx.get(), kNss, std::move(collptr));
        stage_builder::ClassicStageBuilder builder{
            opCtx(), coll, *cq, *querySolution, workingSet(), &_planStageQsnMap};
        return builder.build(querySolution->root());
    }

    /**
     * Builds a collection whose index catalog holds a single index with the given name and key
     * pattern. As in 'IndexCatalogMock', the index's ident is its name.
     */
    std::unique_ptr<Collection> makeCollectionWithIndex(std::string indexName, const BSONObj& kp) {
        auto indexCatalog = std::make_unique<IndexCatalogMock>();
        IndexSpec spec;
        spec.version(1).name(std::move(indexName)).addKeys(kp);
        indexCatalog->createIndexEntry(
            _opCtx.get(),
            nullptr /*collection*/,
            IndexDescriptor(IndexNames::findPluginName(kp), spec.toBSON()),
            CreateIndexEntryFlags::kNone);
        return std::make_unique<CollectionMock>(UUID::gen(), kNss, std::move(indexCatalog));
    }

    /**
     * A helper to repeatedly call work() until the stage returns a PlanStage::IS_EOF state and
     * returns the resulting documents as a vector of BSONObj.
     */
    std::vector<BSONObj> collectResults(std::unique_ptr<PlanStage> stage) {
        WorkingSetID id;
        std::vector<BSONObj> results;
        auto state = PlanStage::ADVANCED;

        while (state != PlanStage::IS_EOF) {
            state = stage->work(&id);
            if (state == PlanStage::ADVANCED) {
                auto member = workingSet()->get(id);
                auto doc = member->doc.value().toBson();
                results.push_back(doc);
            }
        }
        return results;
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    WorkingSet* workingSet() {
        return _workingSet.get();
    }

    const stage_builder::PlanStageToQsnMap& planStageQsnMap() const {
        return _planStageQsnMap;
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<WorkingSet> _workingSet;
    std::unique_ptr<Collection> _collection;
    stage_builder::PlanStageToQsnMap _planStageQsnMap;
};

namespace {
// Builds an IndexEntry whose indexCatalogEntryStorage has the given ident. The index plugin (and
// so the descriptor's access method) is derived from the key pattern.
IndexEntry buildIndexEntryWithIdent(const BSONObj& kp,
                                    std::string_view ident,
                                    std::string indexName = "a_1") {
    IndexSpec spec;
    spec.version(1).name(indexName).addKeys(kp);
    const std::string pluginName = IndexNames::findPluginName(kp);
    auto mockEntry =
        std::make_shared<IndexCatalogEntryMock>(nullptr,
                                                CollectionPtr{},
                                                std::string(ident),
                                                IndexDescriptor(pluginName, spec.toBSON()),
                                                false /* isFrozen */);
    IndexEntry entry{kp,
                     IndexNames::nameToType(pluginName),
                     IndexConfig::kLatestIndexVersion,
                     false,
                     {},
                     {},
                     false,
                     false,
                     CoreIndexInfo::Identifier(std::move(indexName)),
                     {},
                     nullptr,
                     std::move(mockEntry)};
    return entry;
}

IndexEntry buildSimpleIndexEntry(const BSONObj& kp) {
    return buildIndexEntryWithIdent(kp, "a_1");
}
}  // namespace

/**
 * Verify that a VirtualScanNode can be translated to a MockStage and produce a filtered data
 * stream.
 */
TEST_F(ClassicStageBuilderTest, VirtualScanTranslation) {
    static const std::vector<BSONArray> kFilteredDocs = std::vector<BSONArray>{
        BSON_ARRAY(BSON("a" << 1 << "b" << 2)), BSON_ARRAY(BSON("a" << 3 << "b" << 2))};
    BSONObj query = fromjson("{a: {$ne: 2}}");
    auto filter = uassertStatusOK(MatchExpressionParser::parse(
        query, make_intrusive<ExpressionContextForTest>(opCtx(), kNss)));

    std::vector<BSONArray> allDocs = kFilteredDocs;
    allDocs.insert(allDocs.begin() + 1, BSON_ARRAY(BSON("a" << 2 << "b" << 2)));
    allDocs.insert(allDocs.end(), BSON_ARRAY(BSON("a" << 2 << "b" << 2)));

    // Construct a QuerySolution consisting of a single VirtualScanNode to test if a stream of
    // documents can be produced and filtered, according to the provided filter.
    auto virtScan = std::make_unique<VirtualScanNode>(
        std::move(allDocs), VirtualScanNode::ScanType::kCollScan, false);
    virtScan->filter = std::move(filter);
    // Make a QuerySolution from the root virtual scan node.
    auto querySolution = makeQuerySolution(std::move(virtScan));
    ASSERT_EQ(querySolution->root()->nodeId(), 1);

    // Translate the QuerySolution to a classic PlanStage.
    auto stage = buildPlanStage(std::move(querySolution));

    // Work the stage and collect the results.
    auto results = collectResults(std::move(stage));
    ASSERT_EQ(results.size(), kFilteredDocs.size());

    // Check that the results produced from the translated VirtualScanNode meet expectation.
    for (size_t i = 0; i < kFilteredDocs.size(); ++i) {
        BSONObjIterator arrIt{kFilteredDocs[i]};
        auto firstElt = arrIt.next();
        ASSERT_BSONOBJ_EQ(firstElt.embeddedObject(), results[i]);
    }
}

TEST_F(ClassicStageBuilderTest, IndexFetchTranslationPopulatesMap) {
    auto idxScan = std::make_unique<IndexScanNode>(kNss, buildSimpleIndexEntry(BSON("a" << 1)));
    QuerySolutionNode* idxScanPtr = idxScan.get();
    auto fetch = std::make_unique<FetchNode>(std::move(idxScan), kNss);
    QuerySolutionNode* fetchPtr = fetch.get();

    auto stage = buildPlanStage(makeQuerySolution(std::move(fetch)));

    stage_builder::PlanStageToQsnMap expectedResults = {{stage.get(), fetchPtr},
                                                        {stage->child().get(), idxScanPtr}};
    ASSERT_EQ(expectedResults, planStageQsnMap());
}

// The stage builder looks up the index by ident when building the plan. If the ident is gone
// from the catalog (index was dropped after planning), it throws QueryPlanKilled.
TEST_F(ClassicStageBuilderTest, DroppedIndexThrowsQueryPlanKilled) {
    // Use an empty catalog — no entries at all — so both name and ident lookup fail.
    // This simulates the index having been dropped with no replacement.
    auto emptyCollection =
        std::make_unique<CollectionMock>(UUID::gen(), kNss, std::make_unique<IndexCatalogMock>());
    auto idxScan = std::make_unique<IndexScanNode>(
        kNss, buildIndexEntryWithIdent(BSON("a" << 1), "original-ident"));
    ASSERT_THROWS_CODE(buildPlanStage(makeQuerySolution(std::move(idxScan)), emptyCollection.get()),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

// Even when the catalog has an index with the same NAME as the planned index, a different ident
// means the original index was dropped and replaced. The stage builder detects this via ident
// comparison (not name lookup) and throws QueryPlanKilled.
TEST_F(ClassicStageBuilderTest, DroppedAndReplacedIndexThrowsQueryPlanKilled) {
    // setUp has "a_1" with ident "a_1" in the catalog. The plan refers to "original-ident".
    // findIndexByName("a_1") would succeed, but findIndexByIdent("original-ident") returns
    // null — proving the check is ident-based, not name-based.
    auto idxScan = std::make_unique<IndexScanNode>(
        kNss, buildIndexEntryWithIdent(BSON("a" << 1), "original-ident"));
    ASSERT_THROWS_CODE(buildPlanStage(makeQuerySolution(std::move(idxScan))),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

// Same ident-based check as for STAGE_IXSCAN, but for STAGE_DISTINCT_SCAN. The catalog holds an
// index named "a_1" with ident "a_1", while the plan refers to ident "original-ident".
TEST_F(ClassicStageBuilderTest, DistinctScanDroppedAndReplacedIndexThrowsQueryPlanKilled) {
    auto distinct = std::make_unique<DistinctNode>(
        kNss, buildIndexEntryWithIdent(BSON("a" << 1), "original-ident"));
    ASSERT_THROWS_CODE(buildPlanStage(makeQuerySolution(std::move(distinct))),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

// As above, but with an empty catalog so that neither name nor ident lookup can succeed.
TEST_F(ClassicStageBuilderTest, DistinctScanDroppedIndexThrowsQueryPlanKilled) {
    auto emptyCollection =
        std::make_unique<CollectionMock>(UUID::gen(), kNss, std::make_unique<IndexCatalogMock>());
    auto distinct = std::make_unique<DistinctNode>(
        kNss, buildIndexEntryWithIdent(BSON("a" << 1), "original-ident"));
    ASSERT_THROWS_CODE(
        buildPlanStage(makeQuerySolution(std::move(distinct)), emptyCollection.get()),
        DBException,
        ErrorCodes::QueryPlanKilled);
}

TEST_F(ClassicStageBuilderTest, CountScanDroppedAndReplacedIndexThrowsQueryPlanKilled) {
    auto count = std::make_unique<CountScanNode>(
        kNss, buildIndexEntryWithIdent(BSON("a" << 1), "original-ident"));
    ASSERT_THROWS_CODE(buildPlanStage(makeQuerySolution(std::move(count))),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

TEST_F(ClassicStageBuilderTest, CountScanDroppedIndexThrowsQueryPlanKilled) {
    auto emptyCollection =
        std::make_unique<CollectionMock>(UUID::gen(), kNss, std::make_unique<IndexCatalogMock>());
    auto count = std::make_unique<CountScanNode>(
        kNss, buildIndexEntryWithIdent(BSON("a" << 1), "original-ident"));
    ASSERT_THROWS_CODE(buildPlanStage(makeQuerySolution(std::move(count)), emptyCollection.get()),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

// The geo and text plans below each come in two flavors: the index is gone from the catalog
// altogether, and the index was recreated under the same name but with a different ident. The
// latter is what makes the ident-based lookup necessary: a name-based lookup would find the
// replacement index and build a stage over the wrong index.
TEST_F(ClassicStageBuilderTest, GeoNear2DDroppedIndexThrowsQueryPlanKilled) {
    auto emptyCollection =
        std::make_unique<CollectionMock>(UUID::gen(), kNss, std::make_unique<IndexCatalogMock>());
    auto geoNear = std::make_unique<GeoNear2DNode>(
        kNss, buildIndexEntryWithIdent(BSON("loc" << "2d"), "original-ident", "loc_2d"));
    ASSERT_THROWS_CODE(buildPlanStage(makeQuerySolution(std::move(geoNear)), emptyCollection.get()),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

TEST_F(ClassicStageBuilderTest, GeoNear2DDroppedAndReplacedIndexThrowsQueryPlanKilled) {
    auto collection = makeCollectionWithIndex("loc_2d", BSON("loc" << "2d"));
    auto geoNear = std::make_unique<GeoNear2DNode>(
        kNss, buildIndexEntryWithIdent(BSON("loc" << "2d"), "original-ident", "loc_2d"));
    ASSERT_THROWS_CODE(buildPlanStage(makeQuerySolution(std::move(geoNear)), collection.get()),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

TEST_F(ClassicStageBuilderTest, GeoNear2DSphereDroppedIndexThrowsQueryPlanKilled) {
    auto emptyCollection =
        std::make_unique<CollectionMock>(UUID::gen(), kNss, std::make_unique<IndexCatalogMock>());
    auto geoNear = std::make_unique<GeoNear2DSphereNode>(
        kNss,
        buildIndexEntryWithIdent(BSON("loc" << "2dsphere"), "original-ident", "loc_2dsphere"));
    ASSERT_THROWS_CODE(buildPlanStage(makeQuerySolution(std::move(geoNear)), emptyCollection.get()),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

TEST_F(ClassicStageBuilderTest, GeoNear2DSphereDroppedAndReplacedIndexThrowsQueryPlanKilled) {
    auto collection = makeCollectionWithIndex("loc_2dsphere", BSON("loc" << "2dsphere"));
    auto geoNear = std::make_unique<GeoNear2DSphereNode>(
        kNss,
        buildIndexEntryWithIdent(BSON("loc" << "2dsphere"), "original-ident", "loc_2dsphere"));
    ASSERT_THROWS_CODE(buildPlanStage(makeQuerySolution(std::move(geoNear)), collection.get()),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

TEST_F(ClassicStageBuilderTest, TextMatchDroppedIndexThrowsQueryPlanKilled) {
    auto emptyCollection =
        std::make_unique<CollectionMock>(UUID::gen(), kNss, std::make_unique<IndexCatalogMock>());
    auto textMatch = std::make_unique<TextMatchNode>(
        kNss,
        buildIndexEntryWithIdent(BSON("txt" << "text"), "original-ident", "txt_text"),
        std::make_unique<fts::FTSQueryImpl>(),
        false /* wantTextScore */);
    ASSERT_THROWS_CODE(
        buildPlanStage(makeQuerySolution(std::move(textMatch)), emptyCollection.get()),
        DBException,
        ErrorCodes::QueryPlanKilled);
}

TEST_F(ClassicStageBuilderTest, TextMatchDroppedAndReplacedIndexThrowsQueryPlanKilled) {
    auto collection = makeCollectionWithIndex("txt_text", BSON("txt" << "text"));
    auto textMatch = std::make_unique<TextMatchNode>(
        kNss,
        buildIndexEntryWithIdent(BSON("txt" << "text"), "original-ident", "txt_text"),
        std::make_unique<fts::FTSQueryImpl>(),
        false /* wantTextScore */);
    ASSERT_THROWS_CODE(buildPlanStage(makeQuerySolution(std::move(textMatch)), collection.get()),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

namespace {
RecordIdRange makeIntRange(int min, bool minInclusive, int max, bool maxInclusive) {
    RecordIdRange r;
    r.maybeNarrowMin(RecordIdBound(RecordId(min)), minInclusive);
    r.maybeNarrowMax(RecordIdBound(RecordId(max)), maxInclusive);
    return r;
}

// CollectionMock with isClustered() == true, as required by MultiRangeClusteredScan's
// constructor check.
class ClusteredCollectionMock : public CollectionMock {
public:
    ClusteredCollectionMock(const UUID& uuid, const NamespaceString& nss)
        : CollectionMock(uuid, nss) {}
    bool isClustered() const override {
        return true;
    }
};
}  // namespace

// A bounded CollectionScanNode on a clustered collection with exactly one range (including an
// unbounded CollectionScanNode) collapses to a contiguous CollectionScan.
TEST_F(ClassicStageBuilderTest, CollScanSingleRangeDispatchesToCollectionScan) {
    auto clusteredColl = std::make_unique<ClusteredCollectionMock>(UUID::gen(), kNss);
    auto csn = std::make_unique<CollectionScanNode>(kNss);
    auto stage = buildPlanStage(makeQuerySolution(std::move(csn)), clusteredColl.get());
    ASSERT_EQ(stage->stageType(), STAGE_COLLSCAN);

    csn = std::make_unique<CollectionScanNode>(kNss);
    csn->rangeList = RecordIdRangeList{makeIntRange(1, true, 10, true)};
    stage = buildPlanStage(makeQuerySolution(std::move(csn)), clusteredColl.get());
    ASSERT_EQ(stage->stageType(), STAGE_COLLSCAN);
}

// A CollectionScanNode on a clustered collection with two disjoint ranges dispatches to
// MultiRangeClusteredScan.
TEST_F(ClassicStageBuilderTest, CollScanMultiRangeDispatchesToMultiRangeClusteredScan) {
    auto clusteredColl = std::make_unique<ClusteredCollectionMock>(UUID::gen(), kNss);
    auto csn = std::make_unique<CollectionScanNode>(kNss);
    csn->rangeList = RecordIdRangeList::makeUnion(
        {makeIntRange(1, true, 5, true), makeIntRange(10, true, 20, true)});

    auto stage = buildPlanStage(makeQuerySolution(std::move(csn)), clusteredColl.get());
    ASSERT_EQ(stage->stageType(), STAGE_COLLSCAN_MULTI_RANGE);
}

// An empty rangeList (∅, zero ranges) on a clustered collection also dispatches to
// MultiRangeClusteredScan.
TEST_F(ClassicStageBuilderTest, CollScanEmptyRangeListDispatchesToMultiRangeClusteredScan) {
    auto clusteredColl = std::make_unique<ClusteredCollectionMock>(UUID::gen(), kNss);
    auto csn = std::make_unique<CollectionScanNode>(kNss);
    csn->rangeList = RecordIdRangeList::makeUnion({});  // explicit empty list

    auto stage = buildPlanStage(makeQuerySolution(std::move(csn)), clusteredColl.get());
    ASSERT_EQ(stage->stageType(), STAGE_COLLSCAN_MULTI_RANGE);
}

}  // namespace mongo
