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

#include "mongo/db/pipeline/lite_parsed_rank_fusion.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_rank_fusion.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class LiteParsedRankFusionTest : public service_context_test::WithSetupTransportLayer,
                                 public AggregationContextFixture {
protected:
    std::unique_ptr<Pipeline> makePipelineFromStages(const std::vector<BSONObj>& pipeline) {
        return pipeline_factory::makePipeline(
            pipeline,
            getExpCtx(),
            pipeline_factory::MakePipelineOptions{.attachCursorSource = false});
    }

private:
    RAIIServerParameterControllerForTest featureFlagController1{"featureFlagRankFusionBasic", true};
    RAIIServerParameterControllerForTest featureFlagController2{"featureFlagRankFusionFull", true};
    RAIIServerParameterControllerForTest _ifrFlagController{
        "featureFlagExtensionsInsideHybridSearch", true};
};

TEST_F(LiteParsedRankFusionTest, ErrorsIfNoInputsField) {
    std::vector<BSONObj> pipeline = {fromjson(R"({
        $rankFusion: {}
    })")};

    ASSERT_THROWS_CODE(
        makePipelineFromStages(pipeline), AssertionException, ErrorCodes::IDLFailedToParse);
}

TEST_F(LiteParsedRankFusionTest, ErrorsIfNoNestedObject) {
    std::vector<BSONObj> pipeline = {fromjson(R"({
        $rankFusion: "not_an_object"
    })")};

    ASSERT_THROWS_CODE(
        makePipelineFromStages(pipeline), AssertionException, ErrorCodes::FailedToParse);
}

TEST_F(LiteParsedRankFusionTest, ErrorsIfUnknownField) {
    std::vector<BSONObj> pipeline = {fromjson(R"({
        $rankFusion: {
            input: { pipelines: { p: [{ $match: { x: 1 } }, { $sort: { x: 1 } }] } },
            unknown: "bad field"
        }
    })")};

    ASSERT_THROWS_CODE(
        makePipelineFromStages(pipeline), AssertionException, ErrorCodes::IDLUnknownField);
}

TEST_F(LiteParsedRankFusionTest, ErrorsIfInputsIsNotObject) {
    std::vector<BSONObj> pipeline = {fromjson(R"({
        $rankFusion: {
            input: "not an object"
        }
    })")};

    ASSERT_THROWS_CODE(
        makePipelineFromStages(pipeline), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(LiteParsedRankFusionTest, ErrorsIfPipelineNameEmpty) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    "": [
                        { $match: { author: "Agatha Christie" } },
                        { $sort: { author: 1 } }
                    ],
                    valid: [
                        { $sort: { x: 1 } }
                    ]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedRankFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 15998);
}

TEST_F(LiteParsedRankFusionTest, ErrorsIfPipelineNameStartsWithDollar) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    $invalid: [
                        { $match: { author: "Agatha Christie" } },
                        { $sort: { author: 1 } }
                    ]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedRankFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 16410);
}

TEST_F(LiteParsedRankFusionTest, ErrorsIfPipelineNameContainsDot) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    "invalid.name": [
                        { $match: { author: "Agatha Christie" } },
                        { $sort: { author: 1 } }
                    ]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedRankFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 16412);
}

TEST_F(LiteParsedRankFusionTest, ValidateThrowsOnDuplicatePipelineNames) {
    // Construct BSON with duplicate pipeline names manually since fromjson deduplicates.
    auto spec =
        BSON("$rankFusion" << BSON(
                 "input" << BSON("pipelines" << BSON(
                                     "dup" << BSON_ARRAY(BSON("$sort" << BSON("x" << 1))) << "dup"
                                           << BSON_ARRAY(BSON("$sort" << BSON("y" << 1)))))));

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedRankFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108714);
}

TEST_F(LiteParsedRankFusionTest, ErrorsIfRankFusionNotFirstStage) {
    auto nss = getExpCtx()->getNamespaceString();
    std::vector<BSONObj> pipeline = {BSON("$match" << BSON("x" << 1)), fromjson(R"({
            $rankFusion: {
                input: {
                    pipelines: {
                        p: [
                            { $match: { author: "Agatha Christie" } },
                            { $sort: { author: 1 } }
                        ]
                    }
                }
            }
        })")};

    LiteParsedPipeline lpp(nss, pipeline);
    ASSERT_THROWS_CODE(lpp.validate(nullptr, false), AssertionException, 10170100);
}

TEST_F(LiteParsedRankFusionTest, ErrorsIfScoreDetailsWithoutRankFusionFullFF) {
    RAIIServerParameterControllerForTest rankFusionFullController{"featureFlagRankFusionFull",
                                                                  false};
    std::vector<BSONObj> pipeline = {fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match: { author: "Agatha Christie" } },
                        { $sort: { author: 1 } }
                    ]
                }
            },
            scoreDetails: true
        }
    })")};

    ASSERT_THROWS_CODE(
        makePipelineFromStages(pipeline), AssertionException, ErrorCodes::QueryFeatureNotAllowed);
}

TEST_F(LiteParsedRankFusionTest, SucceedsWithValidRankedSelectionPipeline) {
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{getExpCtx()->getNamespaceString(),
                              {getExpCtx()->getNamespaceString(), std::vector<BSONObj>()}}});

    std::vector<BSONObj> pipeline = {fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match: { author: "Agatha Christie" } },
                        { $sort: { author: 1 } }
                    ]
                }
            }
        }
    })")};

    auto result = makePipelineFromStages(pipeline);
    ASSERT(result != nullptr);
    ASSERT_GT(result->getSources().size(), 0U);
}

TEST_F(LiteParsedRankFusionTest, ValidateSucceedsWithValidRankedPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    p1: [{$sort: {x: 1}}]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedRankFusion::parse(nss, spec.firstElement(), {});
    liteParsed->validate();  // Should not throw.
}

TEST_F(LiteParsedRankFusionTest, ValidateThrowsOnEmptySubpipeline) {
    auto nss = getExpCtx()->getNamespaceString();
    auto spec =
        BSON("$rankFusion" << BSON("input" << BSON("pipelines" << BSON("p1" << BSONArray()))));
    auto liteParsed = LiteParsedRankFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108700);
}

TEST_F(LiteParsedRankFusionTest, ValidateThrowsOnNonRankedPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    p1: [{$match: {x: 1}}]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedRankFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108702);
}

TEST_F(LiteParsedRankFusionTest, ValidateThrowsOnNonSelectionStage) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    p1: [{$sort: {x: 1}}, {$addFields: {y: 1}}]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedRankFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108704);
}

TEST_F(LiteParsedRankFusionTest, ValidateThrowsOnNestedHybridSearch) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    p1: [{$rankFusion: {input: {pipelines: {inner: [{$sort: {x: 1}}]}}}}]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedRankFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108701);
}

TEST_F(LiteParsedRankFusionTest, ValidateThrowsOnScoreStageInPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    p1: [{$sort: {x: 1}}, {$score: {}}]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedRankFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108703);
}

TEST_F(LiteParsedRankFusionTest, ValidateSucceedsWithMultipleValidPipelines) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    byX: [{$sort: {x: 1}}],
                    byY: [{$sort: {y: -1}}]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedRankFusion::parse(nss, spec.firstElement(), {});
    liteParsed->validate();  // Should not throw.
}

}  // namespace
}  // namespace mongo
