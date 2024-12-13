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
#include "mongo/bson/json.h"
#include <boost/smart_ptr.hpp>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_score.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_dependencies.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using DocumentSourceScoreTest = AggregationContextFixture;

// Sigmoid function: 1/(1+exp(-x))
double testEvaluateSigmoid(double score) {
    return 1 / (1 + std::exp((-1 * score)));
}

TEST_F(DocumentSourceScoreTest, ErrorsIfNoScoreField) {
    RAIIServerParameterControllerForTest prerequisitesController("featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceScoreTest, CheckNoOptionalArgsIncluded) {
    RAIIServerParameterControllerForTest prerequisitesController("featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore"
        }
    })");

    Document inputDoc = Document{{"myScore", 5}};
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());

    ASSERT_DOES_NOT_THROW(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx())
                              ->setSource(mock.get()));
}

TEST_F(DocumentSourceScoreTest, CheckAllOptionalArgsIncluded) {
    RAIIServerParameterControllerForTest prerequisitesController("featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "expression",
            normalizeFunction: "none",
            weight: 1.0
        }
    })");

    ASSERT_DOES_NOT_THROW(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreTest, CheckOnlyNormalizeFunctionSpecified) {
    RAIIServerParameterControllerForTest prerequisitesController("featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "expression",
            normalizeFunction: "none"
        }
    })");

    ASSERT_DOES_NOT_THROW(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreTest, CheckOnlyWeightSpecified) {
    RAIIServerParameterControllerForTest prerequisitesController("featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            weight: 1.0
        }
    })");

    Document inputDoc = Document{{"myScore", 5}};
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());

    ASSERT_DOES_NOT_THROW(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx())
                              ->setSource(mock.get()));
}

TEST_F(DocumentSourceScoreTest, ErrorsIfWrongNormalizeFunctionType) {
    RAIIServerParameterControllerForTest prerequisitesController("featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "expression",
            normalizeFunction: 1.0
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreTest, ErrorsIfWrongWeightType) {
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
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
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalizeFunction: "none",
            weight: 1.0
        }
    })");
    Document inputDoc = Document{{"myScore", 5}};

    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());
    docSourceScore->setSource(mock.get());

    auto next = docSourceScore->getNext();
    ASSERT(next.isAdvanced());

    // Assert inputDoc's metadata equals 5.1
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), 5);
}

TEST_F(DocumentSourceScoreTest, CheckDoubleScoreMetadataUpdated) {
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalizeFunction: "none",
            weight: 1.0
        }
    })");
    Document inputDoc = Document{{"myScore", 5.1}};

    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());
    docSourceScore->setSource(mock.get());

    auto next = docSourceScore->getNext();
    ASSERT(next.isAdvanced());

    // Assert inputDoc's metadata equals 5.1
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), 5.1);
}

TEST_F(DocumentSourceScoreTest, CheckLengthyDocScoreMetadataUpdated) {
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalizeFunction: "none"
        }
    })");
    Document inputDoc =
        Document{{"field1", "hello"_sd}, {"field2", 10}, {"myScore", 5.3}, {"field3", true}};

    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());
    docSourceScore->setSource(mock.get());

    auto next = docSourceScore->getNext();
    ASSERT(next.isAdvanced());

    // Assert inputDoc's metadata equals 5.1
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), 5.3);
}

TEST_F(DocumentSourceScoreTest, ErrorsIfScoreNotDouble) {
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalizeFunction: "none"
        }
    })");
    Document inputDoc =
        Document{{"field1", "hello"_sd}, {"field2", 10}, {"myScore", "5.3"_sd}, {"field3", true}};

    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());
    docSourceScore->setSource(mock.get());

    // Assert cannot evaluate expression into double
    ASSERT_THROWS_CODE(docSourceScore->getNext(), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreTest, ErrorsIfExpressionFieldPathDoesNotExist) {
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalizeFunction: "none"
        }
    })");
    Document inputDoc = Document{{"field1", "hello"_sd}, {"field2", 10}, {"field3", true}};

    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());
    docSourceScore->setSource(mock.get());

    // Assert cannot evaluate expression into double
    ASSERT_THROWS_CODE(docSourceScore->getNext(), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreTest, ErrorsIfScoreInvalidExpression) {
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: { $ad: ['$myScore', '$otherScore'] },
            normalizeFunction: "none"
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
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: { $add: ['$myScore', '$otherScore'] },
            normalizeFunction: "none"
        }
    })");
    Document inputDoc =
        Document{{"field1", "hello"_sd}, {"otherScore", 10}, {"myScore", 5.3}, {"field3", true}};

    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());
    docSourceScore->setSource(mock.get());

    auto next = docSourceScore->getNext();
    ASSERT(next.isAdvanced());

    // Assert inputDoc's metadata equals 15.3
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), 15.3);
}

