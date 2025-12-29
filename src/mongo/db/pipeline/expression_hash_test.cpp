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

#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace ExpressionHashTest {

class ExpressionHashTest : public AggregationContextFixture {
protected:
    void assertHashResult(StringData algorithm, const Document& input, std::string expectedBytes) {
        auto expCtx = getExpCtx();
        auto spec = fromjson(str::stream()
                             << "{$hash: {input: '$path', algorithm: '" << algorithm << "'}}");
        auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        auto result = hashExp->evaluate(input, &expCtx->variables);

        ASSERT_EQ(result.getType(), BSONType::binData);
        ASSERT_VALUE_EQ(
            result, Value(BSONBinData(expectedBytes.data(), expectedBytes.size(), BinDataGeneral)));
    }
};

TEST_F(ExpressionHashTest, ParseAndSerialize) {
    auto expCtx = getExpCtx();
    auto spec = fromjson("{$hash: {input: '$path', algorithm: 'xxh64'}}");
    auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ(hashExp->serialize(),
                    Value(fromjson("{$hash: {input: '$path', algorithm: {$const: 'xxh64'}}}")));
}

TEST_F(ExpressionHashTest, ParseFailsWithoutInput) {
    auto expCtx = getExpCtx();
    auto spec = fromjson("{$hash: {algorithm: 'xxh64'}}");
    ASSERT_THROWS_WITH_CHECK(
        Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
            ASSERT_STRING_CONTAINS(exception.reason(), "Missing 'input' parameter to $hash");
        });
}

TEST_F(ExpressionHashTest, ParseFailsWithoutAlgorithm) {
    auto expCtx = getExpCtx();
    auto spec = fromjson("{$hash: {input: '$field'}}");
    ASSERT_THROWS_WITH_CHECK(
        Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
            ASSERT_STRING_CONTAINS(exception.reason(), "Missing 'algorithm' parameter to $hash");
        });
}

TEST_F(ExpressionHashTest, ParseFailsWithUnknownArgument) {
    auto expCtx = getExpCtx();
    auto spec = fromjson("{$hash: {input: '$f', algorithm: 'xxh64', extra: 1}}");
    ASSERT_THROWS_WITH_CHECK(
        Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
            ASSERT_STRING_CONTAINS(exception.reason(), "$hash found an unknown argument: extra");
        });
}

TEST_F(ExpressionHashTest, ParseFailsWithNonObjectArg) {
    auto expCtx = getExpCtx();
    auto spec = fromjson("{$hash: 'hey'}");
    ASSERT_THROWS_WITH_CHECK(
        Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
            ASSERT_STRING_CONTAINS(exception.reason(),
                                   "$hash expects an object of named arguments but found: string");
        });
}

TEST_F(ExpressionHashTest, InvalidUtf8StringFails) {
    auto expCtx = getExpCtx();
    auto spec = fromjson("{$hash: {input: '$path', algorithm: 'xxh64'}}");
    auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document input{{"path", "\xc2"_sd}};
    ASSERT_THROWS_WITH_CHECK(
        hashExp->evaluate(input, &expCtx->variables),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_STRING_CONTAINS(
                exception.reason(),
                "$hash requires that 'input' be a valid UTF-8 string or binData, found: string");
        });
}

TEST_F(ExpressionHashTest, InputArrayFails) {
    auto expCtx = getExpCtx();
    auto spec = fromjson("{$hash: {input: \"$path\", algorithm: 'xxh64'}}");
    auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document input{{"path", BSON_ARRAY(1 << 2 << 3)}};
    ASSERT_THROWS_WITH_CHECK(hashExp->evaluate(input, &expCtx->variables),
                             AssertionException,
                             [](const AssertionException& exception) {
                                 ASSERT_STRING_CONTAINS(
                                     exception.reason(),
                                     "$hash requires that 'input' be a valid UTF-8 string or "
                                     "binData, found: array with value [1, 2, 3]");
                             });
}

TEST_F(ExpressionHashTest, NullAlgorithmFails) {
    auto expCtx = getExpCtx();
    auto spec = fromjson("{$hash: {input: '$path', algorithm: null}}");
    auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document input{{"path", "test"_sd}};
    ASSERT_THROWS_WITH_CHECK(
        hashExp->evaluate(input, &expCtx->variables),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_STRING_CONTAINS(
                exception.reason(),
                "$hash requires that 'algorithm' be a string, found: null with value null");
        });
}

TEST_F(ExpressionHashTest, InvalidAlgorithmNameFails) {
    auto expCtx = getExpCtx();
    auto spec = fromjson("{$hash: {input: '$path', algorithm: 'sha1'}}");
    auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document input{{"path", "test"_sd}};
    ASSERT_THROWS_WITH_CHECK(
        hashExp->evaluate(input, &expCtx->variables),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_STRING_CONTAINS(exception.reason(),
                                   "Currently, the only supported algorithms for $hash are 'md5', "
                                   "'sha256' and 'xxh64', found: sha1");
        });
}

TEST_F(ExpressionHashTest, AlgorithmCaseSensitiveFails) {
    auto expCtx = getExpCtx();
    auto spec = fromjson("{$hash: {input: '$path', algorithm: 'MD5'}}");
    auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    Document input{{"path", "test"_sd}};
    ASSERT_THROWS_WITH_CHECK(
        hashExp->evaluate(input, &expCtx->variables),
        AssertionException,
        [](const AssertionException& exception) {
            ASSERT_STRING_CONTAINS(exception.reason(),
                                   "Currently, the only supported algorithms for $hash are 'md5', "
                                   "'sha256' and 'xxh64', found: MD5");
        });
}

TEST_F(ExpressionHashTest, HashWithXxh64WorksForString) {
    assertHashResult(
        "xxh64"_sd, Document{{"path", "Hello World"_sd}}, base64::decode("YzTSBxkkW8I="));
}

TEST_F(ExpressionHashTest, HashWithXxh64WorksForBinaryData) {
    StringData helloWorld = "Hello World"_sd;
    assertHashResult(
        "xxh64"_sd,
        Document{{"path", BSONBinData(helloWorld.data(), helloWorld.size(), BinDataGeneral)}},
        base64::decode("YzTSBxkkW8I="));
}

TEST_F(ExpressionHashTest, HashWithSha256WorksForString) {
    assertHashResult("sha256"_sd,
                     Document{{"path", "Hello World"_sd}},
                     base64::decode("pZGm1Av0IEBKARczz7exkNYsZb8LzaMrV7J32a2fFG4="));
}

TEST_F(ExpressionHashTest, HashWithSha256WorksForBinaryData) {
    StringData helloWorld = "Hello World"_sd;
    assertHashResult(
        "sha256"_sd,
        Document{{"path", BSONBinData(helloWorld.data(), helloWorld.size(), BinDataGeneral)}},
        base64::decode("pZGm1Av0IEBKARczz7exkNYsZb8LzaMrV7J32a2fFG4="));
}

TEST_F(ExpressionHashTest, HashWithMd5WorksForString) {
    assertHashResult(
        "md5"_sd, Document{{"path", "Hello World"_sd}}, base64::decode("sQqNsWTgdUEFt6mb5y4/5Q=="));
}

TEST_F(ExpressionHashTest, HashWithMd5WorksForBinaryData) {
    StringData helloWorld = "Hello World"_sd;
    assertHashResult(
        "md5"_sd,
        Document{{"path", BSONBinData(helloWorld.data(), helloWorld.size(), BinDataGeneral)}},
        base64::decode("sQqNsWTgdUEFt6mb5y4/5Q=="));
}

}  // namespace ExpressionHashTest
}  // namespace mongo
