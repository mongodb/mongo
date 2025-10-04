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
#include "mongo/db/pipeline/document_source_score.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

/**
 * This test fixture will provide tests with an ExpressionContext (among other things like
 * OperationContext, etc.) and configure the common feature flags that we need.
 */
class DocumentSourceScoreTest : service_context_test::WithSetupTransportLayer,
                                public AggregationContextFixture {
private:
    RAIIServerParameterControllerForTest scoreFusionFlag{"featureFlagSearchHybridScoringFull",
                                                         true};
    // Feature flag needed to use 'score' meta field
    RAIIServerParameterControllerForTest rankFusionFlag{"featureFlagRankFusionFull", true};
};

TEST_F(DocumentSourceScoreTest, ErrorsIfNoScoreField) {
    auto spec = fromjson(R"({
        $score: {
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceScoreTest, CheckNoOptionalArgsIncluded) {
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore"
        }
    })");

    Document inputDoc = Document{{"myScore", 5}};
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());

    const auto desugaredList =
        DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    // Default normalization is none.
    ASSERT_EQ(desugaredList.size(), 4);
    boost::intrusive_ptr<DocumentSource> ds = *desugaredList.begin();
    auto stage = exec::agg::buildStage(ds);
    ASSERT_DOES_NOT_THROW(stage->setSource(mock.get()));
}

TEST_F(DocumentSourceScoreTest, CheckAllOptionalArgsIncluded) {
    auto spec = fromjson(R"({
         $score: {
             score: "expression",
             normalization: "none",
             weight: 1.0
         }
     })");

    ASSERT_DOES_NOT_THROW(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreTest, CheckOnlynormalizationSpecified) {
    auto spec = fromjson(R"({
         $score: {
             score: "expression",
             normalization: "none"
         }
     })");

    ASSERT_DOES_NOT_THROW(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreTest, CheckOnlyWeightSpecified) {
    auto spec = fromjson(R"({
         $score: {
             score: "$myScore",
             weight: 1.0
         }
     })");

    Document inputDoc = Document{{"myScore", 5}};
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());

    const auto desugaredList =
        DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    // Default normalization is none.
    ASSERT_EQ(desugaredList.size(), 4);
    boost::intrusive_ptr<DocumentSource> ds = *desugaredList.begin();
    auto stage = exec::agg::buildStage(ds);
    ASSERT_DOES_NOT_THROW(stage->setSource(mock.get()));
}

TEST_F(DocumentSourceScoreTest, ErrorsIfWrongnormalizationType) {
    auto spec = fromjson(R"({
         $score: {
             score: "expression",
             normalization: 1.0
         }
     })");

    ASSERT_THROWS_CODE(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreTest, ErrorsIfWrongWeightType) {
    auto spec = fromjson(R"({
         $score: {
             score: "expression",
             weight: "1.0"
         }
     })");

    ASSERT_THROWS_CODE(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreTest, CheckIntScoreMetadataUpdated) {
    auto spec = fromjson(R"({
         $score: {
             score: "$myScore",
             normalization: "none",
             weight: 1.0
         }
     })");
    Document inputDoc = Document{{"myScore", 5}};

    const auto desugaredList =
        DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_EQ(desugaredList.size(), 4);
    boost::intrusive_ptr<DocumentSource> docSourceScore = *desugaredList.begin();
    auto stage = exec::agg::buildStage(docSourceScore);
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    stage->setSource(mock.get());

    auto next = stage->getNext();
    ASSERT(next.isAdvanced());

    // Assert inputDoc's metadata equals 5.1
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), 5);
}

TEST_F(DocumentSourceScoreTest, CheckDoubleScoreMetadataUpdated) {
    auto spec = fromjson(R"({
         $score: {
             score: "$myScore",
             normalization: "none",
             weight: 1.0
         }
     })");
    Document inputDoc = Document{{"myScore", 5.1}};

    const auto desugaredList =
        DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_EQ(desugaredList.size(), 4);
    boost::intrusive_ptr<DocumentSource> docSourceScore = *desugaredList.begin();
    auto stage = exec::agg::buildStage(docSourceScore);
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    stage->setSource(mock.get());

    auto next = stage->getNext();
    ASSERT(next.isAdvanced());

    // Assert inputDoc's metadata equals 5.1
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), 5.1);
}