TEST_F(DocumentSourceScoreTest, CheckNormFuncSigmoidScoreMetadataUpdated) {
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalizeFunction: "sigmoid"
        }
    })");
    double myScore = 5.3;
    Document inputDoc = Document{
        {"field1", "hello"_sd}, {"otherScore", 10}, {"myScore", myScore}, {"field3", true}};

    boost::intrusive_ptr<ExpressionContextForTest> pExpCtx = getExpCtx();
    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), pExpCtx);
    auto mock = DocumentSourceMock::createForTest(inputDoc, pExpCtx);
    docSourceScore->setSource(mock.get());

    auto next = docSourceScore->getNext();
    ASSERT(next.isAdvanced());

    double sigmoidDbl = testEvaluateSigmoid(myScore);

    // Assert inputDoc's score metadata is sigmoid(5.3)
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), sigmoidDbl);
}

TEST_F(DocumentSourceScoreTest, CheckNormFuncSigmoidWeightScoreMetadataUpdated) {
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalizeFunction: "sigmoid",
            weight: 0.5
        }
    })");
    double myScore = 5.3;
    Document inputDoc = Document{
        {"field1", "hello"_sd}, {"otherScore", 10}, {"myScore", myScore}, {"field3", true}};

    boost::intrusive_ptr<ExpressionContextForTest> pExpCtx = getExpCtx();
    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), pExpCtx);
    auto mock = DocumentSourceMock::createForTest(inputDoc, pExpCtx);
    docSourceScore->setSource(mock.get());

    auto next = docSourceScore->getNext();
    ASSERT(next.isAdvanced());

    double sigmoidDbl = testEvaluateSigmoid(myScore) * 0.5;

    // Assert inputDoc's score metadata is (0.5 * sigmoid(5.3))
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), sigmoidDbl);
}

TEST_F(DocumentSourceScoreTest, CheckWeightScoreMetadataUpdated) {
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            weight: 0.5
        }
    })");
    double myScore = 5.3;
    Document inputDoc = Document{
        {"field1", "hello"_sd}, {"otherScore", 10}, {"myScore", myScore}, {"field3", true}};

    boost::intrusive_ptr<ExpressionContextForTest> pExpCtx = getExpCtx();
    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), pExpCtx);
    auto mock = DocumentSourceMock::createForTest(inputDoc, pExpCtx);
    docSourceScore->setSource(mock.get());

    auto next = docSourceScore->getNext();
    ASSERT(next.isAdvanced());

    double sigmoidDbl = testEvaluateSigmoid(myScore) * 0.5;

    // Assert inputDoc's score metadata is (0.5 * sigmoid(5.3))
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), sigmoidDbl);
}

TEST_F(DocumentSourceScoreTest, ErrorsNormFuncSigmoidInvalidWeight) {
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalizeFunction: "sigmoid",
            weight: -0.5
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceScoreTest, ErrorsInvalidWeight) {
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
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

TEST_F(DocumentSourceScoreTest, ErrorsInvalidNormalizeFunction) {
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalizeFunction: "Sigmoid",
            weight: 0.5
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceScoreTest, CheckNormFuncNoneWeightScoreZeroMetadataUpdated) {
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagRankFusionFull", true);
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            normalizeFunction: "none",
            weight: 0
        }
    })");
    double myScore = 5.3;
    Document inputDoc = Document{
        {"field1", "hello"_sd}, {"otherScore", 10}, {"myScore", myScore}, {"field3", true}};

    boost::intrusive_ptr<ExpressionContextForTest> pExpCtx = getExpCtx();
    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), pExpCtx);
    auto mock = DocumentSourceMock::createForTest(inputDoc, pExpCtx);
    docSourceScore->setSource(mock.get());

    auto next = docSourceScore->getNext();
    ASSERT(next.isAdvanced());

    double sigmoidDbl = testEvaluateSigmoid(myScore) * 0;

    // Assert inputDoc's score metadata is (0 * 5.3)
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), sigmoidDbl);
}


