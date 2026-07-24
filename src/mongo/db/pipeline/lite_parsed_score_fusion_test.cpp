// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/lite_parsed_score_fusion.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_score_fusion.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class LiteParsedScoreFusionTest : public service_context_test::WithSetupTransportLayer,
                                  public AggregationContextFixture {
protected:
    std::unique_ptr<Pipeline> makePipelineFromStages(const std::vector<BSONObj>& pipeline) {
        return pipeline_factory::makePipeline(
            pipeline,
            getExpCtx(),
            pipeline_factory::MakePipelineOptions{.attachCursorSource = false});
    }

private:
    unittest::ServerParameterGuard featureFlagController{"featureFlagSearchHybridScoringFull",
                                                         true};
    unittest::ServerParameterGuard _ifrFlagController{"featureFlagExtensionsInsideHybridSearch",
                                                      true};
};

TEST_F(LiteParsedScoreFusionTest, ErrorsIfNoInputsField) {
    std::vector<BSONObj> pipeline = {fromjson(R"({
        $scoreFusion: {}
    })")};

    ASSERT_THROWS_CODE(
        makePipelineFromStages(pipeline), AssertionException, ErrorCodes::IDLFailedToParse);
}

TEST_F(LiteParsedScoreFusionTest, ErrorsIfNoNestedObject) {
    std::vector<BSONObj> pipeline = {fromjson(R"({
        $scoreFusion: "not_an_object"
    })")};

    ASSERT_THROWS_CODE(
        makePipelineFromStages(pipeline), AssertionException, ErrorCodes::FailedToParse);
}

TEST_F(LiteParsedScoreFusionTest, ErrorsIfUnknownField) {
    std::vector<BSONObj> pipeline = {fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: { p: [{ $sort: { x: 1 } }, { $score: {} }] },
                normalization: "none"
            },
            unknown: "bad field"
        }
    })")};

    ASSERT_THROWS_CODE(
        makePipelineFromStages(pipeline), AssertionException, ErrorCodes::IDLUnknownField);
}

TEST_F(LiteParsedScoreFusionTest, ErrorsIfInputsIsNotObject) {
    std::vector<BSONObj> pipeline = {fromjson(R"({
        $scoreFusion: {
            input: "not an object"
        }
    })")};

    ASSERT_THROWS_CODE(
        makePipelineFromStages(pipeline), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(LiteParsedScoreFusionTest, ErrorsIfPipelineNameEmpty) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    "": [{ $sort: { x: 1 } }, { $score: {} }],
                    valid: [{ $sort: { y: 1 } }, { $score: {} }]
                },
                normalization: "none"
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedScoreFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(
        liteParsed->validate(getExpCtx()->getOperationContext()), AssertionException, 15998);
}

TEST_F(LiteParsedScoreFusionTest, ErrorsIfPipelineNameStartsWithDollar) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    $invalid: [{ $sort: { x: 1 } }, { $score: {} }]
                },
                normalization: "none"
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedScoreFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(
        liteParsed->validate(getExpCtx()->getOperationContext()), AssertionException, 16410);
}

TEST_F(LiteParsedScoreFusionTest, ErrorsIfPipelineNameContainsDot) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    "invalid.name": [{ $sort: { x: 1 } }, { $score: {} }]
                },
                normalization: "none"
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedScoreFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(
        liteParsed->validate(getExpCtx()->getOperationContext()), AssertionException, 16412);
}

TEST_F(LiteParsedScoreFusionTest, ErrorsIfDuplicatePipelineNames) {
    auto spec = BSON(
        "$scoreFusion" << BSON(
            "input" << BSON("pipelines" << BSON("dup" << BSON_ARRAY(BSON("$sort" << BSON("x" << 1))
                                                                    << BSON("$score" << BSONObj()))
                                                      << "dup"
                                                      << BSON_ARRAY(BSON("$sort" << BSON("y" << 1))
                                                                    << BSON("$score" << BSONObj())))
                                        << "normalization"
                                        << "none")));

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedScoreFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(
        liteParsed->validate(getExpCtx()->getOperationContext()), AssertionException, 12108715);
}