TEST_F(DocumentSourceScoreTest, CheckLengthyDocScoreMetadataUpdated) {
    auto spec = fromjson(R"({
          $score: {
              score: "$myScore",
              normalization: "none"
          }
      })");
    Document inputDoc =
        Document{{"field1", "hello"_sd}, {"field2", 10}, {"myScore", 5.3}, {"field3", true}};

    const auto desugaredList =
        DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_EQ(desugaredList.size(), 4);
    boost::intrusive_ptr<DocumentSource> docSourceScore = *desugaredList.begin();
    auto stage = exec::agg::buildStage(docSourceScore);
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    stage->setSource(mock.get());

    auto next = stage->getNext();
    ASSERT(next.isAdvanced());

    // Assert inputDoc's metadata equals 5.1
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), 5.3);
}

TEST_F(DocumentSourceScoreTest, ErrorsIfScoreNotDouble) {
    auto spec = fromjson(R"({
          $score: {
              score: "$myScore",
              normalization: "none"
          }
      })");
    Document inputDoc =
        Document{{"field1", "hello"_sd}, {"field2", 10}, {"myScore", "5.3"_sd}, {"field3", true}};

    const auto desugaredList =
        DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_EQ(desugaredList.size(), 4);
    boost::intrusive_ptr<DocumentSource> docSourceScore = *desugaredList.begin();
    auto stage = exec::agg::buildStage(docSourceScore);
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    stage->setSource(mock.get());

    // Assert cannot evaluate expression into double
    ASSERT_THROWS_CODE(stage->getNext(), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreTest, ErrorsIfExpressionFieldPathDoesNotExist) {
    auto spec = fromjson(R"({
          $score: {
              score: "$myScore",
              normalization: "none"
          }
      })");
    Document inputDoc = Document{{"field1", "hello"_sd}, {"field2", 10}, {"field3", true}};

    const auto desugaredList =
        DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_EQ(desugaredList.size(), 4);
    boost::intrusive_ptr<DocumentSource> docSourceScore = *desugaredList.begin();
    auto stage = exec::agg::buildStage(docSourceScore);
    auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    stage->setSource(mock.get());

    // Assert cannot evaluate expression into double
    ASSERT_THROWS_CODE(stage->getNext(), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreTest, ErrorsIfScoreInvalidExpression) {
    auto spec = fromjson(R"({
          $score: {
              score: { $ad: ['$myScore', '$otherScore'] },
              normalization: "none"
          }
      })");
    Document inputDoc =
        Document{{"field1", "hello"_sd}, {"otherScore", 10}, {"myScore", 5.3}, {"field3", true}};

    // Assert cannot parse expression
    ASSERT_THROWS_CODE(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidPipelineOperator);
}

TEST_F(DocumentSourceScoreTest, ChecksScoreMetadatUpdatedValidExpression) {
    auto spec = fromjson(R"({
          $score: {
              score: { $add: ['$myScore', '$otherScore'] },
              normalization: "none"
          }
      })");
    Document inputDoc =
        Document{{"field1", "hello"_sd}, {"otherScore", 10}, {"myScore", 5.3}, {"field3", true}};

    const auto desugaredList =
        DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_EQ(desugaredList.size(), 4);
}

