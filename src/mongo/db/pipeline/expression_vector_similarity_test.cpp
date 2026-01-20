/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/unittest/unittest.h"


namespace mongo {

using ExpressionVectorSimilarityTest = AggregationContextFixture;

TEST_F(ExpressionVectorSimilarityTest, ParseAssertConstraintsForDotProduct) {
    // Assert that the inputs are arrays / vectors.
    {
        // Test that arrays of different sizes throws.
        auto expCtx = getExpCtx();
        auto expr =
            Expression::parseExpression(expCtx.get(),
                                        fromjson("{$similarityDotProduct: [ [ 1, 2, 3], [4, 5]]}"),
                                        expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413202);
    }

    {
        // Test that calling $similarityDotProduct with arrays containing non-numeric values throws.
        auto expCtx = getExpCtx();
        auto expr = Expression::parseExpression(
            expCtx.get(),
            fromjson("{ $similarityDotProduct: [ [ 1, 2, 3 ], [ 4, 5, \"x\"] ]}"),
            expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413204);
    }

    {
        // Test that calling $similarityDotProduct with arrays containing non-numeric values throws.
        auto expCtx = getExpCtx();
        auto expr = Expression::parseExpression(
            expCtx.get(),
            fromjson("{ $similarityDotProduct: [ [ 1, 2, \"y\" ], [ 4, 5, 6 ] ]}"),
            expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413203);
    }

    {
        // Test that using non-arrays throws.
        auto expCtx = getExpCtx();
        auto expr =
            Expression::parseExpression(expCtx.get(),
                                        fromjson("{ $similarityDotProduct: [ 1, [ 1, 2, 3 ] ]}"),
                                        expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413200);
    }

    {
        // Test that using non-arrays throws.
        auto expCtx = getExpCtx();
        auto expr = Expression::parseExpression(
            expCtx.get(),
            fromjson("{ $similarityDotProduct: [ [ 1, 2, 3 ], \"z\" ]}"),
            expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413200);
    }

    {
        // Test that if the entire array is null, we return a null value.
        auto expCtx = getExpCtx();
        auto exprWithNulls = fromjson("{ $similarityDotProduct: [ null, null ] }");
        auto expr =
            Expression::parseExpression(expCtx.get(), exprWithNulls, expCtx->variablesParseState);
        auto result = expr->evaluate(Document{}, &expCtx->variables);
        ASSERT_VALUE_EQ(result, Value(BSONNULL));
    }
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateDotProduct) {
    auto expCtx = getExpCtx();
    auto expr = Expression::parseExpression(
        expCtx.get(),
        fromjson("{ $similarityDotProduct : [ [1, 2, 3] , [4, 5, 6] ] }"),
        expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_VALUE_EQ(result, Value(32.0));  // 1*4 + 2*5 + 3*6 = 32
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateDotProductScore) {
    auto expCtx = getExpCtx();
    auto expr = Expression::parseExpression(
        expCtx.get(),
        fromjson("{ $similarityDotProduct : { vectors: [ [1, 2, 3] , [4, 5, 6] ], score: true} }"),
        expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_VALUE_EQ(result, Value(16.5));  // 1*4 + 2*5 + 3*6 = 32 -> normalized (1 + 32)/2
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateDotProductExplicitNoScore) {
    auto expCtx = getExpCtx();
    auto expr = Expression::parseExpression(
        expCtx.get(),
        fromjson("{ $similarityDotProduct : { vectors: [ [1, 2, 3] , [4, 5, 6] ], score: false} }"),
        expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_VALUE_EQ(result, Value(32.0));  // 1*4 + 2*5 + 3*6 = 32
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateDotProductDouble) {
    auto expCtx = getExpCtx();
    auto expr = Expression::parseExpression(
        expCtx.get(),
        fromjson("{ $similarityDotProduct : [ [1.0, 2.5, 3] , [4, 5.0, 6] ] }"),
        expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_VALUE_EQ(result, Value(34.5));  // 1*4 + 2.5*5 + 3*6 = 34.5
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateDotProductNegative) {
    auto expCtx = getExpCtx();
    auto expr = Expression::parseExpression(
        expCtx.get(),
        fromjson("{ $similarityDotProduct : [ [-1.0, -2.5, -3] , [4, 5.0, 6] ] }"),
        expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_VALUE_EQ(result, Value(-34.5));  // -1*4 + -2.5*5 + -3*6 = -34.5
}

TEST_F(ExpressionVectorSimilarityTest, ParseAssertConstraintsForCosineSimilarity) {
    // Assert that the inputs are arrays / vectors.
    {
        // Test that arrays of different sizes throws.
        auto expCtx = getExpCtx();
        auto expr =
            Expression::parseExpression(expCtx.get(),
                                        fromjson("{$similarityCosine: [ [ 1, 2, 3], [4, 5]]}"),
                                        expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413202);
    }

    {
        // Test that calling $similarityDotProduct with arrays containing non-numeric values throws.
        auto expCtx = getExpCtx();
        auto expr = Expression::parseExpression(
            expCtx.get(),
            fromjson("{ $similarityCosine: [ [ 1, 2, 3 ], [ 4, 5, \"x\"] ]}"),
            expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413204);
    }

    {
        // Test that calling $similarityDotProduct with arrays containing non-numeric values throws.
        auto expCtx = getExpCtx();
        auto expr = Expression::parseExpression(
            expCtx.get(),
            fromjson("{ $similarityCosine: [ [ 1, 2, \"y\" ], [ 4, 5, 6 ] ]}"),
            expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413203);
    }

    {
        // Test that using non-arrays throws.
        auto expCtx = getExpCtx();
        auto expr =
            Expression::parseExpression(expCtx.get(),
                                        fromjson("{ $similarityCosine: [ 1, [ 1, 2, 3 ] ]}"),
                                        expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413200);
    }

    {
        // Test that using non-arrays throws.
        auto expCtx = getExpCtx();
        auto expr =
            Expression::parseExpression(expCtx.get(),
                                        fromjson("{ $similarityCosine: [ [ 1, 2, 3 ], \"z\" ]}"),
                                        expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413200);
    }

    {
        // Test that if the entire array is null, we return a null value.
        auto expCtx = getExpCtx();
        auto exprWithNulls = fromjson("{ $similarityCosine: [ null, null ] }");
        auto expr =
            Expression::parseExpression(expCtx.get(), exprWithNulls, expCtx->variablesParseState);
        auto result = expr->evaluate(Document{}, &expCtx->variables);
        ASSERT_VALUE_EQ(result, Value(BSONNULL));
    }

    {
        auto expCtx = getExpCtx();
        auto expr = Expression::parseExpression(expCtx.get(),
                                                fromjson("{ $similarityCosine : [ [], [] ] }"),
                                                expCtx->variablesParseState);
        auto result = expr->evaluate(Document{}, &expCtx->variables);
        ASSERT_EQ(result.coerceToDouble(), 0);
    }
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateCosineSimilarity) {
    auto expCtx = getExpCtx();
    auto expr = Expression::parseExpression(expCtx.get(),
                                            fromjson("{ $similarityCosine : [ [1, 2] , [3, 4] ] }"),
                                            expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_APPROX_EQUAL(result.coerceToDouble(), static_cast<double>(0.98387), 0.0001);
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateCosineSimilarityNormalization) {
    auto expCtx = getExpCtx();
    auto expr = Expression::parseExpression(
        expCtx.get(),
        fromjson("{ $similarityCosine : { vectors: [ [1, 2] , [3, 4] ], score: true  }}"),
        expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_APPROX_EQUAL(result.coerceToDouble(),
                        static_cast<double>(0.991935),
                        0.0001);  // 0.98387 -> normalized (1 + 0.98387)/2
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateCosineSimilarityExplicitNoNormalization) {
    auto expCtx = getExpCtx();
    auto expr = Expression::parseExpression(
        expCtx.get(),
        fromjson("{ $similarityCosine : { vectors: [ [1, 2] , [3, 4] ], score: false  }}"),
        expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_APPROX_EQUAL(result.coerceToDouble(), static_cast<double>(0.98387), 0.0001);
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateCosineSimilarityDouble) {
    auto expCtx = getExpCtx();
    auto expr =
        Expression::parseExpression(expCtx.get(),
                                    fromjson("{ $similarityCosine : [ [1.5, 2] , [3, 4.5] ] }"),
                                    expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_APPROX_EQUAL(result.coerceToDouble(), static_cast<double>(0.99846), 0.0001);
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateCosineSimilarityNegative) {
    auto expCtx = getExpCtx();
    auto expr =
        Expression::parseExpression(expCtx.get(),
                                    fromjson("{ $similarityCosine : [ [-1, 2] , [-3, 4] ] }"),
                                    expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_APPROX_EQUAL(result.coerceToDouble(), static_cast<double>(0.98387), 0.0001);
}

TEST_F(ExpressionVectorSimilarityTest, ParseAssertConstraintsEuclideanDistance) {
    // Assert that the inputs are arrays / vectors.
    {
        // Test that arrays of different sizes throws.
        auto expCtx = getExpCtx();
        auto expr =
            Expression::parseExpression(expCtx.get(),
                                        fromjson("{$similarityEuclidean: [ [ 1, 2, 3], [4, 5]]}"),
                                        expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413202);
    }

    {
        // Test that calling $similarityEuclidean with arrays containing non-numeric values throws.
        auto expCtx = getExpCtx();
        auto expr = Expression::parseExpression(
            expCtx.get(),
            fromjson("{ $similarityEuclidean: [ [ 1, 2, 3 ], [ 4, 5, \"x\"] ]}"),
            expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413204);
    }

    {
        // Test that calling $similarityEuclidean with arrays containing non-numeric values throws.
        auto expCtx = getExpCtx();
        auto expr = Expression::parseExpression(
            expCtx.get(),
            fromjson("{ $similarityEuclidean: [ [ 1, 2, \"y\" ], [ 4, 5, 6 ] ]}"),
            expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413203);
    }

    {
        // Test that using non-arrays throws.
        auto expCtx = getExpCtx();
        auto expr =
            Expression::parseExpression(expCtx.get(),
                                        fromjson("{ $similarityEuclidean: [ 1, [ 1, 2, 3 ] ]}"),
                                        expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413200);
    }

    {
        // Test that using non-arrays throws.
        auto expCtx = getExpCtx();
        auto expr =
            Expression::parseExpression(expCtx.get(),
                                        fromjson("{ $similarityEuclidean: [ [ 1, 2, 3 ], \"z\" ]}"),
                                        expCtx->variablesParseState);
        ASSERT_THROWS_CODE(
            expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413200);
    }

    {
        // Test that if the entire array is null, we return a null value.
        auto expCtx = getExpCtx();
        auto exprWithNulls = fromjson("{ $similarityEuclidean: [ null, null ] }");
        auto expr =
            Expression::parseExpression(expCtx.get(), exprWithNulls, expCtx->variablesParseState);
        auto result = expr->evaluate(Document{}, &expCtx->variables);
        ASSERT_VALUE_EQ(result, Value(BSONNULL));
    }
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateEuclideanDistance) {
    auto expCtx = getExpCtx();
    auto expr = Expression::parseExpression(
        expCtx.get(),
        fromjson("{ $similarityEuclidean : [ [1, 2, 3] , [4, 5, 6] ] }"),
        expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_APPROX_EQUAL(result.coerceToDouble(), static_cast<double>(5.19615), 0.0001);
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateEuclideanDistanceNormalization) {
    auto expCtx = getExpCtx();
    auto expr = Expression::parseExpression(
        expCtx.get(),
        fromjson("{ $similarityEuclidean : { vectors: [ [1, 2, 3] , [4, 5, 6] ], score: true} }"),
        expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_APPROX_EQUAL(result.coerceToDouble(),
                        static_cast<double>(0.16139),
                        0.0001);  // 5.19615 -> normalization 1/(1+5.19615)
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateEuclideanDistanceExplicitNoNormalization) {
    auto expCtx = getExpCtx();
    auto expr = Expression::parseExpression(
        expCtx.get(),
        fromjson("{ $similarityEuclidean : { vectors: [ [1, 2, 3] , [4, 5, 6] ], score: false} }"),
        expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_APPROX_EQUAL(result.coerceToDouble(), static_cast<double>(5.19615), 0.0001);
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateEuclideanDistanceDouble) {
    auto expCtx = getExpCtx();
    auto expr = Expression::parseExpression(
        expCtx.get(),
        fromjson("{ $similarityEuclidean : [ [1.0, 2.5, 3] , [4, 5.0, 6] ] }"),
        expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_APPROX_EQUAL(result.coerceToDouble(), static_cast<double>(4.92443), 0.0001);
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateEuclideanDistanceNegative) {
    auto expCtx = getExpCtx();
    auto expr = Expression::parseExpression(
        expCtx.get(),
        fromjson("{ $similarityEuclidean : [ [-1.0, -2.5, -3] , [4, 5.0, 6] ] }"),
        expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_APPROX_EQUAL(result.coerceToDouble(), static_cast<double>(12.73774), 0.0001);
}

// Creates a PACKED_BIT binData vector from a list of boolean values.
Value createPackedBitBinDataVector(const std::vector<bool>& values) {
    // Calculate padding: unused bits in the last byte.
    char padding = values.empty() ? 0 : (8 - (values.size() % 8)) % 8;
    std::vector<char> data = {0x10, padding};  // PACKED_BIT dType + padding
    for (size_t i = 0; i < values.size(); i += 8) {
        char byte = 0;
        for (size_t bit = 0; bit < 8 && (i + bit) < values.size(); ++bit) {
            if (values[i + bit]) {
                byte |= (1 << (7 - bit));  // MSB first.
            }
        }
        data.push_back(byte);
    }
    return Value(BSONBinData(data.data(), data.size(), BinDataType::Vector));
}

// Creates an INT8 binData vector from a list of int8 values.
Value createInt8BinDataVector(const std::vector<int8_t>& values) {
    std::vector<char> data = {0x03, 0x00};  // INT8 dType + padding.
    data.insert(data.end(), values.begin(), values.end());
    return Value(BSONBinData(data.data(), data.size(), BinDataType::Vector));
}

// Creates a FLOAT32 binData vector from a list of float values.
Value createFloat32BinDataVector(const std::vector<float>& values) {
    std::vector<char> data = {0x27, 0x00};  // FLOAT32 dType + padding.
    for (auto v : values) {
        auto* bytes = reinterpret_cast<const char*>(&v);
        data.insert(data.end(), bytes, bytes + sizeof(float));
    }
    return Value(BSONBinData(data.data(), data.size(), BinDataType::Vector));
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateDotProductWithPackedBitVectors) {
    auto expCtx = getExpCtx();
    auto vec1 = createPackedBitBinDataVector({true, true, false, true});
    auto vec2 = createPackedBitBinDataVector({true, false, true, true});

    auto expr =
        Expression::parseExpression(expCtx.get(),
                                    BSON("$similarityDotProduct" << BSON_ARRAY(vec1 << vec2)),
                                    expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_VALUE_EQ(result, Value(2.0));
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateCosineWithPackedBitVectors) {
    auto expCtx = getExpCtx();
    auto vec1 = createPackedBitBinDataVector({true, true, false, false});
    auto vec2 = createPackedBitBinDataVector({true, false, true, false});

    auto expr = Expression::parseExpression(expCtx.get(),
                                            BSON("$similarityCosine" << BSON_ARRAY(vec1 << vec2)),
                                            expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_APPROX_EQUAL(result.coerceToDouble(), 0.5, 0.0001);
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateEuclideanWithPackedBitVectors) {
    auto expCtx = getExpCtx();
    auto vec1 = createPackedBitBinDataVector({true, false, true});
    auto vec2 = createPackedBitBinDataVector({false, true, false});

    auto expr =
        Expression::parseExpression(expCtx.get(),
                                    BSON("$similarityEuclidean" << BSON_ARRAY(vec1 << vec2)),
                                    expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_APPROX_EQUAL(result.coerceToDouble(), 1.732, 0.001);
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateDotProductWithBinDataVectors) {
    auto expCtx = getExpCtx();
    auto vec1 = createInt8BinDataVector({1, 2, 3});
    auto vec2 = createInt8BinDataVector({4, 5, 6});

    auto expr =
        Expression::parseExpression(expCtx.get(),
                                    BSON("$similarityDotProduct" << BSON_ARRAY(vec1 << vec2)),
                                    expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_VALUE_EQ(result, Value(32.0));
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateDotProductWithFloat32BinDataVectors) {
    auto expCtx = getExpCtx();
    auto vec1 = createFloat32BinDataVector({1.0f, 2.0f, 3.0f});
    auto vec2 = createFloat32BinDataVector({4.0f, 5.0f, 6.0f});

    auto expr =
        Expression::parseExpression(expCtx.get(),
                                    BSON("$similarityDotProduct" << BSON_ARRAY(vec1 << vec2)),
                                    expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_VALUE_EQ(result, Value(32.0));
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateCosineWithBinDataVectors) {
    auto expCtx = getExpCtx();
    auto vec1 = createInt8BinDataVector({1, 2, 3});
    auto vec2 = createInt8BinDataVector({4, 5, 6});

    auto expr = Expression::parseExpression(expCtx.get(),
                                            BSON("$similarityCosine" << BSON_ARRAY(vec1 << vec2)),
                                            expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_APPROX_EQUAL(result.coerceToDouble(), static_cast<double>(0.97463), 0.0001);
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateEuclideanWithBinDataVectors) {
    auto expCtx = getExpCtx();
    auto vec1 = createInt8BinDataVector({1, 2, 3});
    auto vec2 = createInt8BinDataVector({4, 5, 6});

    auto expr =
        Expression::parseExpression(expCtx.get(),
                                    BSON("$similarityEuclidean" << BSON_ARRAY(vec1 << vec2)),
                                    expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_APPROX_EQUAL(result.coerceToDouble(), static_cast<double>(5.19615), 0.0001);
}

TEST_F(ExpressionVectorSimilarityTest, EvaluateDotProductEmptyBinDataVectors) {
    auto expCtx = getExpCtx();
    Value emptyVec(BSONBinData(nullptr, 0, BinDataType::Vector));

    auto expr = Expression::parseExpression(
        expCtx.get(),
        BSON("$similarityDotProduct" << BSON_ARRAY(emptyVec << emptyVec)),
        expCtx->variablesParseState);
    auto result = expr->evaluate(Document{}, &expCtx->variables);
    ASSERT_VALUE_EQ(result, Value(0.0));
}

// Tests that binData vectors of different sizes throws an error.
TEST_F(ExpressionVectorSimilarityTest, BinDataVectorsDifferentSizesThrows) {
    auto expCtx = getExpCtx();
    auto vec1 = createInt8BinDataVector({1, 2, 3});
    auto vec2 = createInt8BinDataVector({4, 5});

    auto expr =
        Expression::parseExpression(expCtx.get(),
                                    BSON("$similarityDotProduct" << BSON_ARRAY(vec1 << vec2)),
                                    expCtx->variablesParseState);
    ASSERT_THROWS_CODE(
        expr->evaluate(Document{}, &expCtx->variables), AssertionException, 10413202);
}
}  // namespace mongo