TEST_F(LiteParsedScoreFusionTest, ErrorsIfScoreFusionNotFirstStage) {
    auto nss = getExpCtx()->getNamespaceString();
    std::vector<BSONObj> pipeline = {BSON("$match" << BSON("x" << 1)), fromjson(R"({
            $scoreFusion: {
                input: {
                    pipelines: {
                        p: [{ $sort: { x: 1 } }, { $score: {} }]
                    },
                    normalization: "none"
                }
            }
        })")};

    LiteParsedPipeline lpp(nss, pipeline);
    ASSERT_THROWS_CODE(lpp.validate(nullptr, false), AssertionException, 10170100);
}

// =====================================================================
// End-to-end success test.
// =====================================================================

TEST_F(LiteParsedScoreFusionTest, SucceedsWithValidScoredSelectionPipeline) {
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{getExpCtx()->getNamespaceString(),
                              {getExpCtx()->getNamespaceString(), std::vector<BSONObj>()}}});

    std::vector<BSONObj> pipeline = {fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    byX: [{ $sort: { x: 1 } }, { $score: { score: "expression" } }]
                },
                normalization: "none"
            }
        }
    })")};

    auto result = makePipelineFromStages(pipeline);
    ASSERT(result != nullptr);
    ASSERT_GT(result->getSources().size(), 0U);
}

TEST_F(LiteParsedScoreFusionTest, ValidateSucceedsWithValidScoredPipeline) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    p1: [{$sort: {x: 1}}, {$score: {}}]
                },
                normalization: "none"
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedScoreFusion::parse(nss, spec.firstElement(), {});
    liteParsed->validate(getExpCtx()->getOperationContext());  // Should not throw.
}

TEST_F(LiteParsedScoreFusionTest, ValidateThrowsOnEmptySubpipeline) {
    auto nss = getExpCtx()->getNamespaceString();
    auto spec = BSON("$scoreFusion" << BSON("input" << BSON("pipelines" << BSON("p1" << BSONArray())
                                                                        << "normalization"
                                                                        << "none")));
    auto liteParsed = LiteParsedScoreFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(
        liteParsed->validate(getExpCtx()->getOperationContext()), AssertionException, 12108710);
}

TEST_F(LiteParsedScoreFusionTest, ValidateThrowsOnNonScoredPipeline) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    p1: [{$match: {x: 1}}]
                },
                normalization: "none"
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedScoreFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(
        liteParsed->validate(getExpCtx()->getOperationContext()), AssertionException, 12108712);
}

TEST_F(LiteParsedScoreFusionTest, ValidateThrowsOnNonSelectionStage) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    p1: [{$sort: {x: 1}}, {$score: {}}, {$addFields: {y: 1}}]
                },
                normalization: "none"
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedScoreFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(
        liteParsed->validate(getExpCtx()->getOperationContext()), AssertionException, 12108713);
}

TEST_F(LiteParsedScoreFusionTest, ValidateThrowsOnNestedHybridSearch) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    p1: [{$scoreFusion: {input: {pipelines: {inner: [{$sort: {x: 1}}, {$score: {}}]}, normalization: "none"}}}]
                },
                normalization: "none"
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedScoreFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(
        liteParsed->validate(getExpCtx()->getOperationContext()), AssertionException, 12108711);
}

TEST_F(LiteParsedScoreFusionTest, ValidateSucceedsWithMultipleValidPipelines) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    byX: [{$sort: {x: 1}}, {$score: {}}],
                    byY: [{$sort: {y: -1}}, {$score: {}}]
                },
                normalization: "none"
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = LiteParsedScoreFusion::parse(nss, spec.firstElement(), {});
    liteParsed->validate(getExpCtx()->getOperationContext());  // Should not throw.
}

}  // namespace
}  // namespace mongo
