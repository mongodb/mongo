// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo {
namespace ExpressionHashTest {
using namespace std::literals::string_view_literals;

class ExpressionHashTest : public AggregationContextFixture {
protected:
    void assertHashResult(std::string_view algorithm,
                          const Document& input,
                          std::string expectedBytes) {
        auto expCtx = getExpCtx();
        auto spec = fromjson(str::stream()
                             << "{$hash: {input: '$path', algorithm: '" << algorithm << "'}}");
        auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        auto result = hashExp->evaluate(input, &expCtx->variables);

        ASSERT_EQ(result.getType(), BSONType::binData);
        ASSERT_VALUE_EQ(
            result, Value(BSONBinData(expectedBytes.data(), expectedBytes.size(), BinDataGeneral)));
    }

    void assertHexHashResult(std::string_view algorithm,
                             const Document& input,
                             std::string_view expectedHexString) {
        auto expCtx = getExpCtx();
        auto spec = fromjson(str::stream()
                             << "{$hexHash: {input: '$path', algorithm: '" << algorithm << "'}}");
        auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        auto result = hashExp->evaluate(input, &expCtx->variables);

        ASSERT_EQ(result.getType(), BSONType::string);
        ASSERT_VALUE_EQ(result, Value(expectedHexString));
    }

    const std::vector<std::string_view> opNames = {"$hash"sv, "$hexHash"sv};
};

TEST_F(ExpressionHashTest, ParseAndSerializeHash) {
    auto expCtx = getExpCtx();
    auto spec = fromjson("{$hash: {input: '$path', algorithm: 'xxh64'}}");
    auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ(hashExp->serialize(),
                    Value(fromjson("{$hash: {input: '$path', algorithm: {$const: 'xxh64'}}}")));
}

TEST_F(ExpressionHashTest, ParseAndSerializeHexHash) {
    auto expCtx = getExpCtx();
    auto spec = fromjson("{$hexHash: {input: '$path', algorithm: 'xxh64'}}");
    auto hexHashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

    ASSERT_VALUE_EQ(
        hexHashExp->serialize(),
        Value(fromjson("{$convert: {input: {$hash: {input: '$path', algorithm: {$const: "
                       "'xxh64'}}}, to: {$const: 'string'}, format: {$const: 'hex'}}}")));
}

TEST_F(ExpressionHashTest, ParseFailsWithoutInput) {
    for (auto opName : opNames) {
        auto expCtx = getExpCtx();
        auto spec = BSON(opName << BSON("algorithm" << "xxh64"));
        ASSERT_THROWS_WITH_CHECK(
            Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
            AssertionException,
            [&](const AssertionException& exception) {
                ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
                ASSERT_STRING_CONTAINS(exception.reason(),
                                       str::stream() << "Missing 'input' parameter to " << opName);
            });
    }
}

TEST_F(ExpressionHashTest, ParseFailsWithoutAlgorithm) {
    for (auto opName : opNames) {
        auto expCtx = getExpCtx();
        auto spec = BSON(opName << BSON("input" << "$field"));
        ASSERT_THROWS_WITH_CHECK(
            Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
            AssertionException,
            [&](const AssertionException& exception) {
                ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
                ASSERT_STRING_CONTAINS(exception.reason(),
                                       str::stream()
                                           << "Missing 'algorithm' parameter to " << opName);
            });
    }
}

TEST_F(ExpressionHashTest, ParseFailsWithUnknownArgument) {
    for (auto opName : opNames) {
        auto expCtx = getExpCtx();
        auto spec = BSON(opName << BSON("input" << "$f"
                                                << "algorithm"
                                                << "xxh64"
                                                << "extra" << 1));
        ASSERT_THROWS_WITH_CHECK(
            Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
            AssertionException,
            [&](const AssertionException& exception) {
                ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
                ASSERT_STRING_CONTAINS(exception.reason(),
                                       str::stream()
                                           << opName << " found an unknown argument: extra");
            });
    }
}

TEST_F(ExpressionHashTest, ParseFailsWithNonObjectArg) {
    for (auto opName : opNames) {
        auto expCtx = getExpCtx();
        BSONObj spec = BSON(opName << "hey");
        ASSERT_THROWS_WITH_CHECK(
            Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
            AssertionException,
            [&](const AssertionException& exception) {
                ASSERT_EQ(exception.code(), ErrorCodes::FailedToParse);
                ASSERT_STRING_CONTAINS(
                    exception.reason(),
                    str::stream() << opName
                                  << " expects an object of named arguments but found: string");
            });
    }
}

TEST_F(ExpressionHashTest, InvalidUtf8StringFails) {
    for (auto opName : opNames) {
        auto expCtx = getExpCtx();
        auto spec = BSON(opName << BSON("input" << "$path"
                                                << "algorithm"
                                                << "xxh64"));
        auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

        Document input{{"path", "\xc2"sv}};
        ASSERT_THROWS_WITH_CHECK(hashExp->evaluate(input, &expCtx->variables),
                                 AssertionException,
                                 [&](const AssertionException& exception) {
                                     ASSERT_STRING_CONTAINS(
                                         exception.reason(),
                                         "$hash requires that 'input' be a valid UTF-8 string or "
                                         "binData, found: string");
                                 });
    }
}

TEST_F(ExpressionHashTest, InputArrayFails) {
    for (auto opName : opNames) {
        auto expCtx = getExpCtx();
        auto spec = BSON(opName << BSON("input" << "$path"
                                                << "algorithm"
                                                << "xxh64"));
        auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

        Document input{{"path", BSON_ARRAY(1 << 2 << 3)}};
        ASSERT_THROWS_WITH_CHECK(hashExp->evaluate(input, &expCtx->variables),
                                 AssertionException,
                                 [&](const AssertionException& exception) {
                                     ASSERT_STRING_CONTAINS(
                                         exception.reason(),
                                         "$hash requires that 'input' be a valid UTF-8 string or "
                                         "binData, found: array with value [1, 2, 3]");
                                 });
    }
}

TEST_F(ExpressionHashTest, NullAlgorithmFails) {
    for (auto opName : opNames) {
        auto expCtx = getExpCtx();
        auto spec = BSON(opName << BSON("input" << "$path" << "algorithm" << BSONNULL));
        auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

        Document input{{"path", "test"sv}};
        ASSERT_THROWS_WITH_CHECK(
            hashExp->evaluate(input, &expCtx->variables),
            AssertionException,
            [&](const AssertionException& exception) {
                ASSERT_STRING_CONTAINS(
                    exception.reason(),
                    "$hash requires that 'algorithm' be a string, found: null with value null");
            });
    }
}

TEST_F(ExpressionHashTest, InvalidAlgorithmNameFails) {
    for (auto opName : {"$hash"sv, "$hexHash"sv}) {
        auto expCtx = getExpCtx();
        auto spec = BSON(opName << BSON("input" << "$path"
                                                << "algorithm"
                                                << "sha1"));
        auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

        Document input{{"path", "test"sv}};
        ASSERT_THROWS_WITH_CHECK(hashExp->evaluate(input, &expCtx->variables),
                                 AssertionException,
                                 [&](const AssertionException& exception) {
                                     ASSERT_STRING_CONTAINS(
                                         exception.reason(),
                                         "Currently, the only supported algorithms for $hash are "
                                         "'md5', 'sha256' and 'xxh64', found: sha1");
                                 });
    }
}

TEST_F(ExpressionHashTest, AlgorithmCaseSensitiveFails) {
    for (auto opName : {"$hash"sv, "$hexHash"sv}) {
        auto expCtx = getExpCtx();
        auto spec = BSON(opName << BSON("input" << "$path"
                                                << "algorithm"
                                                << "MD5"));
        auto hashExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);

        Document input{{"path", "test"sv}};
        ASSERT_THROWS_WITH_CHECK(hashExp->evaluate(input, &expCtx->variables),
                                 AssertionException,
                                 [&](const AssertionException& exception) {
                                     ASSERT_STRING_CONTAINS(
                                         exception.reason(),
                                         "Currently, the only supported algorithms for $hash are "
                                         "'md5', 'sha256' and 'xxh64', found: MD5");
                                 });
    }
}