TEST_F(DocumentSourceScoreTest, ErrorsNormFuncSigmoidInvalidWeight) {
    auto spec = fromjson(R"({
          $score: {
              score: "$myScore",
              normalization: "sigmoid",
              weight: -0.5
          }
      })");

    ASSERT_THROWS_CODE(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceScoreTest, ErrorsInvalidWeight) {
    auto spec = fromjson(R"({
          $score: {
              score: "$myScore",
              weight: 1.5
          }
      })");

    ASSERT_THROWS_CODE(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceScoreTest, ErrorsInvalidnormalization) {
    auto spec = fromjson(R"({
          $score: {
              score: "$myScore",
              normalization: "Sigmoid",
              weight: 0.5
          }
      })");

    ASSERT_THROWS_CODE(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

void runRepresentativeQueryShapeTest(boost::intrusive_ptr<ExpressionContextForTest> expCtx,
                                     const BSONObj& querySpec,
                                     const std::string& expectedDesugar) {
    const auto desugaredList =
        DocumentSourceScore::createFromBson(querySpec.firstElement(), expCtx);
    const auto pipeline = Pipeline::create(desugaredList, expCtx);
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());

    ASSERT_BSONOBJ_EQ_AUTO(expectedDesugar, asOneObj);
}

TEST_F(DocumentSourceScoreTest, RepresentativeQueryShapeExpressionNoNormalization) {
    auto spec = fromjson(R"({
        $score: {
            score: {$multiply: ["$myScore", "$myScore"]},
            normalization: "none"
        }
    })");

    auto expected = R"({
        "expectedStages": [
        {
            "$setMetadata": {
                "score": {
                    "$multiply": [
                        "$myScore",
                        "$myScore"
                    ]
                }
            }
        },
        {
            "$replaceRoot": {
                "newRoot": {
                    "docs": "$$ROOT"
                }
            }
        },
        {
            "$addFields": {
                "internal_raw_score": {
                    "$meta": "score"
                }
            }
        },
        {
            "$replaceRoot": {
                "newRoot": "$docs"
            }
        }
    ]})";

    runRepresentativeQueryShapeTest(getExpCtx(), spec, expected);
}

TEST_F(DocumentSourceScoreTest, RepresentativeQueryShapeNoNormalizationUnweighted) {
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalization: "none"
        }
    })");

    auto expected = R"({
        "expectedStages": [
        {
            "$setMetadata": {
                "score": "$myScore"
            }
        },
        {
            "$replaceRoot": {
                "newRoot": {
                    "docs": "$$ROOT"
                }
            }
        },
        {
            "$addFields": {
                "internal_raw_score": {
                    "$meta": "score"
                }
            }
        },
        {
            "$replaceRoot": {
                "newRoot": "$docs"
            }
        }
    ]})";

    runRepresentativeQueryShapeTest(getExpCtx(), spec, expected);
}

TEST_F(DocumentSourceScoreTest, RepresentativeQueryShapeNoNormalizationWeighted) {
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalization: "none",
            weight: 0.5
        }
    })");

    auto expected = R"({
        "expectedStages": [
        {
            "$setMetadata": {
                "score": "$myScore"
            }
        },
        {
            "$replaceRoot": {
                "newRoot": {
                    "docs": "$$ROOT"
                }
            }
        },
        {
            "$addFields": {
                "internal_raw_score": {
                    "$meta": "score"
                }
            }
        },
        {
            "$setMetadata": {
                "score": {
                    "$multiply": [
                        {"$meta": "score"},
                        {"$const": 0.5}
                    ]
                }
            }
        },
        {
            "$replaceRoot": {
                "newRoot": "$docs"
            }
        }
    ]})";

    runRepresentativeQueryShapeTest(getExpCtx(), spec, expected);
}

TEST_F(DocumentSourceScoreTest, RepresentativeQueryShapeSigmoidNormalization) {
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalization: "sigmoid"
        }
    })");

    auto expected = R"({
        "expectedStages": [
        {
            "$setMetadata": {
                "score": "$myScore"
            }
        },
        {
            "$replaceRoot": {
                "newRoot": {
                    "docs": "$$ROOT"
                }
            }
        },
        {
            "$addFields": {
                "internal_raw_score": {
                    "$meta": "score"
                }
            }
        },
        {
            "$setMetadata": {
                "score": {
                    "$divide": [
                        {"$const": 1},
                        {
                            "$add": [
                                {"$const": 1},
                                {
                                    "$exp": [
                                        {
                                            "$multiply": [
                                                {"$const": -1},
                                                {"$meta": "score"}
                                            ]
                                        }
                                    ]
                                }
                            ]
                        }
                    ]
                }
            }
        },
        {
            "$replaceRoot": {
                "newRoot": "$docs"
            }
        }
    ]})";

    runRepresentativeQueryShapeTest(getExpCtx(), spec, expected);
}