TEST_F(DocumentSourceScoreTest, QueryShapeDebugString) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);

    SerializationOptions opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;

    {
        BSONObj spec = fromjson("{$score: {score: \"$myScore\", normalizeFunction: \"none\"}}");
        auto score = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
        std::vector<Value> output;
        score->serializeToArray(output, opts);
        ASSERT_EQ(output.size(), 1);
        ASSERT_BSONOBJ_EQ_AUTO(
            R"({
                $setMetadata: {
                    score: "$HASH<myScore>"
                }
            })",
            output.front().getDocument().toBson());
    }

    {
        BSONObj spec = fromjson(R"({
            $score: {
                score: "$myScore",
                normalizeFunction: "sigmoid",
                weight: 0.5
            }
        })");
        auto score = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
        std::vector<Value> output;
        score->serializeToArray(output, opts);
        ASSERT_EQ(output.size(), 1);
        ASSERT_BSONOBJ_EQ_AUTO(
            R"({
            "$setMetadata": {
                "score": {
                    "$multiply": [
                        {
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
                                                        "$HASH<myScore>"
                                                    ]
                                                }
                                            ]
                                        }
                                    ]
                                }
                            ]
                        },
                        "?number"
                    ]
                }
            }
        })",
            output.front().getDocument().toBson());
    }

    {
        BSONObj spec = fromjson(R"({
            $score: {
                score: {$divide: ["$myScore", "$otherScore"]},
                normalizeFunction: "sigmoid",
                weight: 1
            }
        })");
        auto score = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
        std::vector<Value> output;
        score->serializeToArray(output, opts);
        ASSERT_EQ(output.size(), 1);
        ASSERT_BSONOBJ_EQ_AUTO(
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
                                                {
                                                    "$divide": [
                                                        "$HASH<myScore>",
                                                        "$HASH<otherScore>"
                                                    ]
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
        })",
            output.front().getDocument().toBson());
    }
}

TEST_F(DocumentSourceScoreTest, RepresentativeQueryShape) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);

    SerializationOptions opts = SerializationOptions::kRepresentativeQueryShapeSerializeOptions;

    {
        BSONObj spec = fromjson("{$score: {score: \"$myScore\", normalizeFunction: \"none\"}}");
        auto score = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
        std::vector<Value> output;
        score->serializeToArray(output, opts);
        ASSERT_EQ(output.size(), 1);
        ASSERT_BSONOBJ_EQ_AUTO(
            R"({
                $setMetadata: {
                    score: "$myScore"
                }
            })",
            output.front().getDocument().toBson());
    }

    {
        BSONObj spec = fromjson(R"({
            $score: {
                score: "$myScore",
                normalizeFunction: "sigmoid",
                weight: 0.5
            }
        })");
        auto score = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
        std::vector<Value> output;
        score->serializeToArray(output, opts);
        ASSERT_EQ(output.size(), 1);
        ASSERT_BSONOBJ_EQ_AUTO(
            R"({
            "$setMetadata": {
                "score": {
                    "$multiply": [
                        {
                            "$divide": [
                                1,
                                {
                                    "$add": [
                                        1,
                                        {
                                            "$exp": [
                                                {
                                                    "$multiply": [
                                                        1,
                                                        "$myScore"
                                                    ]
                                                }
                                            ]
                                        }
                                    ]
                                }
                            ]
                        },
                        1
                    ]
                }
            }
        })",
            output.front().getDocument().toBson());
    }

    {
        BSONObj spec = fromjson(R"({
            $score: {
                score: {$divide: ["$myScore", 0.5]},
                normalizeFunction: "sigmoid",
                weight: 1
            }
        })");
        auto score = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
        std::vector<Value> output;
        score->serializeToArray(output, opts);
        ASSERT_EQ(output.size(), 1);
        ASSERT_BSONOBJ_EQ_AUTO(
            R"({
            "$setMetadata": {
                "score": {
                    "$divide": [
                        1,
                        {
                            "$add": [
                                1,
                                {
                                    "$exp": [
                                        {
                                            "$multiply": [
                                                1,
                                                {
                                                    "$divide": [
                                                        "$myScore",
                                                        1
                                                    ]
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
        })",
            output.front().getDocument().toBson());
    }
}

// TODO SERVER-94600: Add minMaxScaler Testcases

}  // namespace
}  // namespace mongo
