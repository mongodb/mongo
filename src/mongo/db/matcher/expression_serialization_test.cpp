/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

// Unit tests for MatchExpression::serialize serialization.

#include "mongo/platform/basic.h"

#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::pair;
using std::string;
using std::unique_ptr;

BSONObj serialize(MatchExpression* match) {
    return match->serialize();
}

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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 0}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 2]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: -1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'a'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionOrWithNoChildrenSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // We construct an OrMatchExpression directly rather than using the match expression
    // parser, since the parser does not permit a $or with no children.
    OrMatchExpression original;
    Matcher reserialized(serialize(&original),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), BSON(AlwaysFalseMatchExpression::kName << 1));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [{a: 1, b: 1}, {a: 0, b: 0}]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{'': [{a: 1, b: 1}, {a: 0, b: 0}]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [{a: 1, b: 1}, {a: 0, b: 0}]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 0]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionElemMatchValueWithRegexSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    const auto match = BSON("x" << BSON("$elemMatch" << BSON("$regex"
                                                             << "abc"
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: ['ABC', 'XYZ']}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: ['def', 'xyz']}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [{a: 1, b: 1}, {a: 0, b: 0}]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 0]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [{a: 1, b: 1}, {a: 0, b: 0}]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 0]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 2]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 3]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
                      BSON("$and" << BSON_ARRAY(BSON("x" << BSON("$regex"
                                                                 << "a.b.c"))
                                                << BSON("x" << BSON("$regex"
                                                                    << ".d.e.")))));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abcde'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'adbec'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: {a: [1, 2]}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: {a: 2}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: {a: [1, 2]}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionNeWithRegexObjectSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(BSON("x" << BSON("$ne" << BSON("$regex"
                                                    << "abc"))),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      BSON("x" << BSON("$not" << BSON("$eq" << BSON("$regex"
                                                                    << "abc")))));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: {a: 1}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: {a: [1, 2]}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 2.9}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 3.1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 2}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      BSON("x" << BSON("$regex"
                                       << "a.b")));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
                      BSON("x" << BSON("$regex"
                                       << "a.b"
                                       << "$options"
                                       << "i")));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      BSON("x" << BSON("$regex"
                                       << "a.b")));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: /a.b.c/}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 2}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{a: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{a: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 2]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'abcd'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: '1a2b'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 2]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'def'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
    obj = fromjson("{x: [/abc/, /def/]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 10}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 10}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
                      BSON("x" << BSON("$not" << BSON("$regex"
                                                      << "a.b"))));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
                      BSON("x" << BSON("$not" << BSON("$regex"
                                                      << "a.b"))));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
                      BSON("x" << BSON("$not" << BSON("$regex"
                                                      << "a.b"
                                                      << "$options"
                                                      << "i"))));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson(
        "{x: {type: 'Polygon', coordinates: [[4, 4], [4, 4.5], [4.5, 4.5], [4.5, 4], [4, 4]]}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson(
        "{x: {type: 'Polygon', coordinates: [[5.5, 5.5], [5.5, 6], [6, 6], [6, 5.5], [5.5, "
        "5.5]]}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionNotWithDirectPathExpSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // At the time of this writing, the MatchExpression parser does not ever create a NOT with a
    // direct path expression child, instead creating a NOT -> AND -> path expression. This test
    // manually constructs such an expression in case it ever turns up, since that should still be
    // able to serialize.
    auto originalBSON = fromjson("{a: {$not: {$eq: 2}}}}");
    auto equalityRHSElem = originalBSON["a"]["$not"]["$eq"];
    auto equalityExpression = std::make_unique<EqualityMatchExpression>("a"_sd, equalityRHSElem);

    auto notExpression = std::make_unique<NotMatchExpression>(equalityExpression.release());
    Matcher reserialized(serialize(notExpression.get()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), originalBSON);

    auto obj = fromjson("{a: 2}");
    ASSERT_EQ(notExpression->matchesBSON(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionNotNotDirectlySerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // At the time of this writing, the MatchExpression parser does not ever create a NOT with a
    // direct NOT child, instead creating a NOT -> AND -> NOT. This test manually constructs such an
    // expression in case it ever turns up, since that should still be able to serialize to
    // {$not: {$not: ...}}.
    auto originalBSON = fromjson("{a: {$not: {$not: {$eq: 2}}}}");
    auto equalityRHSElem = originalBSON["a"]["$not"]["$not"]["$eq"];
    auto equalityExpression = std::make_unique<EqualityMatchExpression>("a"_sd, equalityRHSElem);

    auto nestedNot = std::make_unique<NotMatchExpression>(equalityExpression.release());
    auto topNot = std::make_unique<NotMatchExpression>(nestedNot.release());
    Matcher reserialized(serialize(topNot.get()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{$nor: [{a: {$not: {$eq: 2}}}]}"));

    auto obj = fromjson("{a: 2}");
    ASSERT_EQ(topNot->matchesBSON(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionNotWithoutPathChildrenSerializesCorrectly) {
    // The grammar only permits a $not under a given path. For example, {a: {$not: {$eq: 4}}} is OK
    // but {$not: {a: 4}} is not OK). However, we sometimes use the NOT MatchExpression to negate
    // clauses within a JSONSchema. In such circumstances we need to be able to serialize the tree
    // and re-parse it but the parser will reject the NOT in the place it's in. As a result, we need
    // to translate the NOT to a $nor.

    // MatchExpression tree expected:
    //  {$or: [
    //    {$and: [
    //      {foo: {$_internalSchemaType: [2]}},
    //      {foo: {$not: {
    //        // This whole $or represents the {maxLength: 4}, since the restriction only applies if
    //        // the element is the right type.
    //        $or: [
    //          {$_internalSchemaMaxLength: 4},
    //          {foo: {$not: {$_internalSchemaType: [2]}}}
    //        ]
    //      }}}
    //    ]},
    //    {foo: {$not: {$exists: true}}}
    //  ]}
    BSONObj query =
        fromjson("{$jsonSchema: {properties: {foo: {type: 'string', not: {maxLength: 4}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expression = unittest::assertGet(MatchExpressionParser::parse(query, expCtx));

    Matcher reserialized(serialize(expression.get()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);

    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{$and: ["
                               "  {$and: ["
                               "    {$or: ["
                               "      {foo: {$not: {$exists: true}}},"
                               "      {$and: ["
                               "        {$nor: ["  // <-- This is the interesting part of this test.
                               "          {$or: ["
                               "            {foo: {$not: {$_internalSchemaType: [2]}}},"
                               "            {foo: {$_internalSchemaMaxLength: 4}}"
                               "          ]}"
                               "        ]},"
                               "        {foo: {$_internalSchemaType: [2]}}"
                               "      ]}"
                               "    ]}"
                               "  ]}"
                               "]}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{foo: 'abc'}");
    ASSERT_EQ(expression->matchesBSON(obj), reserialized.matches(obj));

    obj = fromjson("{foo: 'acbdf'}");
    ASSERT_EQ(expression->matchesBSON(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 0}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 2}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: '3'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: '3'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionTypeWithMultipleTypesSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$type: ['double', 'string', 'object', 'number']}}"), expCtx);
    Matcher reserialized(serialize(original.getMatchExpression()), expCtx);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$type: ['number', 1, 2, 3]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
    obj = fromjson("{x: 'foo'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
    obj = fromjson("{x: []}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
    obj = fromjson("{x: {}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeInternalSchema, InternalSchemaTypeExpressionSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaType: 2}}"), expCtx);
    Matcher reserialized(serialize(original.getMatchExpression()), expCtx);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$_internalSchemaType: [2]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: '3'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeInternalSchema, InternalSchemaTypeExpressionWithNumberSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaType: 'number'}}"), expCtx);
    Matcher reserialized(serialize(original.getMatchExpression()), expCtx);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$_internalSchemaType: ['number']}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: '3'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
    obj = fromjson("{x: 'foo'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
    obj = fromjson("{x: []}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
    obj = fromjson("{x: {}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionWhereSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$where: 'this.a == this.b'}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      BSONObjBuilder().appendCode("$where", "this.a == this.b").obj());
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{a: 3}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionInternalExprEqSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{'a.b': {$_internalExprEq: 'foo'}}"), expCtx);
    Matcher reserialized(serialize(original.getMatchExpression()), expCtx);

    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{'a.b': {$_internalExprEq: 'foo'}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{a: {b: 'foo'}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{a: {b: 3}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{a: 'z', b: 'z'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: {type: 'Point', coordinates: [50, 50]}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson(
        "{x: {type: 'Polygon', coordinates: [[4, 4], [4, 4.5], [4.5, 4.5], [4.5, 4], [4, 4]]}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson(
        "{x: {type: 'Polygon', coordinates: [[5.5, 5.5], [5.5, 6], [6, 6], [6, 5.5], [5.5, "
        "5.5]]}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionNearSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(
        fromjson("{x: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}, $maxDistance: 10, "
                 "$minDistance: 1}}}"),
        expCtx,
        ExtensionsCallbackNoop(),
        MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(
        *reserialized.getQuery(),
        fromjson("{x: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}, $maxDistance: 10, "
                 "$minDistance: 1}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionNearSphereSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(
        fromjson(
            "{x: {$nearSphere: {$geometry: {type: 'Point', coordinates: [0, 0]}, $maxDistance: 10, "
            "$minDistance: 1}}}"),
        expCtx,
        ExtensionsCallbackNoop(),
        MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(
        *reserialized.getQuery(),
        fromjson("{x: {$nearSphere: {$geometry: {type: 'Point', coordinates: [0, 0]}, "
                 "$maxDistance: 10, $minDistance: 1}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionTextSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$text: {$search: 'a', $language: 'en', $caseSensitive: true}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{$text: {$search: 'a', $language: 'en', $caseSensitive: true, "
                               "$diacriticSensitive: false}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionNorWithTextSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$nor: [{$text: {$search: 'x'}}]}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{$nor: [{$text: {$search: 'x', $language: '', $caseSensitive: "
                               "false, $diacriticSensitive: false}}]}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionTextWithDefaultLanguageSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$text: {$search: 'a', $caseSensitive: false}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{$text: {$search: 'a', $language: '', $caseSensitive: false, "
                               "$diacriticSensitive: false}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionAlwaysTrueSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(BSON(AlwaysTrueMatchExpression::kName << 1),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), BSON(AlwaysTrueMatchExpression::kName << 1));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionAlwaysFalseSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(BSON(AlwaysFalseMatchExpression::kName << 1),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), BSON(AlwaysFalseMatchExpression::kName << 1));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaAllElemMatchFromIndexSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaAllElemMatchFromIndex: [2, {y: 1}]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$_internalSchemaAllElemMatchFromIndex: [2, {y: {$eq: 1}}]}}"));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaMinItemsSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaMinItems: 1}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$_internalSchemaMinItems: 1}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaMaxItemsSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaMaxItems: 1}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$_internalSchemaMaxItems: 1}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaUniqueItemsSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaUniqueItems: true}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$_internalSchemaUniqueItems: true}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaObjectMatchSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaObjectMatch: {y: 1}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{x: {$_internalSchemaObjectMatch: {y: {$eq: 1}}}}"));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaMinLengthSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaMinLength: 1}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$_internalSchemaMinLength: 1}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaMaxLengthSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaMaxLength: 1}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$_internalSchemaMaxLength: 1}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaCondSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$_internalSchemaCond: [{a: 1}, {b: 2}, {c: 3}]}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    BSONObjBuilder builder;
    ASSERT_BSONOBJ_EQ(
        *reserialized.getQuery(),
        fromjson("{$_internalSchemaCond: [{a: {$eq: 1}}, {b: {$eq: 2}}, {c: {$eq: 3}}]}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaMinPropertiesSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$_internalSchemaMinProperties: 1}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{$_internalSchemaMinProperties: 1}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaMaxPropertiesSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$_internalSchemaMaxProperties: 1}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{$_internalSchemaMaxProperties: 1}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
    obj = fromjson("{a: NumberDecimal('2.3')}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaMatchArrayIndexSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{a: {$_internalSchemaMatchArrayIndex:"
                              "{index: 2, namePlaceholder: 'i', expression: {i: {$lt: 3}}}}}"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{a: {$_internalSchemaMatchArrayIndex:"
                               "{index: 2, namePlaceholder: 'i', expression: {i: {$lt: 3}}}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaAllowedPropertiesSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson(R"({$_internalSchemaAllowedProperties: {
            properties: ['a'],
            otherwise: {i: {$gt: 10}},
            namePlaceholder: 'i',
            patternProperties: [{regex: /b/, expression: {i: {$type: 'number'}}}]
        }})"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson(R"({$_internalSchemaAllowedProperties: {
            properties: ['a'],
            namePlaceholder: 'i',
            patternProperties: [{regex: /b/, expression: {i: {$type: ['number']}}}],
            otherwise: {i: {$gt: 10}}
        }})"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema,
     ExpressionInternalSchemaAllowedPropertiesEmptyOtherwiseSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson(R"({$_internalSchemaAllowedProperties: {
            properties: [],
            otherwise: {},
            namePlaceholder: 'i',
            patternProperties: []
        }})"),
                     expCtx,
                     ExtensionsCallbackNoop(),
                     MatchExpressionParser::kAllowAllSpecialFeatures);
    Matcher reserialized(serialize(original.getMatchExpression()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson(R"({$_internalSchemaAllowedProperties: {
            properties: [],
            namePlaceholder: 'i',
            patternProperties: [],
            otherwise: {}
        }})"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaEqSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{x: {$_internalSchemaEq: {y: 1}}}"), expCtx);
    Matcher reserialized(serialize(original.getMatchExpression()), expCtx);

    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{x: {$_internalSchemaEq: {y: 1}}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeInternalSchema, ExpressionInternalSchemaRootDocEqSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher original(fromjson("{$_internalSchemaRootDocEq: {y: 1}}"), expCtx);
    Matcher reserialized(serialize(original.getMatchExpression()), expCtx);

    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{$_internalSchemaRootDocEq: {y: 1}}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
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
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    uint8_t bytes[] = {0, 1, 2, 10, 11, 12};
    obj = BSON("x" << BSONBinData(bytes, 5, BinDataType::bdtCustom));
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = BSON("x" << BSONBinData(bytes, 5, BinDataType::Function));
    ASSERT_TRUE(original.matches(obj));
}

std::string redactFieldNameForTest(StringData s) {
    return str::stream() << "HASH(" << s << ")";
}
TEST(SerializeInternalSchema, AllowedPropertiesRedactsCorrectly) {

    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['a', 'b'],"
        "namePlaceholder: 'i', patternProperties: [], otherwise: {i: 0}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    SerializationOptions opts;
    opts.redactIdentifiers = true;
    opts.identifierRedactionPolicy = redactFieldNameForTest;
    opts.replacementForLiteralArgs = "?";

    ASSERT_BSONOBJ_EQ(
        fromjson(
            "{ $_internalSchemaAllowedProperties: { properties: \"?\", namePlaceholder: \"?\", "
            "patternProperties: [], otherwise: { \"HASH(i)\": { $eq: \"?\" } } } }"),
        objMatch.getValue()->serialize(opts));
}

/**
 * Helper function for parsing and creating MatchExpressions.
 */
std::unique_ptr<InternalSchemaCondMatchExpression> createCondMatchExpression(BSONObj condition,
                                                                             BSONObj thenBranch,
                                                                             BSONObj elseBranch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto conditionExpr = MatchExpressionParser::parse(condition, expCtx);
    ASSERT_OK(conditionExpr.getStatus());
    auto thenBranchExpr = MatchExpressionParser::parse(thenBranch, expCtx);
    ASSERT_OK(thenBranchExpr.getStatus());
    auto elseBranchExpr = MatchExpressionParser::parse(elseBranch, expCtx);

    std::array<std::unique_ptr<MatchExpression>, 3> expressions = {
        {std::move(conditionExpr.getValue()),
         std::move(thenBranchExpr.getValue()),
         std::move(elseBranchExpr.getValue())}};

    auto cond = std::make_unique<InternalSchemaCondMatchExpression>(std::move(expressions));

    return cond;
}

TEST(SerializeInternalSchema, CondMatchRedactsCorrectly) {
    SerializationOptions opts;
    opts.redactIdentifiers = true;
    opts.replacementForLiteralArgs = "?";
    opts.identifierRedactionPolicy = redactFieldNameForTest;
    auto conditionQuery = BSON("age" << BSON("$lt" << 18));
    auto thenQuery = BSON("job"
                          << "student");
    auto elseQuery = BSON("job"
                          << "engineer");
    auto cond = createCondMatchExpression(conditionQuery, thenQuery, elseQuery);
    BSONObjBuilder bob;
    cond->serialize(&bob, opts);
    auto expectedResult =
        BSON("$_internalSchemaCond" << BSON_ARRAY(BSON("HASH(age)" << BSON("$lt"
                                                                           << "?"))
                                                  << BSON("HASH(job)" << BSON("$eq"
                                                                              << "?"))
                                                  << BSON("HASH(job)" << BSON("$eq"
                                                                              << "?"))));
    ASSERT_BSONOBJ_EQ(expectedResult, bob.done());
}

TEST(SerializeInternalSchema, FmodMatchRedactsCorrectly) {
    InternalSchemaFmodMatchExpression m("a"_sd, Decimal128(1.7), Decimal128(2));
    SerializationOptions opts;
    opts.replacementForLiteralArgs = "?";
    BSONObjBuilder bob;
    m.serialize(&bob, opts);
    ASSERT_BSONOBJ_EQ(BSON("a" << BSON("$_internalSchemaFmod" << BSON_ARRAY("?"
                                                                            << "?"))),
                      bob.done());
}

TEST(SerializeInternalSchema, MatchArrayIndexRedactsCorrectly) {
    auto query = fromjson(
        "{foo: {$_internalSchemaMatchArrayIndex:"
        "{index: 0, namePlaceholder: 'i', expression: {i: {$type: 'number'}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    BSONObjBuilder bob;
    SerializationOptions opts;
    opts.redactIdentifiers = true;
    opts.identifierRedactionPolicy = redactFieldNameForTest;
    opts.replacementForLiteralArgs = "?";
    objMatch.getValue()->serialize(&bob, opts);

    ASSERT_BSONOBJ_EQ(bob.done(),
                      BSON("HASH(foo)" << BSON("$_internalSchemaMatchArrayIndex"
                                               << BSON("index"
                                                       << "?"
                                                       << "namePlaceholder"
                                                       << "HASH(i)"
                                                       << "expression"
                                                       << BSON("HASH(i)" << BSON("$type"
                                                                                 << "?"))))));
}

TEST(SerializeInternalSchema, MaxItemsRedactsCorrectly) {
    InternalSchemaMaxItemsMatchExpression maxItems("a.b"_sd, 2);
    SerializationOptions opts;
    opts.redactIdentifiers = true;
    opts.replacementForLiteralArgs = "?";
    opts.identifierRedactionPolicy = redactFieldNameForTest;

    ASSERT_BSONOBJ_EQ(maxItems.getSerializedRightHandSide(opts),
                      BSON("$_internalSchemaMaxItems"
                           << "?"));
}

TEST(SerializeInternalSchema, MaxLengthRedactsCorrectly) {
    InternalSchemaMaxLengthMatchExpression maxLength("a"_sd, 2);
    SerializationOptions opts;
    opts.replacementForLiteralArgs = "?";
    ASSERT_BSONOBJ_EQ(maxLength.getSerializedRightHandSide(opts),
                      BSON("$_internalSchemaMaxLength"
                           << "?"));
}

TEST(SerializeInternalSchema, MinItemsRedactsCorrectly) {
    InternalSchemaMinItemsMatchExpression minItems("a.b"_sd, 2);
    SerializationOptions opts;
    opts.redactIdentifiers = true;
    opts.replacementForLiteralArgs = "?";
    opts.identifierRedactionPolicy = redactFieldNameForTest;

    ASSERT_BSONOBJ_EQ(minItems.getSerializedRightHandSide(opts),
                      BSON("$_internalSchemaMinItems"
                           << "?"));
}

TEST(SerializeInternalSchema, MinLengthRedactsCorrectly) {
    InternalSchemaMinLengthMatchExpression minLength("a"_sd, 2);
    SerializationOptions opts;
    opts.replacementForLiteralArgs = "?";
    ASSERT_BSONOBJ_EQ(minLength.getSerializedRightHandSide(opts),
                      BSON("$_internalSchemaMinLength"
                           << "?"));
}

TEST(SerializeInternalSchema, MinPropertiesRedactsCorrectly) {
    InternalSchemaMinPropertiesMatchExpression minProperties(5);
    SerializationOptions opts;
    opts.replacementForLiteralArgs = "?";

    BSONObjBuilder bob;
    minProperties.serialize(&bob, opts);
    ASSERT_BSONOBJ_EQ(bob.done(),
                      BSON("$_internalSchemaMinProperties"
                           << "?"));
}

TEST(SerializeInternalSchema, ObjectMatchRedactsCorrectly) {
    SerializationOptions opts;
    opts.redactIdentifiers = true;
    opts.identifierRedactionPolicy = redactFieldNameForTest;
    opts.replacementForLiteralArgs = "?";
    auto query = fromjson(
        "    {a: {$_internalSchemaObjectMatch: {"
        "        c: {$eq: 3}"
        "    }}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_BSONOBJ_EQ(
        objMatch.getValue()->serialize(opts),
        BSON("HASH(a)" << BSON("$_internalSchemaObjectMatch" << BSON("HASH(c)" << BSON("$eq"
                                                                                       << "?")))));
}

TEST(SerializeInternalSchema, RootDocEqRedactsCorrectly) {
    auto query = fromjson("{$_internalSchemaRootDocEq: {a:1, b: {c: 1, d: [1]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SerializationOptions opts;
    opts.redactIdentifiers = true;
    opts.identifierRedactionPolicy = redactFieldNameForTest;
    opts.replacementForLiteralArgs = "?";
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_BSONOBJ_EQ(
        objMatch.getValue()->serialize(opts),
        BSON("$_internalSchemaRootDocEq" << BSON("HASH(a)"
                                                 << "?"
                                                 << "HASH(b)"
                                                 << BSON("HASH(c)"
                                                         << "?"
                                                         << "HASH(d)" << BSON_ARRAY("?")))));
}

TEST(SerializeInternalSchema, BinDataEncryptedTypeRedactsCorrectly) {
    MatcherTypeSet typeSet;
    typeSet.bsonTypes.insert(BSONType::String);
    typeSet.bsonTypes.insert(BSONType::Date);
    InternalSchemaBinDataEncryptedTypeExpression e("a"_sd, std::move(typeSet));
    SerializationOptions opts;
    opts.replacementForLiteralArgs = "?";
    ASSERT_BSONOBJ_EQ(BSON("$_internalSchemaBinDataEncryptedType"
                           << "?"),
                      e.getSerializedRightHandSide(opts));
}

TEST(SerializeInternalSchema, BinDataFLE2EncryptedTypeRedactsCorrectly) {
    InternalSchemaBinDataFLE2EncryptedTypeExpression e("ssn"_sd, BSONType::String);
    SerializationOptions opts;
    opts.replacementForLiteralArgs = "?";
    ASSERT_BSONOBJ_EQ(BSON("$_internalSchemaBinDataFLE2EncryptedType"
                           << "?"),
                      e.getSerializedRightHandSide(opts));
}

TEST(SerializesInternalSchema, MaxPropertiesRedactsCorrectly) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties(5);
    SerializationOptions opts;
    opts.replacementForLiteralArgs = "?";

    BSONObjBuilder bob;
    maxProperties.serialize(&bob, opts);
    ASSERT_BSONOBJ_EQ(bob.done(),
                      BSON("$_internalSchemaMaxProperties"
                           << "?"));
}

TEST(SerializesInternalSchema, EqRedactsCorrectly) {
    SerializationOptions opts;
    opts.identifierRedactionPolicy = redactFieldNameForTest;
    opts.redactIdentifiers = true;
    opts.replacementForLiteralArgs = "?";
    auto query = fromjson("{$_internalSchemaEq: {a:1, b: {c: 1, d: [1]}}}");
    BSONObjBuilder bob;
    InternalSchemaEqMatchExpression e("a"_sd, query.firstElement());
    e.serialize(&bob, opts);
    ASSERT_BSONOBJ_EQ(bob.done(),
                      BSON("HASH(a)" << BSON("$_internalSchemaEq"
                                             << BSON("HASH(a)"
                                                     << "?"
                                                     << "HASH(b)"
                                                     << BSON("HASH(c)"
                                                             << "?"
                                                             << "HASH(d)" << BSON_ARRAY("?"))))));
}

TEST(InternalSchemaAllElemMatchFromIndexMatchExpression, RedactsExpressionCorrectly) {
    auto query = fromjson("{a: {$_internalSchemaAllElemMatchFromIndex: [2, {a: {$lt: 5}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());
    auto elemMatchExpr = dynamic_cast<const InternalSchemaAllElemMatchFromIndexMatchExpression*>(
        expr.getValue().get());

    SerializationOptions opts;
    opts.redactIdentifiers = true;
    opts.identifierRedactionPolicy = redactFieldNameForTest;
    opts.replacementForLiteralArgs = "?";

    ASSERT_BSONOBJ_EQ(BSON("$_internalSchemaAllElemMatchFromIndex"
                           << BSON_ARRAY("?" << BSON("HASH(a)" << BSON("$lt"
                                                                       << "?")))),
                      elemMatchExpr->getSerializedRightHandSide(opts));
}
}  // namespace
}  // namespace mongo