TEST_F(ExpressionHashTest, HashWithXxh64WorksForString) {
    assertHashResult(
        "xxh64"sv, Document{{"path", "Hello World"sv}}, base64::decode("YzTSBxkkW8I="));
}

TEST_F(ExpressionHashTest, HexHashWithXxh64WorksForString) {
    assertHexHashResult("xxh64"sv, Document{{"path", "Hello World"sv}}, "6334D20719245BC2");
}

TEST_F(ExpressionHashTest, HashWithXxh64WorksForBinaryData) {
    std::string_view helloWorld = "Hello World"sv;
    assertHashResult(
        "xxh64"sv,
        Document{{"path", BSONBinData(helloWorld.data(), helloWorld.size(), BinDataGeneral)}},
        base64::decode("YzTSBxkkW8I="));
}

TEST_F(ExpressionHashTest, HexHashWithXxh64WorksForBinaryData) {
    std::string_view helloWorld = "Hello World"sv;
    assertHexHashResult(
        "xxh64"sv,
        Document{{"path", BSONBinData(helloWorld.data(), helloWorld.size(), BinDataGeneral)}},
        "6334D20719245BC2");
}

TEST_F(ExpressionHashTest, HashWithSha256WorksForString) {
    assertHashResult("sha256"sv,
                     Document{{"path", "Hello World"sv}},
                     base64::decode("pZGm1Av0IEBKARczz7exkNYsZb8LzaMrV7J32a2fFG4="));
}

