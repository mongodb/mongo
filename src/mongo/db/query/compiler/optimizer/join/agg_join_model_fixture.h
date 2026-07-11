// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/compiler/metadata/path_arrayness.h"
#include "mongo/db/query/compiler/optimizer/join/agg_join_model.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::join_ordering {
class AggJoinModelFixture : public AggregationContextFixture {
public:
    static constexpr size_t kMaxNumberNodesConsideredForImplicitEdges = 4;

    static std::string toString(const BSONObj& bson) {
        return bson.jsonString(
            /*format*/ ExtendedCanonicalV2_0_0,
            /*pretty*/ true);
    }

    static std::string toString(const std::unique_ptr<Pipeline>& pipeline);
    static std::vector<BSONObj> pipelineFromJsonArray(std::string_view jsonArray);

    std::unique_ptr<Pipeline> makePipeline(std::vector<BSONObj> bsonStages,
                                           std::vector<std::string_view> collNames) {
        return makePipelineForTest(bsonStages, collNames, getExpCtx());
    }

    std::unique_ptr<Pipeline> makePipeline(std::string_view query,
                                           std::vector<std::string_view> collNames) {
        return makePipelineForTest(query, collNames, getExpCtx());
    }

    std::unique_ptr<Pipeline> makePipelineOfSize(size_t numJoins);

    /**
     * Marks the given fields as non-array (scalar) in the pipeline's ExpressionContext.
     * 'mainCollFields' are fields on the main collection; 'secondaryCollFieldMap' maps secondary
     * collection names to their fields.
     */
    static void markFieldsAsScalar(
        Pipeline& pipeline,
        const std::vector<std::string_view>& mainCollFields,
        const StringMap<std::vector<std::string_view>>& secondaryCollFieldMap) {
        auto expCtx = pipeline.getContext();

        auto mainPathArrayness = std::make_shared<PathArrayness>();
        for (const auto& field : mainCollFields) {
            mainPathArrayness->addPath(
                FieldPath(field), MultikeyComponents{}, /*isFullRebuild=*/true);
        }
        expCtx->setPathArraynessForNss(expCtx->getNamespaceString(), std::move(mainPathArrayness));

        for (const auto& [collName, fields] : secondaryCollFieldMap) {
            auto pathArrayness = std::make_shared<PathArrayness>();
            for (const auto& field : fields) {
                pathArrayness->addPath(
                    FieldPath(field), MultikeyComponents{}, /*isFullRebuild=*/true);
            }
            expCtx->setPathArraynessForNss(
                NamespaceString::createNamespaceString_forTest("test", collName),
                std::move(pathArrayness));
        }
    }

    const AggModelBuildParams defaultBuildParams{.maxNumberNodesConsideredForImplicitEdges =
                                                     kMaxNumberNodesConsideredForImplicitEdges};

private:
    // Ensure path arrayness is enabled for all tests.
    unittest::ServerParameterGuard queryKnobController{"featureFlagPathArrayness", true};
};
}  // namespace mongo::join_ordering
