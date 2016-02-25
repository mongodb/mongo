/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::pair;
using std::string;
using std::unique_ptr;

BSONObj serialize(MatchExpression* match) {
    BSONObjBuilder bob;
    match->serialize(&bob);
    return bob.obj();
}

TEST(SerializeBasic, AndExpressionWithOneChildSerializesCorrectly) {
    Matcher original(fromjson("{$and: [{x: 0}]}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$and: [{x: {$eq: 0}}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 0}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, AndExpressionWithTwoChildrenSerializesCorrectly) {
    Matcher original(fromjson("{$and: [{x: 1}, {x: 2}]}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$and: [{x: {$eq: 1}}, {x: {$eq: 2}}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 2]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, AndExpressionWithTwoIdenticalChildrenSerializesCorrectly) {
    Matcher original(fromjson("{$and: [{x: 1}, {x: 1}]}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$and: [{x: {$eq: 1}}, {x: {$eq: 1}}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: -1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionOr) {
    Matcher original(fromjson("{$or: [{x: 'A'}, {x: 'B'}]}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$or: [{x: {$eq: 'A'}}, {x: {$eq: 'B'}}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'A'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'a'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionElemMatchObjectSerializesCorrectly) {
    Matcher original(fromjson("{x: {$elemMatch: {a: {$gt: 0}, b: {$gt: 0}}}}"),
                     ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(),
              fromjson("{x: {$elemMatch: {$and: [{a: {$gt: 0}}, {b: {$gt: 0}}]}}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: [{a: 1, b: -1}, {a: -1, b: 1}]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [{a: 1, b: 1}, {a: 0, b: 0}]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionElemMatchObjectWithEmptyStringSerializesCorrectly) {
    Matcher original(fromjson("{'': {$elemMatch: {a: {$gt: 0}, b: {$gt: 0}}}}"),
                     ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(),
              fromjson("{'': {$elemMatch: {$and: [{a: {$gt: 0}}, {b: {$gt: 0}}]}}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{'': [{a: 1, b: -1}, {a: -1, b: 1}]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{'': [{a: 1, b: 1}, {a: 0, b: 0}]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionElemMatchValueSerializesCorrectly) {
    Matcher original(fromjson("{x: {$elemMatch: {$lt: 1, $gt: -1}}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$elemMatch: {$lt: 1, $gt: -1}}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: [{a: 1, b: -1}, {a: -1, b: 1}]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [{a: 1, b: 1}, {a: 0, b: 0}]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 0]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionElemMatchValueWithEmptyStringSerializesCorrectly) {
    Matcher original(fromjson("{x: {$elemMatch: {$lt: 1, $gt: -1}}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$elemMatch: {$lt: 1, $gt: -1}}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: [{a: 1, b: -1}, {a: -1, b: 1}]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [{a: 1, b: 1}, {a: 0, b: 0}]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 0]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionSizeSerializesCorrectly) {
    Matcher original(fromjson("{x: {$size: 2}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$size: 2}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: [1, 2, 3]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 2]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionAllSerializesCorrectly) {
    Matcher original(fromjson("{x: {$all: [1, 2]}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$and: [{x: {$eq: 1}}, {x: {$eq: 2}}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: [1, 2, 3]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 3]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionAllWithEmptyArraySerializesCorrectly) {
    Matcher original(fromjson("{x: {$all: []}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$all: []}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: [1, 2, 3]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionAllWithRegex) {
    Matcher original(fromjson("{x: {$all: [/a.b.c/, /.d.e./]}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$and: [{x: /a.b.c/}, {x: /.d.e./}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abcde'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'adbec'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionEqSerializesCorrectly) {
    Matcher original(fromjson("{x: {$eq: {a: 1}}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$eq: {a: 1}}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: {a: 1}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: {a: [1, 2]}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: {a: 2}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionNeSerializesCorrectly) {
    Matcher original(fromjson("{x: {$ne: {a: 1}}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$nor: [{x: {$eq: {a: 1}}}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: {a: 1}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: {a: [1, 2]}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionLtSerializesCorrectly) {
    Matcher original(fromjson("{x: {$lt: 3}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$lt: 3}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 2.9}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionGtSerializesCorrectly) {
    Matcher original(fromjson("{x: {$gt: 3}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$gt: 3}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 3.1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionGteSerializesCorrectly) {
    Matcher original(fromjson("{x: {$gte: 3}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$gte: 3}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 2}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionLteSerializesCorrectly) {
    Matcher original(fromjson("{x: {$lte: 3}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$lte: 3}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionRegexWithObjSerializesCorrectly) {
    Matcher original(fromjson("{x: {$regex: 'a.b'}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$regex: 'a.b'}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionRegexWithValueSerializesCorrectly) {
    Matcher original(fromjson("{x: /a.b/i}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$regex: 'a.b', $options: 'i'}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionRegexWithValueAndOptionsSerializesCorrectly) {
    Matcher original(fromjson("{x: /a.b/}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$regex: 'a.b'}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionRegexWithEqObjSerializesCorrectly) {
    Matcher original(fromjson("{x: {$eq: {$regex: 'a.b'}}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$eq: {$regex: 'a.b'}}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: /a.b.c/}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionModSerializesCorrectly) {
    Matcher original(fromjson("{x: {$mod: [2, 1]}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$mod: [2, 1]}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 2}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionExistsTrueSerializesCorrectly) {
    Matcher original(fromjson("{x: {$exists: true}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$exists: true}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{a: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionExistsFalseSerializesCorrectly) {
    Matcher original(fromjson("{x: {$exists: false}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$nor: [{x: {$exists: true}}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{a: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionInSerializesCorrectly) {
    Matcher original(fromjson("{x: {$in: [1, 2, 3]}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$in: [1, 2, 3]}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 2]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionInWithEmptyArraySerializesCorrectly) {
    Matcher original(fromjson("{x: {$in: []}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$in: []}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionInWithRegexSerializesCorrectly) {
    Matcher original(fromjson("{x: {$in: [/\\d+/, /\\w+/]}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$in: [/\\d+/, /\\w+/]}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: '1234'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'abcd'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: '1a2b'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionNinSerializesCorrectly) {
    Matcher original(fromjson("{x: {$nin: [1, 2, 3]}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$nor: [{x: {$in: [1, 2, 3]}}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: [1, 2]}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionBitsAllSetSerializesCorrectly) {
    Matcher original(fromjson("{x: {$bitsAllSet: [1, 3]}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$bitsAllSet: [1, 3]}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 2}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 10}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionBitsAllClearSerializesCorrectly) {
    Matcher original(fromjson("{x: {$bitsAllClear: [1, 3]}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$bitsAllClear: [1, 3]}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 2}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionBitsAnySetSerializesCorrectly) {
    Matcher original(fromjson("{x: {$bitsAnySet: [1, 3]}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$bitsAnySet: [1, 3]}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 2}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionBitsAnyClearSerializesCorrectly) {
    Matcher original(fromjson("{x: {$bitsAnyClear: [1, 3]}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$bitsAnyClear: [1, 3]}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 2}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 1}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 10}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionNotSerializesCorrectly) {
    Matcher original(fromjson("{x: {$not: {$eq: 3}}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$nor: [{$and: [{x: {$eq: 3}}]}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionNotWithMultipleChildrenSerializesCorrectly) {
    Matcher original(fromjson("{x: {$not: {$lt: 1, $gt: 3}}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(),
              fromjson("{$nor: [{$and: [{x: {$lt: 1}}, {x: {$gt: 3}}]}]}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 2}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionNotWithBitTestSerializesCorrectly) {
    Matcher original(fromjson("{x: {$not: {$bitsAnySet: [1, 3]}}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$nor: [{$and: [{x: {$bitsAnySet: [1, 3]}}]}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 2}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 4}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionNotWithRegexObjSerializesCorrectly) {
    Matcher original(fromjson("{x: {$not: {$regex: 'a.b'}}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$nor: [{x: /a.b/}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionNotWithRegexValueSerializesCorrectly) {
    Matcher original(fromjson("{x: {$not: /a.b/}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$nor: [{x: /a.b/}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionNotWithRegexValueAndOptionsSerializesCorrectly) {
    Matcher original(fromjson("{x: {$not: /a.b/i}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$nor: [{x: /a.b/i}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 'abc'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 'acb'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionNotWithGeoSerializesCorrectly) {
    Matcher original(fromjson(
                         "{x: {$not: {$geoIntersects: {$geometry: {type: 'Polygon', "
                         "coordinates: [[[0,0], [5,0], "
                         "[5, 5], [0, 5], [0, 0]]]}}}}}"),
                     ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(),
              fromjson(
                  "{$nor: [{$and: [{x: {$geoIntersects: {$geometry: {type: 'Polygon', coordinates: "
                  "[[[0,0], "
                  "[5,0], [5, 5], [0, 5], [0, 0]]]}}}}]}]}"));

    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
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

TEST(SerializeBasic, ExpressionNorSerializesCorrectly) {
    Matcher original(fromjson("{$nor: [{x: 3}, {x: {$lt: 1}}]}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{$nor: [{x: {$eq: 3}}, {x: {$lt: 1}}]}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 0}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: 2}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionTypeSerializesCorrectly) {
    Matcher original(fromjson("{x: {$type: 2}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$type: 2}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: '3'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionTypeWithNumberSerializesCorrectly) {
    Matcher original(fromjson("{x: {$type: 'number'}}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{x: {$type: 'number'}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: '3'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionEmptySerializesCorrectly) {
    Matcher original(fromjson("{}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: 3}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionWhereSerializesCorrectly) {
    Matcher original(fromjson("{$where: 'this.a == this.b'}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(),
              BSONObjBuilder().appendCodeWScope("$where", "this.a == this.b", BSONObj()).obj());
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionWhereWithScopeSerializesCorrectly) {
    Matcher original(BSON("$where" << BSONCodeWScope("this.a == this.b", BSON("x" << 3))),
                     ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(),
              BSON("$where" << BSONCodeWScope("this.a == this.b", BSON("x" << 3))));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionCommentSerializesCorrectly) {
    Matcher original(fromjson("{$comment: 'Hello'}"), ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(), fromjson("{}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{a: 1, b: 2}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{a: 'z', b: 'z'}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionGeoWithinSerializesCorrectly) {
    Matcher original(
        fromjson(
            "{x: {$geoWithin: {$geometry: "
            "{type: 'Polygon', coordinates: [[[0, 0], [10, 0], [10, 10], [0, 10], [0, 0]]]}}}}"),
        ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(),
              fromjson(
                  "{x: {$geoWithin: {$geometry: {type: 'Polygon', coordinates: [[[0,0], [10,0], "
                  "[10, 10], [0, 10], [0, 0]]]}}}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{x: {type: 'Point', coordinates: [5, 5]}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));

    obj = fromjson("{x: {type: 'Point', coordinates: [50, 50]}}");
    ASSERT_EQ(original.matches(obj), reserialized.matches(obj));
}

TEST(SerializeBasic, ExpressionGeoIntersectsSerializesCorrectly) {
    Matcher original(
        fromjson(
            "{x: {$geoIntersects: {$geometry: {type: 'Polygon', coordinates: [[[0,0], [5,0], [5, "
            "5], [0, 5], [0, 0]]]}}}}"),
        ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(),
              fromjson(
                  "{x: {$geoIntersects: {$geometry: {type: 'Polygon', coordinates: [[[0,0], [5,0], "
                  "[5, 5], [0, 5], [0, 0]]]}}}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

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
    Matcher original(
        fromjson(
            "{x: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}, $maxDistance: 10, "
            "$minDistance: 1}}}"),
        ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(),
              fromjson(
                  "{x: {$near: {$geometry: {type: 'Point', coordinates: [0, 0]}, $maxDistance: 10, "
                  "$minDistance: 1}}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionNearSphereSerializesCorrectly) {
    Matcher original(
        fromjson(
            "{x: {$nearSphere: {$geometry: {type: 'Point', coordinates: [0, 0]}, $maxDistance: 10, "
            "$minDistance: 1}}}"),
        ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(),
              fromjson(
                  "{x: {$nearSphere: {$geometry: {type: 'Point', coordinates: [0, 0]}, "
                  "$maxDistance: 10, $minDistance: 1}}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionTextSerializesCorrectly) {
    Matcher original(fromjson("{$text: {$search: 'a', $language: 'en', $caseSensitive: true}}"),
                     ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(),
              fromjson(
                  "{$text: {$search: 'a', $language: 'en', $caseSensitive: true, "
                  "$diacriticSensitive: false}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

TEST(SerializeBasic, ExpressionTextWithDefaultLanguageSerializesCorrectly) {
    Matcher original(fromjson("{$text: {$search: 'a', $caseSensitive: false}}"),
                     ExtensionsCallbackNoop());
    Matcher reserialized(serialize(original.getMatchExpression()), ExtensionsCallbackNoop());
    ASSERT_EQ(*reserialized.getQuery(),
              fromjson(
                  "{$text: {$search: 'a', $language: '', $caseSensitive: false, "
                  "$diacriticSensitive: false}}"));
    ASSERT_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));
}

}  // namespace
}  // namespace mongo
