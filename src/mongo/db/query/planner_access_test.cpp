/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

BSONObj serializeMatcher(Matcher* matcher) {
    BSONObjBuilder builder;
    matcher->getMatchExpression()->serialize(&builder);
    return builder.obj();
}

TEST(PlannerAccessTest, PrepareForAccessPlanningSortsEqualNodesByTheirChildren) {
    boost::intrusive_ptr<ExpressionContext> expCtx{new ExpressionContextForTest()};
    Matcher matcher{fromjson("{$or: [{x: 1, b: 1}, {y: 1, a: 1}]}"), expCtx};
    // Before sorting for access planning, the order of the tree should be as specified in the
    // original input BSON.
    ASSERT_BSONOBJ_EQ(serializeMatcher(&matcher),
                      fromjson("{$or: [{$and: [{x: {$eq: 1}}, {b: {$eq: 1}}]},"
                               "{$and: [{y: {$eq: 1}}, {a: {$eq: 1}}]}]}"));

    // The two $or nodes in the match expression tree are only differentiated by their children.
    // After sorting in the order expected by the access planner, the $or node with the "a" child
    // should come first.
    prepareForAccessPlanning(matcher.getMatchExpression());
    ASSERT_BSONOBJ_EQ(serializeMatcher(&matcher),
                      fromjson("{$or: [{$and: [{a: {$eq: 1}}, {y: {$eq: 1}}]},"
                               "{$and: [{b: {$eq: 1}}, {x: {$eq: 1}}]}]}"));
}

TEST(PlannerAccessTest, PrepareForAccessPlanningSortsByNumberOfChildren) {
    boost::intrusive_ptr<ExpressionContext> expCtx{new ExpressionContextForTest()};
    Matcher matcher{fromjson("{$or: [{a: 1, c: 1, b: 1}, {b: 1, a: 1}]}"), expCtx};
    // Before sorting for access planning, the order of the tree should be as specified in the
    // original input BSON.
    ASSERT_BSONOBJ_EQ(serializeMatcher(&matcher),
                      fromjson("{$or: [{$and: [{a: {$eq: 1}}, {c: {$eq: 1}}, {b: {$eq: 1}}]},"
                               "{$and: [{b: {$eq: 1}}, {a: {$eq: 1}}]}]}"));

    // The two $or nodes in the match expression tree are only differentiated by the number of
    // children they have. Both have {a: {$eq: 1}} and {b: {$eq: 1}} as their first children, but
    // one has an additional child. The node with fewer children should be sorted first.
    prepareForAccessPlanning(matcher.getMatchExpression());
    ASSERT_BSONOBJ_EQ(serializeMatcher(&matcher),
                      fromjson("{$or: [{$and: [{a: {$eq: 1}}, {b: {$eq: 1}}]},"
                               "{$and: [{a: {$eq: 1}}, {b: {$eq: 1}}, {c: {$eq: 1}}]}]}"));
}

TEST(PlannerAccessTest, PrepareForAccessPlanningSortIsStable) {
    boost::intrusive_ptr<ExpressionContext> expCtx{new ExpressionContextForTest()};
    Matcher matcher{fromjson("{$or: [{a: 2, b: 2}, {a: 1, b: 1}]}"), expCtx};
    BSONObj expectedSerialization = fromjson(
        "{$or: [{$and: [{a: {$eq: 2}}, {b: {$eq: 2}}]},"
        "{$and: [{a: {$eq: 1}}, {b: {$eq: 1}}]}]}");
    // Before sorting for access planning, the order of the tree should be as specified in the
    // original input BSON.
    ASSERT_BSONOBJ_EQ(serializeMatcher(&matcher), expectedSerialization);

    // Sorting for access planning should not change the order of the match expression tree. The two
    // $or branches are considered equal, since they only differ by their constants.
    prepareForAccessPlanning(matcher.getMatchExpression());
    ASSERT_BSONOBJ_EQ(serializeMatcher(&matcher), expectedSerialization);
}

}  // namespace
}  // namespace mongo
