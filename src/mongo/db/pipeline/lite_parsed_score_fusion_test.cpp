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

#include "mongo/db/pipeline/lite_parsed_score_fusion.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_score_fusion.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/idl/server_parameter_test_controller.h"
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
    RAIIServerParameterControllerForTest featureFlagController{"featureFlagSearchHybridScoringFull",
                                                               true};
    RAIIServerParameterControllerForTest _ifrFlagController{
        "featureFlagExtensionsInsideHybridSearch", true};
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
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 15998);
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
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 16410);
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
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 16412);
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
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108715);
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
    liteParsed->validate();  // Should not throw.
}

TEST_F(LiteParsedScoreFusionTest, ValidateThrowsOnEmptySubpipeline) {
    auto nss = getExpCtx()->getNamespaceString();
    auto spec = BSON("$scoreFusion" << BSON("input" << BSON("pipelines" << BSON("p1" << BSONArray())
                                                                        << "normalization"
                                                                        << "none")));
    auto liteParsed = LiteParsedScoreFusion::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108710);
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
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108712);
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
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108713);
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
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108711);
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
    liteParsed->validate();  // Should not throw.
}

}  // namespace
}  // namespace mongo
