// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/pipeline_d.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_geo_near_cursor.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sample_from_random_cursor.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/shard_role_transaction_resources_stasher_for_pipeline.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.collection");

class PipelineDGroupAnnotationTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _expCtx = make_intrusive<ExpressionContextForTest>(operationContext(), kNss);
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions()));
        createIndex(BSON("loc" << "2dsphere"), "loc_2dsphere");
        insertDocuments(200);
    }

    void tearDown() override {
        _expCtx.reset();
        CatalogTestFixture::tearDown();
    }

    void createIndex(const BSONObj& keyPattern, const std::string& name) {
        auto acquisition =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), kNss, AcquisitionPrerequisites::kWrite),
                              MODE_X);
        CollectionWriter coll(operationContext(), &acquisition);
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(
            coll.getWritableCollection(operationContext())
                ->getIndexCatalog()
                ->createIndexOnEmptyCollection(operationContext(),
                                               coll.getWritableCollection(operationContext()),
                                               BSON("v" << IndexConfig::kLatestIndexVersion << "key"
                                                        << keyPattern << "name" << name))
                .getStatus());
        wuow.commit();
    }

    void insertDocuments(int count) {
        std::vector<BSONObj> docs;
        for (int i = 0; i < count; ++i) {
            docs.push_back(BSON("_id" << i << "a" << i));
        }
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), kNss, AcquisitionPrerequisites::OperationType::kWrite),
            MODE_IX);
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(Helpers::insert(operationContext(), coll.getCollectionPtr(), docs));
        wuow.commit();
    }

    std::unique_ptr<Pipeline> makeOptimizedPipeline(const std::string& firstStage) {
        DocumentSourceContainer sources;
        for (auto&& stage : {fromjson(firstStage),
                             fromjson("{$set: {k: 1}}"),
                             fromjson("{$group: {_id: '$k', n: {$sum: 1}}}")}) {
            sources.push_back(DocumentSource::parse(_expCtx, stage).front());
        }
        auto pipeline = Pipeline::create(sources, _expCtx);
        pipeline_optimization::optimizePipeline(*pipeline);
        return pipeline;
    }

    void attachExecutor(Pipeline& pipeline) {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), kNss, AcquisitionPrerequisites::OperationType::kRead),
            MODE_IS);
        auto colls = MultipleCollectionAccessor(
            std::move(coll), {}, /*isAnySecondaryNamespaceAViewOrNotFullyLocal=*/false);
        auto request = AggregateCommandRequest(kNss, pipeline.serializeToBson());
        auto stasher = make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
        PipelineD::buildAndAttachInnerQueryExecutorAndBindCatalogInfoToPipeline(
            colls, kNss, &request, &pipeline, stasher);
        stashTransactionResourcesFromOperationContext(operationContext(), stasher.get());
    }

    static DocumentSourceGroup& lastGroup(Pipeline& pipeline) {
        return checked_cast<DocumentSourceGroup&>(*pipeline.getSources().back());
    }

    static bool hasStage(const Pipeline& pipeline, std::string_view name) {
        return std::ranges::any_of(pipeline.getSources(), [&](const auto& stage) {
            return stage->getSourceName() == name;
        });
    }

    struct AttachedClone {
        std::unique_ptr<Pipeline> pipeline;
        boost::intrusive_ptr<DocumentSourceGroup> group;
    };

    AttachedClone attachClone(const std::string& firstStage) {
        auto pipeline = makeOptimizedPipeline(firstStage);
        ASSERT_EQ(lastGroup(*pipeline).getNonArrayIdFields(), OrderedPathSet({"_id"}));

        auto clone = pipeline->clone();
        boost::intrusive_ptr<DocumentSourceGroup> group = &lastGroup(*clone);
        ASSERT_EQ(group->getNonArrayIdFields(), OrderedPathSet{});

        attachExecutor(*clone);
        return {std::move(clone), std::move(group)};
    }

    boost::intrusive_ptr<ExpressionContext> _expCtx;