TEST_F(DocumentSourceScoreTest, RepresentativeQueryShapeSigmoidNormalizationWeighted) {
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalization: "sigmoid",
            weight: 0.5
        }
    })");

    auto expected = R"({
        "expectedStages": [
        {
            "$setMetadata": {
                "score": "$myScore"
            }
        },
        {
            "$replaceRoot": {
                "newRoot": {
                    "docs": "$$ROOT"
                }
            }
        },
        {
            "$addFields": {
                "internal_raw_score": {
                    "$meta": "score"
                }
            }
        },
        {
            "$setMetadata": {
                "score": {
                    "$divide": [
                        {"$const": 1},
                        {
                            "$add": [
                                {"$const": 1},
                                {
                                    "$exp": [
                                        {
                                            "$multiply": [
                                                {"$const": -1},
                                                {"$meta": "score"}
                                            ]
                                        }
                                    ]
                                }
                            ]
                        }
                    ]
                }
            }
        },
        {
            "$setMetadata": {
                "score": {
                    "$multiply": [
                        {"$meta": "score"},
                        {"$const": 0.5}
                    ]
                }
            }
        },
        {
            "$replaceRoot": {
                "newRoot": "$docs"
            }
        }
    ]})";

    runRepresentativeQueryShapeTest(getExpCtx(), spec, expected);
}

TEST_F(DocumentSourceScoreTest, RepresentativeQueryShapeExpressionSigmoidNormalization) {
    auto spec = fromjson(R"({
        $score: {
            score: {$multiply: ["$myScore", "$myScore"]},
            normalization: "sigmoid"
        }
    })");

    auto expected = R"({
        "expectedStages": [
        {
            "$setMetadata": {
                "score": {
                    "$multiply": [
                        "$myScore",
                        "$myScore"
                    ]
                }
            }
        },
        {
            "$replaceRoot": {
                "newRoot": {
                    "docs": "$$ROOT"
                }
            }
        },
        {
            "$addFields": {
                "internal_raw_score": {
                    "$meta": "score"
                }
            }
        },
        {
            "$setMetadata": {
                "score": {
                    "$divide": [
                        {"$const": 1},
                        {
                            "$add": [
                                {"$const": 1},
                                {
                                    "$exp": [
                                        {
                                            "$multiply": [
                                                {"$const": -1},
                                                {"$meta": "score"}
                                            ]
                                        }
                                    ]
                                }
                            ]
                        }
                    ]
                }
            }
        },
        {
            "$replaceRoot": {
                "newRoot": "$docs"
            }
        }
    ]})";

    runRepresentativeQueryShapeTest(getExpCtx(), spec, expected);
}

TEST_F(DocumentSourceScoreTest, RepresentativeQueryShapeMinMaxScalerNormalization) {
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalization: "minMaxScaler"
        }
    })");

    auto expected = R"({
        "expectedStages": [
        {
            "$setMetadata": {
                "score": "$myScore"
            }
        },
        {
            "$replaceRoot": {
                "newRoot": {
                    "docs": "$$ROOT"
                }
            }
        },
        {
            "$addFields": {
                "internal_raw_score": {
                    "$meta": "score"
                }
            }
        },
        {
            "$_internalSetWindowFields": {
                "sortBy": {"internal_min_max_scaler_normalization_score": -1},
                "output": {
                    "internal_min_max_scaler_normalization_score": {
                        "$minMaxScaler": {
                            "input": {"$meta": "score"},
                            "min": 0,
                            "max": 1
                        },
                        "window": {
                            "documents": [
                                "unbounded",
                                "unbounded"
                            ]
                        }
                    }
                }
            }
        },
        {
            "$setMetadata": {
                "score": "$internal_min_max_scaler_normalization_score"
            }
        },
        {
            "$replaceRoot": {
                "newRoot": "$docs"
            }
        }
    ]})";

    runRepresentativeQueryShapeTest(getExpCtx(), spec, expected);
}