TEST_F(ExpressionHashTest, HexHashWithSha256WorksForString) {
    assertHexHashResult("sha256"sv,
                        Document{{"path", "Hello World"sv}},
                        "A591A6D40BF420404A011733CFB7B190D62C65BF0BCDA32B57B277D9AD9F146E");
}

TEST_F(ExpressionHashTest, HashWithSha256WorksForBinaryData) {
    std::string_view helloWorld = "Hello World"sv;
    assertHashResult(
        "sha256"sv,
        Document{{"path", BSONBinData(helloWorld.data(), helloWorld.size(), BinDataGeneral)}},
        base64::decode("pZGm1Av0IEBKARczz7exkNYsZb8LzaMrV7J32a2fFG4="));
}

TEST_F(ExpressionHashTest, HexHashWithSha256WorksForBinaryData) {
    std::string_view helloWorld = "Hello World"sv;
    assertHexHashResult(
        "sha256"sv,
        Document{{"path", BSONBinData(helloWorld.data(), helloWorld.size(), BinDataGeneral)}},
        "A591A6D40BF420404A011733CFB7B190D62C65BF0BCDA32B57B277D9AD9F146E");
}

TEST_F(ExpressionHashTest, HashWithMd5WorksForString) {
    assertHashResult(
        "md5"sv, Document{{"path", "Hello World"sv}}, base64::decode("sQqNsWTgdUEFt6mb5y4/5Q=="));
}

TEST_F(ExpressionHashTest, HexHashWithMd5WorksForString) {
    assertHexHashResult(
        "md5"sv, Document{{"path", "Hello World"sv}}, "B10A8DB164E0754105B7A99BE72E3FE5");
}

TEST_F(ExpressionHashTest, HashWithMd5WorksForBinaryData) {
    std::string_view helloWorld = "Hello World"sv;
    assertHashResult(
        "md5"sv,
        Document{{"path", BSONBinData(helloWorld.data(), helloWorld.size(), BinDataGeneral)}},
        base64::decode("sQqNsWTgdUEFt6mb5y4/5Q=="));
}

TEST_F(ExpressionHashTest, HexHashWithMd5WorksForBinaryData) {
    std::string_view helloWorld = "Hello World"sv;
    assertHexHashResult(
        "md5"sv,
        Document{{"path", BSONBinData(helloWorld.data(), helloWorld.size(), BinDataGeneral)}},
        "B10A8DB164E0754105B7A99BE72E3FE5");
}

}  // namespace ExpressionHashTest
}  // namespace mongo
