// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/expression/evaluate_test_helpers.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace expression_evaluation_test {
using namespace std::literals::string_view_literals;

TEST(ExpressionMetaTest, ExpressionMetaSearchScore) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"searchScore\"}");
    auto expressionMeta = ExpressionMeta::parse(&expCtx, expr.firstElement(), vps);
    MutableDocument doc;
    doc.metadata().setSearchScore(1.234);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_EQ(val.getDouble(), 1.234);
}

TEST(ExpressionMetaTest, ExpressionMetaSearchHighlights) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"searchHighlights\"}");
    auto expressionMeta = ExpressionMeta::parse(&expCtx, expr.firstElement(), vps);

    MutableDocument doc;
    Document highlights = DOC("this part" << 1 << "is opaque to the server" << 1);
    doc.metadata().setSearchHighlights(Value(highlights));

    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_DOCUMENT_EQ(val.getDocument(), highlights);
}

TEST(ExpressionMetaTest, ExpressionMetaGeoNearDistance) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"geoNearDistance\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    doc.metadata().setGeoNearDistance(1.23);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_EQ(val.getDouble(), 1.23);
}

TEST(ExpressionMetaTest, ExpressionMetaGeoNearPoint) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"geoNearPoint\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    Document pointDoc = Document{fromjson("{some: 'document'}")};
    doc.metadata().setGeoNearPoint(Value(pointDoc));
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_DOCUMENT_EQ(val.getDocument(), pointDoc);
}

TEST(ExpressionMetaTest, ExpressionMetaIndexKey) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"indexKey\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    BSONObj ixKey = fromjson("{'': 1, '': 'string'}");
    doc.metadata().setIndexKey(ixKey);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_DOCUMENT_EQ(val.getDocument(), Document(ixKey));
}

TEST(ExpressionMetaTest, ExpressionMetaRecordId) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"recordId\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    doc.metadata().setRecordId(RecordId(123LL));
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_EQ(val.getLong(), 123LL);
}

TEST(ExpressionMetaTest, ExpressionMetaRandVal) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"randVal\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    doc.metadata().setRandVal(1.23);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_EQ(val.getDouble(), 1.23);
}

TEST(ExpressionMetaTest, ExpressionMetaSortKey) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"sortKey\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    Value sortKey = Value(std::vector<Value>{Value(1), Value(2)});
    doc.metadata().setSortKey(sortKey, /* isSingleElementSortKey = */ false);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_VALUE_EQ(val, Value(std::vector<Value>{Value(1), Value(2)}));
}

TEST(ExpressionMetaTest, ExpressionMetaTextScore) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"textScore\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    doc.metadata().setTextScore(1.23);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_EQ(val.getDouble(), 1.23);
}

TEST(ExpressionMetaTest, ExpressionMetaSearchScoreDetails) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"searchScoreDetails\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    auto details = BSON("scoreDetails" << "foo");
    MutableDocument doc;
    doc.metadata().setSearchScoreDetails(details);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_DOCUMENT_EQ(val.getDocument(), Document(details));
}

TEST(ExpressionMetaTest, ExpressionMetaVectorSearchScore) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"vectorSearchScore\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    doc.metadata().setVectorSearchScore(1.23);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_EQ(val.getDouble(), 1.23);
}

TEST(ExpressionMetaTest, ExpressionMetaScore) {
    // Used to set 'score' metadata.
    unittest::ServerParameterGuard featureFlagController("featureFlagRankFusionFull", true);
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"score\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    MutableDocument doc;
    doc.metadata().setScore(1.23);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_EQ(val.getDouble(), 1.23);
}

TEST(ExpressionMetaTest, ExpressionMetaScoreDetails) {
    // Used to set 'scoreDetails' metadata.
    unittest::ServerParameterGuard featureFlagController("featureFlagRankFusionFull", true);
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"scoreDetails\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    auto details = BSON("value" << 5 << "scoreDetails"
                                << "foo");
    MutableDocument doc;
    doc.metadata().setScoreAndScoreDetails(Value(details));
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_DOCUMENT_EQ(val.getDocument(), Document(details));
}

