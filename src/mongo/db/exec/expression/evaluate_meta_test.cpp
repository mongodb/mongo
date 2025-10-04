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

#include "mongo/bson/json.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/expression/evaluate_test_helpers.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace expression_evaluation_test {

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
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
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
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
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
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagStreams", true);

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
    ASSERT_VALUE_EQ(Value(std::vector<Value>{Value("foo"_sd), Value("foo2"_sd)}),
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
    ASSERT_VALUE_EQ(Value("foo-hello"_sd), exp->evaluate(makeDoc(kafkaMeta), &expCtx.variables));
}

TEST(ExpressionMetaTest, ExpressionMetaStreamSerialization) {
    RAIIServerParameterControllerForTest searchHybridScoringPrerequisitesController(
        "featureFlagStreams", true);
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
        expressionMeta->serialize(SerializationOptions{.serializeForCloning = true})
            .getDocument()
            .toBson()
            .firstElement(),
        expCtx2.variablesParseState);

    ASSERT_VALUE_EQ(Value(result), Value(kafkaMeta));
}

TEST(ExpressionTypeTest, WithMinKeyValue) {
    assertExpectedResults("$type", {{{Value(MINKEY)}, Value("minKey"_sd)}});
}

TEST(ExpressionTypeTest, WithDoubleValue) {
    assertExpectedResults("$type", {{{Value(1.0)}, Value("double"_sd)}});
}

TEST(ExpressionTypeTest, WithStringValue) {
    assertExpectedResults("$type", {{{Value("stringValue"_sd)}, Value("string"_sd)}});
}

TEST(ExpressionTypeTest, WithObjectValue) {
    BSONObj objectVal = fromjson("{a: {$literal: 1}}");
    assertExpectedResults("$type", {{{Value(objectVal)}, Value("object"_sd)}});
}

TEST(ExpressionTypeTest, WithArrayValue) {
    assertExpectedResults("$type", {{{Value(BSON_ARRAY(1 << 2))}, Value("array"_sd)}});
}

TEST(ExpressionTypeTest, WithBinDataValue) {
    BSONBinData binDataVal = BSONBinData("", 0, BinDataGeneral);
    assertExpectedResults("$type", {{{Value(binDataVal)}, Value("binData"_sd)}});
}

TEST(ExpressionTypeTest, WithUndefinedValue) {
    assertExpectedResults("$type", {{{Value(BSONUndefined)}, Value("undefined"_sd)}});
}

TEST(ExpressionTypeTest, WithOIDValue) {
    assertExpectedResults("$type", {{{Value(OID())}, Value("objectId"_sd)}});
}

TEST(ExpressionTypeTest, WithBoolValue) {
    assertExpectedResults("$type", {{{Value(true)}, Value("bool"_sd)}});
}

TEST(ExpressionTypeTest, WithDateValue) {
    Date_t dateVal = BSON("" << DATENOW).firstElement().Date();
    assertExpectedResults("$type", {{{Value(dateVal)}, Value("date"_sd)}});
}

TEST(ExpressionTypeTest, WithNullValue) {
    assertExpectedResults("$type", {{{Value(BSONNULL)}, Value("null"_sd)}});
}

TEST(ExpressionTypeTest, WithRegexValue) {
    assertExpectedResults("$type", {{{Value(BSONRegEx("a.b"))}, Value("regex"_sd)}});
}

TEST(ExpressionTypeTest, WithSymbolValue) {
    assertExpectedResults("$type", {{{Value(BSONSymbol("a"))}, Value("symbol"_sd)}});
}

TEST(ExpressionTypeTest, WithDBRefValue) {
    assertExpectedResults("$type", {{{Value(BSONDBRef("", OID()))}, Value("dbPointer"_sd)}});
}

TEST(ExpressionTypeTest, WithCodeWScopeValue) {
    assertExpectedResults(
        "$type",
        {{{Value(BSONCodeWScope("var x = 3", BSONObj()))}, Value("javascriptWithScope"_sd)}});
}

TEST(ExpressionTypeTest, WithCodeValue) {
    assertExpectedResults("$type", {{{Value(BSONCode("var x = 3"))}, Value("javascript"_sd)}});
}

TEST(ExpressionTypeTest, WithIntValue) {
    assertExpectedResults("$type", {{{Value(1)}, Value("int"_sd)}});
}

TEST(ExpressionTypeTest, WithDecimalValue) {
    assertExpectedResults("$type", {{{Value(Decimal128(0.3))}, Value("decimal"_sd)}});
}

TEST(ExpressionTypeTest, WithLongValue) {
    assertExpectedResults("$type", {{{Value(1LL)}, Value("long"_sd)}});
}

TEST(ExpressionTypeTest, WithTimestampValue) {
    assertExpectedResults("$type", {{{Value(Timestamp(0, 0))}, Value("timestamp"_sd)}});
}

TEST(ExpressionTypeTest, WithMaxKeyValue) {
    assertExpectedResults("$type", {{{Value(MAXKEY)}, Value("maxKey"_sd)}});
}

}  // namespace expression_evaluation_test
}  // namespace mongo