TEST_F(DocumentSourceScoreTest, RepresentativeQueryShapeMinMaxScalerNormalizationWeighted) {
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalization: "minMaxScaler",
            weight: 0.5
        }
    })");

    auto expected = R"({
        "expectedStages": [
        {
            "$setMetadata": {
                "score": "$myScore"
            }
        },
        {
            "$replaceRoot": {
                "newRoot": {
                    "docs": "$$ROOT"
                }
            }
        },
        {
            "$addFields": {
                "internal_raw_score": {
                    "$meta": "score"
                }
            }
        },
        {
            "$_internalSetWindowFields": {
                "sortBy": {"internal_min_max_scaler_normalization_score": -1},
                "output": {
                    "internal_min_max_scaler_normalization_score": {
                        "$minMaxScaler": {
                            "input": {"$meta": "score"},
                            "min": 0,
                            "max": 1
                        },
                        "window": {
                            "documents": [
                                "unbounded",
                                "unbounded"
                            ]
                        }
                    }
                }
            }
        },
        {
            "$setMetadata": {
                "score": "$internal_min_max_scaler_normalization_score"
            }
        },
        {
            "$setMetadata": {
                "score": {
                    "$multiply": [
                        {"$meta": "score"},
                        {"$const": 0.5}
                    ]
                }
            }
        },
        {
            "$replaceRoot": {
                "newRoot": "$docs"
            }
        }
    ]})";

    runRepresentativeQueryShapeTest(getExpCtx(), spec, expected);
}

TEST_F(DocumentSourceScoreTest, RepresentativeQueryShapeExpressionMinMaxScalerNormalization) {
    auto spec = fromjson(R"({
        $score: {
            score: {$multiply: ["$myScore", "$myScore"]},
            normalization: "minMaxScaler"
        }
    })");

    auto expected = R"({
        "expectedStages": [
        {
            "$setMetadata": {
                "score": {
                    "$multiply": [
                        "$myScore",
                        "$myScore"
                    ]
                }
            }
        },
        {
            "$replaceRoot": {
                "newRoot": {
                    "docs": "$$ROOT"
                }
            }
        },
        {
            "$addFields": {
                "internal_raw_score": {
                    "$meta": "score"
                }
            }
        },
        {
            "$_internalSetWindowFields": {
                "sortBy": {"internal_min_max_scaler_normalization_score": -1},
                "output": {
                    "internal_min_max_scaler_normalization_score": {
                        "$minMaxScaler": {
                            "input": {"$meta": "score"},
                            "min": 0,
                            "max": 1
                        },
                        "window": {
                            "documents": [
                                "unbounded",
                                "unbounded"
                            ]
                        }
                    }
                }
            }
        },
        {
            "$setMetadata": {
                "score": "$internal_min_max_scaler_normalization_score"
            }
        },
        {
            "$replaceRoot": {
                "newRoot": "$docs"
            }
        }
    ]})";

    runRepresentativeQueryShapeTest(getExpCtx(), spec, expected);
}

void runQueryShapeDebugStringTest(boost::intrusive_ptr<ExpressionContextForTest> expCtx,
                                  const BSONObj& querySpec,
                                  const std::vector<std::string>& expectedDesugarOutputs) {
    SerializationOptions opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;

    const auto desugaredList =
        DocumentSourceScore::createFromBson(querySpec.firstElement(), expCtx);
    std::vector<Value> output;
    for (auto it = desugaredList.begin(); it != desugaredList.end(); it++) {
        boost::intrusive_ptr<DocumentSource> ds = *it;
        ds->serializeToArray(output, opts);
    }

    ASSERT_EQ(output.size(), expectedDesugarOutputs.size());

    for (size_t i = 0; i < expectedDesugarOutputs.size(); i++) {
        ASSERT_BSONOBJ_EQ_AUTO(expectedDesugarOutputs[i], output[i].getDocument().toBson());
    }
}