TEST(ExpressionMetaTest, ExpressionMetaStream) {
    unittest::ServerParameterGuard searchHybridScoringPrerequisitesController("featureFlagStreams",
                                                                              true);

    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    BSONObj expr = fromjson(R"({$meta: "stream"})");
    auto exp = ExpressionMeta::parse(&expCtx, expr.firstElement(), vps);
    auto makeDoc = [](Value streamMeta) {
        MutableDocument doc;
        doc.metadata().setStream(std::move(streamMeta));
        return doc.freeze();
    };

    Value kafkaMeta(Document(fromjson(R"(
        {
            "source": {"type": "kafka", "topic": "hello", "key": {"k1": "v1", "k2": "v2"}, "headers": [{"k": "foo", "v": "bar"}, {"k": "foo2", "v": "bar2"}]}}
    )")));
    Value windowMeta(Document(fromjson(R"(
        {
            "source": {"type": "atlas"},
            "window": {
                "start": {"$date": "2024-01-01T00:00:00.000Z"},
                "end": {"$date": "2024-01-01T00:00:01.000Z"}
            }
        }
    )")));

    // Empty/unset case.
    ASSERT(exp->evaluate(Document{}, &expCtx.variables).missing());

    // Test reading all stream metadata.
    ASSERT_VALUE_EQ(kafkaMeta, exp->evaluate(makeDoc(kafkaMeta), &expCtx.variables));
    ASSERT_VALUE_EQ(windowMeta, exp->evaluate(makeDoc(windowMeta), &expCtx.variables));

    // Test reading source metadata.
    expr = fromjson("{$meta: \"stream.source\"}");
    exp = ExpressionMeta::parse(&expCtx, expr.firstElement(), vps);
    ASSERT_VALUE_EQ(kafkaMeta.getDocument()["source"],
                    exp->evaluate(makeDoc(kafkaMeta), &expCtx.variables));

    // Test reading header metadata.
    expr = fromjson("{$meta: \"stream.source.headers\"}");
    exp = ExpressionMeta::parse(&expCtx, expr.firstElement(), vps);
    ASSERT_VALUE_EQ(kafkaMeta.getDocument()["source"].getDocument()["headers"],
                    exp->evaluate(makeDoc(kafkaMeta), &expCtx.variables));

    // Test reading header metadata with path.
    expr = fromjson("{$meta: \"stream.source.headers.k\"}");
    exp = ExpressionMeta::parse(&expCtx, expr.firstElement(), vps);
    ASSERT_VALUE_EQ(Value(std::vector<Value>{Value("foo"sv), Value("foo2"sv)}),
                    exp->evaluate(makeDoc(kafkaMeta), &expCtx.variables));

    // Test reading window metadata.
    expr = fromjson("{$meta: \"stream.window\"}");
    exp = ExpressionMeta::parse(&expCtx, expr.firstElement(), vps);
    ASSERT_VALUE_EQ(windowMeta.getDocument()["window"],
                    exp->evaluate(makeDoc(windowMeta), &expCtx.variables));
    ASSERT_VALUE_EQ(windowMeta["window"]["end"],
                    Value(dateFromISOString("2024-01-01T00:00:01.000Z").getValue()));

    // Test to ensure that rewriting {$meta: "stream.source.topic"} to
    // {$let: {in: "$$stream.source.topic"}, vars: {stream: {$meta: "stream"}}}
    // doesn't run into weird issues when a parent expression has another variable named stream
    // defined.
    expr = fromjson(R"(
    {
        "$let": {
            in: { "$concat": ["$$stream", "-", "$$topic"] },
            vars: {
                "stream": "foo",
                "topic": {
                    "$meta": "stream.source.topic"
                }
            }
        }
    }
    )");
    exp = ExpressionLet::parse(&expCtx, expr.firstElement(), vps);
    ASSERT_VALUE_EQ(Value("foo-hello"sv), exp->evaluate(makeDoc(kafkaMeta), &expCtx.variables));
}

TEST(ExpressionMetaTest, ExpressionMetaStreamSerialization) {
    unittest::ServerParameterGuard searchHybridScoringPrerequisitesController("featureFlagStreams",
                                                                              true);
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    // Serialize and de-serialize the Document with stream meta set.
    Document kafkaMeta(fromjson(R"(
        {
            "source": {"type": "kafka", "key": {"k1": "v1", "k2": "v2"}, "headers": [{"k": "foo"}, {"v": "bar"}]}
        }
    )"));
    MutableDocument mut;
    mut.metadata().setStream(Value(kafkaMeta));
    BufBuilder b;
    mut.freeze().serializeForSorter(b);
    BufReader r{b.buf(), static_cast<unsigned int>(b.len())};
    Document doc = Document::deserializeForSorter(r, {});

    // Serialize and de-serialize the meta expression.
    auto expr = fromjson("{$meta: \"stream\"}");
    auto expCtx2 = ExpressionContextForTest{};
    auto expressionMeta = ExpressionMeta::parse(&expCtx, expr.firstElement(), vps);
    auto result = expressionMeta->evaluate(doc, &expCtx.variables).getDocument();
    expressionMeta = ExpressionMeta::parse(
        &expCtx2,
        expressionMeta->serialize(query_shape::SerializationOptions{.serializeForCloning = true})
            .getDocument()
            .toBson()
            .firstElement(),
        expCtx2.variablesParseState);

    ASSERT_VALUE_EQ(Value(result), Value(kafkaMeta));
}

TEST(ExpressionTypeTest, WithMinKeyValue) {
    assertExpectedResults("$type", {{{Value(MINKEY)}, Value("minKey"sv)}});
}

TEST(ExpressionTypeTest, WithDoubleValue) {
    assertExpectedResults("$type", {{{Value(1.0)}, Value("double"sv)}});
}

TEST(ExpressionTypeTest, WithStringValue) {
    assertExpectedResults("$type", {{{Value("stringValue"sv)}, Value("string"sv)}});
}

TEST(ExpressionTypeTest, WithObjectValue) {
    BSONObj objectVal = fromjson("{a: {$literal: 1}}");
    assertExpectedResults("$type", {{{Value(objectVal)}, Value("object"sv)}});
}

TEST(ExpressionTypeTest, WithArrayValue) {
    assertExpectedResults("$type", {{{Value(BSON_ARRAY(1 << 2))}, Value("array"sv)}});
}

TEST(ExpressionTypeTest, WithBinDataValue) {
    BSONBinData binDataVal = BSONBinData("", 0, BinDataGeneral);
    assertExpectedResults("$type", {{{Value(binDataVal)}, Value("binData"sv)}});
}

TEST(ExpressionTypeTest, WithUndefinedValue) {
    assertExpectedResults("$type", {{{Value(BSONUndefined)}, Value("undefined"sv)}});
}

TEST(ExpressionTypeTest, WithOIDValue) {
    assertExpectedResults("$type", {{{Value(OID())}, Value("objectId"sv)}});
}

TEST(ExpressionTypeTest, WithBoolValue) {
    assertExpectedResults("$type", {{{Value(true)}, Value("bool"sv)}});
}

TEST(ExpressionTypeTest, WithDateValue) {
    Date_t dateVal = BSON("" << DATENOW).firstElement().Date();
    assertExpectedResults("$type", {{{Value(dateVal)}, Value("date"sv)}});
}

TEST(ExpressionTypeTest, WithNullValue) {
    assertExpectedResults("$type", {{{Value(BSONNULL)}, Value("null"sv)}});
}

TEST(ExpressionTypeTest, WithRegexValue) {
    assertExpectedResults("$type", {{{Value(BSONRegEx("a.b"))}, Value("regex"sv)}});
}

TEST(ExpressionTypeTest, WithSymbolValue) {
    assertExpectedResults("$type", {{{Value(BSONSymbol("a"))}, Value("symbol"sv)}});
}

TEST(ExpressionTypeTest, WithDBRefValue) {
    assertExpectedResults("$type", {{{Value(BSONDBRef("", OID()))}, Value("dbPointer"sv)}});
}

TEST(ExpressionTypeTest, WithCodeWScopeValue) {
    assertExpectedResults(
        "$type",
        {{{Value(BSONCodeWScope("var x = 3", BSONObj()))}, Value("javascriptWithScope"sv)}});
}

TEST(ExpressionTypeTest, WithCodeValue) {
    assertExpectedResults("$type", {{{Value(BSONCode("var x = 3"))}, Value("javascript"sv)}});
}

TEST(ExpressionTypeTest, WithIntValue) {
    assertExpectedResults("$type", {{{Value(1)}, Value("int"sv)}});
}

TEST(ExpressionTypeTest, WithDecimalValue) {
    assertExpectedResults("$type", {{{Value(Decimal128(0.3))}, Value("decimal"sv)}});
}

TEST(ExpressionTypeTest, WithLongValue) {
    assertExpectedResults("$type", {{{Value(1LL)}, Value("long"sv)}});
}

TEST(ExpressionTypeTest, WithTimestampValue) {
    assertExpectedResults("$type", {{{Value(Timestamp(0, 0))}, Value("timestamp"sv)}});
}

TEST(ExpressionTypeTest, WithMaxKeyValue) {
    assertExpectedResults("$type", {{{Value(MAXKEY)}, Value("maxKey"sv)}});
}

}  // namespace expression_evaluation_test
}  // namespace mongo
