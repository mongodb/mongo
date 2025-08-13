/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"

#include <benchmark/benchmark.h>

namespace mongo {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.collection");
const double kIndexUpperBound = 0.0101;

class SbeStageBuilderBenchmark : public CatalogTestFixture {
public:
    SbeStageBuilderBenchmark() : CatalogTestFixture() {
        setUp();
    }

    ~SbeStageBuilderBenchmark() override {
        tearDown();
    }

    std::unique_ptr<IndexScanNode> makeIndexScanNode(std::string index,
                                                     std::string indexName,
                                                     double lowBound,
                                                     double highBound) {
        auto child = std::make_unique<IndexScanNode>(createIndexEntry(BSON(index << 1), indexName));
        IndexBounds bounds{};
        OrderedIntervalList oil(index);
        oil.intervals.emplace_back(BSON("" << lowBound << "" << highBound), true, true);
        bounds.fields.emplace_back(std::move(oil));
        child->bounds = std::move(bounds);
        child->sortSet = ProvidedSortSet{BSON(index << 1)};
        return child;
    }

    BSONObj buildFilter(int numPredicates, bool includeIndex) {
        BSONObjBuilder builder;
        if (includeIndex) {
            builder.append("x1", BSON("$lte" << kIndexUpperBound));
        }
        for (int i = 2; i <= numPredicates; ++i) {
            builder.append("x" + std::to_string(i), BSON("$lte" << 1));
        }
        return builder.obj();
    }

private:
    void _doTest() override {}

    void setUp() final {
        CatalogTestFixture::setUp();
        QueryFCVEnvironmentForTest::setUp();

        OperationContext* opCtx = operationContext();
        ASSERT_OK(storageInterface()->createCollection(opCtx, kNss, CollectionOptions{}));
    }

    void tearDown() final {
        CatalogTestFixture::tearDown();
    }

    IndexEntry createIndexEntry(BSONObj keyPattern, std::string indexName) {
        return IndexEntry(keyPattern,
                          IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                          IndexConfig::kLatestIndexVersion,
                          false /*multikey*/,
                          {} /*mutikeyPaths*/,
                          {} /*multikeyPathSet*/,
                          false /*sparse*/,
                          false /*unique*/,
                          IndexEntry::Identifier{indexName},
                          nullptr /*filterExpr*/,
                          BSONObj() /*infoObj*/,
                          nullptr /*collatorInterface*/,
                          nullptr /*wildcardProjection*/);
    }
};

void BM_Simple(benchmark::State& state) {
    SbeStageBuilderBenchmark fixture;
    auto opCtx = fixture.operationContext();

    // Create collection and add index1, as that is the winning index.
    auto acquisition = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, kNss, AcquisitionPrerequisites::kWrite),
        MODE_X);
    CollectionWriter coll(opCtx, &acquisition);

    WriteUnitOfWork wunit(opCtx);
    auto indexCatalog = coll.getWritableCollection(opCtx)->getIndexCatalog();
    ASSERT_OK(indexCatalog
                  ->createIndexOnEmptyCollection(opCtx,
                                                 coll.getWritableCollection(opCtx),
                                                 BSON("v" << IndexConfig::kLatestIndexVersion
                                                          << "key" << BSON("x1" << 1) << "name"
                                                          << "index1"))
                  .getStatus());
    wunit.commit();

    MultipleCollectionAccessor collections{acquireCollectionMaybeLockFree(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, kNss, AcquisitionPrerequisites::kRead))};

    // Create CanonicalQuery
    auto findCommand = std::make_unique<FindCommandRequest>(kNss);
    findCommand->setFilter(fixture.buildFilter(state.range(0), true));
    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)},
    });
    cq->setSbeCompatible(true);

    // Create QuerySolution
    auto child = fixture.makeIndexScanNode("x1", "index1", -INFINITY, kIndexUpperBound);
    auto root = std::make_unique<FetchNode>(std::move(child));

    auto bsonObj = fixture.buildFilter(state.range(0), false);
    root->filter = std::move(
        MatchExpressionParser::parse(bsonObj, make_intrusive<ExpressionContextForTest>(opCtx, kNss))
            .getValue());
    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::move(root));
    auto yieldPolicy = PlanYieldPolicySBE::make(opCtx,
                                                PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                &opCtx->fastClockSource(),
                                                0,
                                                Milliseconds::zero(),
                                                PlanYieldPolicy::YieldThroughAcquisitions{});

    for (auto _ : state) {
        auto sbePlanAndData = stage_builder::buildSlotBasedExecutableTree(
            opCtx, collections, *cq, *solution, yieldPolicy.get());
    }
}

BENCHMARK(BM_Simple)->Arg(4)->Arg(16)->Arg(64);

}  // namespace mongo