private:
    unittest::ServerParameterGuard _featureFlag{"featureFlagStreamingGroupEligibilityCounter",
                                                true};
};

TEST_F(PipelineDGroupAnnotationTest, GenericPathRecomputesAnnotationOfClonedGroup) {
    auto [pipeline, group] = attachClone("{$sample: {size: 100}}");
    ASSERT_TRUE(hasStage(*pipeline, DocumentSourceSample::kStageName));
    ASSERT_EQ(group->getNonArrayIdFields(), OrderedPathSet({"_id"}));
}

TEST_F(PipelineDGroupAnnotationTest, SamplePathLeavesClonedGroupUnannotated) {
    auto [pipeline, group] = attachClone("{$sample: {size: 1}}");
    ASSERT_TRUE(hasStage(*pipeline, DocumentSourceSampleFromRandomCursor::kStageName));
    ASSERT_EQ(group->getNonArrayIdFields(), OrderedPathSet{});
}

TEST_F(PipelineDGroupAnnotationTest, GeoNearPathLeavesClonedGroupUnannotated) {
    auto [pipeline, group] = attachClone(
        "{$geoNear: {near: {type: 'Point', coordinates: [0, 0]}, distanceField: 'd', key: 'loc'}}");
    ASSERT_TRUE(hasStage(*pipeline, DocumentSourceGeoNearCursor::kStageName));
    ASSERT_EQ(group->getNonArrayIdFields(), OrderedPathSet{});
}

using PipelineDTest = AggregationContextFixture;

TEST_F(PipelineDTest, PipelineConsumesAllOrderInsensitive) {
    const auto noAccsGroup = fromjson("{$group: {_id: null}}");
    struct TestCase {
        std::vector<BSONObj> pipeline;
        bool expected;
    };
    std::vector<TestCase> testCases{
        TestCase{{}, false},
        // An order-insensitive blocker is required.
        TestCase{{fromjson("{$match: {a: 1}}")}, false},
        TestCase{{noAccsGroup}, true},
        TestCase{{fromjson("{$_internalStreamingGroup: {_id: '$key', total: {$sum: '$value'}, "
                           "$monotonicIdFields: ['_id']}}")},
                 false},
        TestCase{{fromjson("{$sort: {b: 1}}")}, true},
        // Single-input stages can precede an order-insensitive blocker.
        TestCase{{fromjson("{$match: {a: {$gte: 0}}}"),
                  fromjson("{$project: {a: 1}}"),
                  fromjson("{$unwind: '$a'}"),
                  noAccsGroup},
                 true},
        // Eligibility is determined by stage constraints.
        TestCase{{fromjson("{$lookup: {from: 'other', localField: 'a', "
                           "foreignField: 'b', as: 'matches'}}"),
                  noAccsGroup},
                 true},
        // A position requirement disqualifies even a single-input stage.
        TestCase{{fromjson("{$geoNear: {near: [0, 0], distanceField: 'distance'}}"), noAccsGroup},
                 false},
        // A stage that does not depend on a single input prevents reaching a later blocker.
        TestCase{{fromjson("{$limit: 1}"), noAccsGroup}, false},
        // A later sort cannot make an earlier order-sensitive group safe.
        TestCase{{fromjson("{$group: {_id: null, values: {$push: '$_id'}}}"),
                  fromjson("{$sort: {_id: 1}}")},
                 false},
        // Blocking sort in front of an ineligible group ($push is not commutative).
        TestCase{
            {fromjson("{$sort: {_id: 1}}"), fromjson("{$group: {_id: null, v: {$push: '$_id'}}}")},
            true},
    };
    for (const auto& testCase : testCases) {
        auto pipeline = pipeline_factory::makePipeline(
            testCase.pipeline, getExpCtx(), pipeline_factory::kOptionsMinimal);
        ASSERT_EQ(PipelineD::pipelineConsumesAllOrderInsensitive(*pipeline), testCase.expected)
            << BSON("pipeline" << testCase.pipeline);
    }
}

}  // namespace
}  // namespace mongo
