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
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::evaluate_serialize_matcher_test {

namespace {

BSONObj serialize(MatchExpression* match) {
    return match->serialize();
}

}  // namespace

TEST(SerializeBasic, AndExpressionWithOneChildSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$and: [{x: 0}]}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{$and: [{x: {$eq: 0}}]}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 0}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, AndExpressionWithTwoChildrenSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$and: [{x: 1}, {x: 2}]}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{$and: [{x: {$eq: 1}}, {x: {$eq: 2}}]}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: [1, 2]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, AndExpressionWithTwoIdenticalChildrenSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$and: [{x: 1}, {x: 1}]}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{$and: [{x: {$eq: 1}}, {x: {$eq: 1}}]}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: -1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionOr) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$or: [{x: 'A'}, {x: 'B'}]}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{$or: [{x: {$eq: 'A'}}, {x: {$eq: 'B'}}]}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'A'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 'a'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionElemMatchObjectSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$elemMatch: {a: {$gt: 0}, b: {$gt: 0}}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$elemMatch: {$and: [{a: {$gt: 0}}, {b: {$gt: 0}}]}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: [{a: 1, b: -1}, {a: -1, b: 1}]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: [{a: 1, b: 1}, {a: 0, b: 0}]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionElemMatchObjectWithEmptyStringSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{'': {$elemMatch: {a: {$gt: 0}, b: {$gt: 0}}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{'': {$elemMatch: {$and: [{a: {$gt: 0}}, {b: {$gt: 0}}]}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{'': [{a: 1, b: -1}, {a: -1, b: 1}]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{'': [{a: 1, b: 1}, {a: 0, b: 0}]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionElemMatchValueSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$elemMatch: {$lt: 1, $gt: -1}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$elemMatch: {$lt: 1, $gt: -1}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: [{a: 1, b: -1}, {a: -1, b: 1}]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: [{a: 1, b: 1}, {a: 0, b: 0}]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: [1, 0]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionElemMatchValueWithRegexSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    const auto match = BSON("x" << BSON("$elemMatch" << BSON("$regex" << "abc"
                                                                      << "$options"
                                                                      << "i")));
    Matcher original(
        match, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), match);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: ['abc', 'xyz']}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: ['ABC', 'XYZ']}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: ['def', 'xyz']}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionElemMatchValueWithEmptyStringSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$elemMatch: {$lt: 1, $gt: -1}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$elemMatch: {$lt: 1, $gt: -1}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: [{a: 1, b: -1}, {a: -1, b: 1}]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: [{a: 1, b: 1}, {a: 0, b: 0}]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: [1, 0]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionElemMatchValueWithNotEqualSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$elemMatch: {$ne: 1}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$elemMatch: {$not: {$eq: 1}}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: [{a: 1, b: -1}, {a: -1, b: 1}]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: [{a: 1, b: 1}, {a: 0, b: 0}]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: [1, 0]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionElemMatchValueWithNotLessThanGreaterThanSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$elemMatch: {$not: {$lt: 10, $gt: 5}}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$elemMatch: {$not: {$lt: 10, $gt: 5}}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    auto obj = fromjson("{x: [5]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionElemMatchValueWithDoubleNotSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$elemMatch: {$not: {$not: {$eq: 10}}}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$elemMatch: {$not: {$not: {$eq: 10}}}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    auto obj = fromjson("{x: [10]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionElemMatchValueWithNotNESerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$elemMatch: {$not: {$ne: 10}}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$elemMatch: {$not: {$not: {$eq: 10}}}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    auto obj = fromjson("{x: [10]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionElemMatchValueWithTripleNotSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$elemMatch: {$not: {$not: {$not: {$eq: 10}}}}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$elemMatch: {$not: {$not: {$not: {$eq: 10}}}}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    auto obj = fromjson("{x: [10]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionSizeSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$size: 2}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$size: 2}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: [1, 2, 3]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: [1, 2]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionAllSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$all: [1, 2]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{$and: [{x: {$eq: 1}}, {x: {$eq: 2}}]}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: [1, 2, 3]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: [1, 3]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionAllWithEmptyArraySerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$all: []}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), BSON(AlwaysFalseMatchExpression::kName << 1));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: [1, 2, 3]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionAllWithRegex) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$all: [/a.b.c/, /.d.e./]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);

    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      BSON("$and" << BSON_ARRAY(BSON("x" << BSON("$regex" << "a.b.c"))
                                                << BSON("x" << BSON("$regex" << ".d.e.")))));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abcde'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 'adbec'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionEqSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$eq: {a: 1}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$eq: {a: 1}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: {a: 1}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: {a: [1, 2]}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: {a: 2}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionNeSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$ne: {a: 1}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$not: {$eq: {a: 1}}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: {a: 1}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: {a: [1, 2]}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionNeWithRegexObjectSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(BSON("x" << BSON("$ne" << BSON("$regex" << "abc"))),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      BSON("x" << BSON("$not" << BSON("$eq" << BSON("$regex" << "abc")))));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: {a: 1}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: {a: [1, 2]}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionLtSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$lt: 3}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$lt: 3}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 2.9}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionGtSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$gt: 3}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$gt: 3}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 3.1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionGteSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$gte: 3}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$gte: 3}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 2}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionLteSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$lte: 3}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$lte: 3}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionRegexWithObjSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$regex: 'a.b'}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), BSON("x" << BSON("$regex" << "a.b")));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionRegexWithValueAndOptionsSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: /a.b/i}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      BSON("x" << BSON("$regex" << "a.b"
                                                << "$options"
                                                << "i")));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionRegexWithValueSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: /a.b/}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), BSON("x" << BSON("$regex" << "a.b")));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionRegexWithEqObjSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$eq: {$regex: 'a.b'}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$eq: {$regex: 'a.b'}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: /a.b.c/}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionModSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$mod: [2, 1]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$mod: [2, 1]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 2}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionExistsTrueSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$exists: true}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$exists: true}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{a: 1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionExistsFalseSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$exists: false}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$not: {$exists: true}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{a: 1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionInSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$in: [1, 2, 3]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$in: [1, 2, 3]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: [1, 2]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionInWithEmptyArraySerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$in: []}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$in: []}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionInWithRegexSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$in: [/\\d+/, /\\w+/]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$in: [/\\d+/, /\\w+/]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: '1234'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 'abcd'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: '1a2b'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionNinSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$nin: [1, 2, 3]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$not: {$in: [1, 2, 3]}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: [1, 2]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionNinWithRegexValueSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$nin: [/abc/, /def/, /xyz/]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$not: {$in: [/abc/, /def/, /xyz/]}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 'def'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
    obj = fromjson("{x: [/abc/, /def/]}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionBitsAllSetSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$bitsAllSet: [1, 3]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$bitsAllSet: [1, 3]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 2}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 10}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionBitsAllClearSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$bitsAllClear: [1, 3]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$bitsAllClear: [1, 3]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 2}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionBitsAnySetSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$bitsAnySet: [1, 3]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$bitsAnySet: [1, 3]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 2}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionBitsAnyClearSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$bitsAnyClear: [1, 3]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$bitsAnyClear: [1, 3]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 2}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 1}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 10}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionNotSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$not: {$eq: 3}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$not: {$eq: 3}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionNotWithMultipleChildrenSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$not: {$lt: 1, $gt: 3}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{$nor: [{$and: [{x:{$lt: 1}},{x: {$gt: 3}}]}]}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 2}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionNotWithBitTestSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$not: {$bitsAnySet: [1, 3]}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$not: {$bitsAnySet: [1, 3]}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 2}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionNotWithRegexObjSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$not: {$regex: 'a.b'}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      BSON("x" << BSON("$not" << BSON("$regex" << "a.b"))));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionNotWithRegexValueSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$not: /a.b/}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      BSON("x" << BSON("$not" << BSON("$regex" << "a.b"))));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionNotWithRegexValueAndOptionsSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$not: /a.b/i}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      BSON("x" << BSON("$not" << BSON("$regex" << "a.b"
                                                               << "$options"
                                                               << "i"))));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionNotWithGeoSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$not: {$geoIntersects: {$geometry: {type: 'Polygon', "
                              "coordinates: [[[0,0], [5,0], "
                              "[5, 5], [0, 5], [0, 0]]]}}}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$not: {$geoIntersects: {$geometry: {type: 'Polygon', "
                               "coordinates: [[[0, 0], [5, 0], [5, 5], [0, 5], [0, 0]]]}}}}}"));

    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
    BSONObj obj =
        fromjson("{x: {type: 'Polygon', coordinates: [[4, 4], [4, 6], [6, 6], [6, 4], [4, 4]]}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson(
        "{x: {type: 'Polygon', coordinates: [[4, 4], [4, 4.5], [4.5, 4.5], [4.5, 4], [4, 4]]}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson(
        "{x: {type: 'Polygon', coordinates: [[5.5, 5.5], [5.5, 6], [6, 6], [6, 5.5], [5.5, "
        "5.5]]}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionNorSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$nor: [{x: 3}, {x: {$lt: 1}}]}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{$nor: [{x: {$eq: 3}}, {x: {$lt: 1}}]}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 0}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: 2}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionTypeSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$type: 2}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$type: [2]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: '3'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionTypeWithNumberSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$type: 'number'}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$type: ['number']}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: '3'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionTypeWithMultipleTypesSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$type: ['double', 'string', 'object', 'number']}}"), expCtx);
    Matcher reserialized(serialize(original.getMatchExpression()), expCtx);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$type: ['number', 1, 2, 3]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
    obj = fromjson("{x: 'foo'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
    obj = fromjson("{x: []}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
    obj = fromjson("{x: {}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeInternalSchema, InternalSchemaTypeExpressionSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaType: 2}}"), expCtx);
    Matcher reserialized(serialize(original.getMatchExpression()), expCtx);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$_internalSchemaType: [2]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: '3'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeInternalSchema, InternalSchemaTypeExpressionWithNumberSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaType: 'number'}}"), expCtx);
    Matcher reserialized(serialize(original.getMatchExpression()), expCtx);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$_internalSchemaType: ['number']}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: '3'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeInternalSchema, InternalSchemaTypeWithMultipleTypesSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(
        fromjson("{x: {$_internalSchemaType: ['double', 'string', 'object', 'number']}}"), expCtx);
    Matcher reserialized(serialize(original.getMatchExpression()), expCtx);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$_internalSchemaType: ['number', 1, 2, 3]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
    obj = fromjson("{x: 'foo'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
    obj = fromjson("{x: []}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
    obj = fromjson("{x: {}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionEmptySerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionExprSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$expr: {$eq: ['$a', 2]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);

    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{$expr: {$eq: ['$a', {$const: 2}]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{a: 2}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{a: 3}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionInternalExprEqSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{'a.b': {$_internalExprEq: 'foo'}}"), expCtx);
    Matcher reserialized(serialize(original.getMatchExpression()), expCtx);

    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{'a.b': {$_internalExprEq: 'foo'}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{a: {b: 'foo'}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{a: {b: 3}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionCommentSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$comment: 'Hello'}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{a: 1, b: 2}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{a: 'z', b: 'z'}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionGeoWithinSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(
        fromjson(
            "{x: {$geoWithin: {$geometry: "
            "{type: 'Polygon', coordinates: [[[0, 0], [10, 0], [10, 10], [0, 10], [0, 0]]]}}}}"),
        expCtx,
        ExtensionsCallbackNoop(),
        MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(
        *reserialized.getQuery(),
        fromjson("{x: {$geoWithin: {$geometry: {type: 'Polygon', coordinates: [[[0,0], [10,0], "
                 "[10, 10], [0, 10], [0, 0]]]}}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: {type: 'Point', coordinates: [5, 5]}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{x: {type: 'Point', coordinates: [50, 50]}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionGeoIntersectsSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(
        fromjson(
            "{x: {$geoIntersects: {$geometry: {type: 'Polygon', coordinates: [[[0,0], [5,0], [5, "
            "5], [0, 5], [0, 0]]]}}}}"),
        expCtx,
        ExtensionsCallbackNoop(),
        MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(
        *reserialized.getQuery(),
        fromjson("{x: {$geoIntersects: {$geometry: {type: 'Polygon', coordinates: [[[0,0], [5,0], "
                 "[5, 5], [0, 5], [0, 0]]]}}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj =
        fromjson("{x: {type: 'Polygon', coordinates: [[4, 4], [4, 6], [6, 6], [6, 4], [4, 4]]}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson(
        "{x: {type: 'Polygon', coordinates: [[4, 4], [4, 4.5], [4.5, 4.5], [4.5, 4], [4, 4]]}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = fromjson(
        "{x: {type: 'Polygon', coordinates: [[5.5, 5.5], [5.5, 6], [6, 6], [6, 5.5], [5.5, "
        "5.5]]}}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaFmodSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(
        fromjson("{a: {$_internalSchemaFmod: [NumberDecimal('2.3'), NumberDecimal('1.1')]}}"),
        expCtx,
        ExtensionsCallbackNoop(),
        MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);

    ASSERT_BSONOBJ_EQ(
        *reserialized.getQuery(),
        fromjson("{a: {$_internalSchemaFmod: [NumberDecimal('2.3'), NumberDecimal('1.1')]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
    BSONObj obj = fromjson("{a: NumberDecimal('1.1')}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
    obj = fromjson("{a: NumberDecimal('2.3')}");
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeInternalBinDataSubType, ExpressionBinDataSubTypeSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(BSON("x" << BSON("$_internalSchemaBinDataSubType" << BinDataType::Function)),
                     expCtx);
    Matcher reserialized(serialize(original.getMatchExpression()), expCtx);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$_internalSchemaBinDataSubType: 1}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = BSON("x" << BSONBinData(nullptr, 0, BinDataType::bdtCustom));
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    uint8_t bytes[] = {0, 1, 2, 10, 11, 12};
    obj = BSON("x" << BSONBinData(bytes, 5, BinDataType::bdtCustom));
    ASSERT_EQ(exec::matcher::matches(&original, obj), exec::matcher::matches(&reserialized, obj));

    obj = BSON("x" << BSONBinData(bytes, 5, BinDataType::Function));
    ASSERT_TRUE(exec::matcher::matches(&original, obj));
}

}  // namespace mongo::evaluate_serialize_matcher_test