TEST_F(DocumentSourceScoreTest, QueryShapeDebugStringNoNormalization) {
    BSONObj spec = fromjson("{$score: {score: \"$myScore\", normalization: \"none\"}}");
    std::vector<std::string> expectedValues = {
        R"({
            $setMetadata: {
                score: "$HASH<myScore>"
            }
        })",
        R"({
            "$replaceRoot": {
                "newRoot": {
                    "HASH<docs>": "$$ROOT"
                }
            }
        })",
        R"({
            "$addFields": {
                "HASH<internal_raw_score>": {
                    "$meta": "score"
                }
            }
        })",
        R"({
            "$replaceRoot": {
                "newRoot": "$HASH<docs>"
            }
        })",
    };

    runQueryShapeDebugStringTest(getExpCtx(), spec, expectedValues);
}

TEST_F(DocumentSourceScoreTest, QueryShapeDebugStringSigmoidNormalizationWeighted) {
    BSONObj spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalization: "sigmoid",
            weight: 0.5
        }
    })");

    std::vector<std::string> expectedValues = {
        R"({
            $setMetadata: {
                score: "$HASH<myScore>"
            }
        })",
        R"({
            "$replaceRoot": {
                "newRoot": {
                    "HASH<docs>": "$$ROOT"
                }
            }
        })",
        R"({
            "$addFields": {
                "HASH<internal_raw_score>": {
                    "$meta": "score"
                }
            }
        })",
        R"({
            "$setMetadata": {
                "score": {
                    "$divide": [
                        "?number",
                        {
                            "$add": [
                                "?number",
                                {
                                    "$exp": [
                                        {
                                            "$multiply": [
                                                "?number",
                                                {"$meta": "score"}
                                            ]
                                        }
                                    ]
                                }
                            ]
                        }
                    ]
                }
            }
        })",
        R"({
            $setMetadata: {
                "score": {
                    "$multiply": [
                        {"$meta": "score"},
                        "?number"
                    ]
                }
            }
        })",
        R"({
            "$replaceRoot": {
                "newRoot": "$HASH<docs>"
            }
        })",
    };

    runQueryShapeDebugStringTest(getExpCtx(), spec, expectedValues);
}

TEST_F(DocumentSourceScoreTest, QueryShapeDebugStringExpressionMinMaxScalerNormalizationWeighted) {
    BSONObj spec = fromjson(R"({
        $score: {
            score: {$multiply: ["$myScore", 2]},
            normalization: "minMaxScaler",
            weight: .75
        }
    })");


    std::vector<std::string> expectedValues = {
        R"({
                "$setMetadata": {
                    "score": {
                        "$multiply": [
                            "$HASH<myScore>",
                            "?number"
                        ]
                    }
                }
            })",
        R"({
                "$replaceRoot": {
                    "newRoot": {
                        "HASH<docs>": "$$ROOT"
                    }
                }
            })",
        R"({
                "$addFields": {
                    "HASH<internal_raw_score>": {
                        "$meta": "score"
                    }
                }
            })",
        R"({
                "$_internalSetWindowFields": {
                    "sortBy": {"HASH<internal_min_max_scaler_normalization_score>": -1},
                    "output": {
                        "HASH<internal_min_max_scaler_normalization_score>": {
                            "$minMaxScaler": {
                                "input": {"$meta": "score"},
                                "min": 0,
                                "max": 1
                            },
                            "window": {
                                "documents": [
                                    "unbounded",
                                    "unbounded"
                                ]
                            }
                        }
                    }
                }
            })",
        R"({
                "$setMetadata": {
                    "score": "$HASH<internal_min_max_scaler_normalization_score>"
                }
            })",
        R"({
                "$setMetadata": {
                    "score": {
                        "$multiply": [
                            {"$meta": "score"},
                            "?number"
                        ]
                    }
                }
            })",
        R"({
                "$replaceRoot": {
                    "newRoot": "$HASH<docs>"
                }
            })",
    };

    runQueryShapeDebugStringTest(getExpCtx(), spec, expectedValues);
}

TEST_F(DocumentSourceScoreTest, ScoreDetailsDesugaring) {
    {
        BSONObj spec = fromjson(
            "{$score: {score: \"$myScore\", normalization: \"none\", scoreDetails: true}}");
        const auto desugaredList =
            DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
        ASSERT_EQ(desugaredList.size(), 5);
        const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
        BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({
            "expectedStages": [
                {
                    "$setMetadata": {
                        "score": "$myScore"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the score calculated from multiplying a weight in the range [0,1] with either a normalized or nonnormalized value:"
                            },
                            "rawScore": "$internal_raw_score",
                            "normalization": {
                                "$const": "none"
                            },
                            "weight": {
                                "$const": 1
                            },
                            "expression": {
                                "$const": "{ string: '$myScore' }"
                            },
                            "details": []
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
            asOneObj);
    }
    {
        BSONObj spec = fromjson(
            R"({
                $score: {
                    score: {
                        $add: ['$myScore', '$otherScore']
                    },
                    normalization: 'sigmoid',
                    weight: 0.5,
                    scoreDetails: true
                }
            })");
        const auto desugaredList =
            DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
        ASSERT_EQ(desugaredList.size(), 7);
        const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
        BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({
            "expectedStages": [
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$myScore",
                                "$otherScore"
                            ]
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$divide": [
                                {
                                    "$const": 1
                                },
                                {
                                    "$add": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$exp": [
                                                {
                                                    "$multiply": [
                                                        {
                                                            "$const": -1
                                                        },
                                                        {
                                                            "$meta": "score"
                                                        }
                                                    ]
                                                }
                                            ]
                                        }
                                    ]
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 0.5
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the score calculated from multiplying a weight in the range [0,1] with either a normalized or nonnormalized value:"
                            },
                            "rawScore": "$internal_raw_score",
                            "normalization": {
                                "$const": "sigmoid"
                            },
                            "weight": {
                                "$const": 0.5
                            },
                            "expression": {
                                "$const": "{ string: { $add: [ '$myScore', '$otherScore' ] } }"
                            },
                            "details": []
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
            asOneObj);
    }
    {
        BSONObj spec = fromjson(
            "{$score: {score: \"$myScore\", normalization: \"minMaxScaler\", weight: 0.5, "
            "scoreDetails: true}}");
        const auto desugaredList =
            DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
        ASSERT_EQ(desugaredList.size(), 8);
        const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
        BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
        ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
            R"({
            "expectedStages": [
                {
                    "$setMetadata": {
                        "score": "$myScore"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "internal_min_max_scaler_normalization_score": -1
                        },
                        "output": {
                            "internal_min_max_scaler_normalization_score": {
                                "$minMaxScaler": {
                                    "input": {
                                        "$meta": "score"
                                    },
                                    "min": 0,
                                    "max": 1
                                },
                                "window": {
                                    "documents": [
                                        "unbounded",
                                        "unbounded"
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": "$internal_min_max_scaler_normalization_score"
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 0.5
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the score calculated from multiplying a weight in the range [0,1] with either a normalized or nonnormalized value:"
                            },
                            "rawScore": "$internal_raw_score",
                            "normalization": {
                                "$const": "minMaxScaler"
                            },
                            "weight": {
                                "$const": 0.5
                            },
                            "expression": {
                                "$const": "{ string: '$myScore' }"
                            },
                            "details": []
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
            asOneObj);
    }
}

}  // namespace
}  // namespace mongo
