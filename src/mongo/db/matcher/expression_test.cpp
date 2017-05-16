// expression_test.cpp

/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/expression.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(LeafMatchExpressionTest, Equal1) {
    BSONObj temp = BSON("x" << 5);
    EqualityMatchExpression e;
    e.init("x", temp["x"]);

    ASSERT_TRUE(e.matchesBSON(fromjson("{ x : 5 }")));
    ASSERT_TRUE(e.matchesBSON(fromjson("{ x : [5] }")));
    ASSERT_TRUE(e.matchesBSON(fromjson("{ x : [1,5] }")));
    ASSERT_TRUE(e.matchesBSON(fromjson("{ x : [1,5,2] }")));
    ASSERT_TRUE(e.matchesBSON(fromjson("{ x : [5,2] }")));

    ASSERT_FALSE(e.matchesBSON(fromjson("{ x : null }")));
    ASSERT_FALSE(e.matchesBSON(fromjson("{ x : 6 }")));
    ASSERT_FALSE(e.matchesBSON(fromjson("{ x : [4,2] }")));
    ASSERT_FALSE(e.matchesBSON(fromjson("{ x : [[5]] }")));
}

TEST(LeafMatchExpressionTest, Comp1) {
    BSONObj temp = BSON("x" << 5);

    {
        LTEMatchExpression e;
        e.init("x", temp["x"]);
        ASSERT_TRUE(e.matchesBSON(fromjson("{ x : 5 }")));
        ASSERT_TRUE(e.matchesBSON(fromjson("{ x : 4 }")));
        ASSERT_FALSE(e.matchesBSON(fromjson("{ x : 6 }")));
        ASSERT_FALSE(e.matchesBSON(fromjson("{ x : 'eliot' }")));
    }

    {
        LTMatchExpression e;
        e.init("x", temp["x"]);
        ASSERT_FALSE(e.matchesBSON(fromjson("{ x : 5 }")));
        ASSERT_TRUE(e.matchesBSON(fromjson("{ x : 4 }")));
        ASSERT_FALSE(e.matchesBSON(fromjson("{ x : 6 }")));
        ASSERT_FALSE(e.matchesBSON(fromjson("{ x : 'eliot' }")));
    }

    {
        GTEMatchExpression e;
        e.init("x", temp["x"]);
        ASSERT_TRUE(e.matchesBSON(fromjson("{ x : 5 }")));
        ASSERT_FALSE(e.matchesBSON(fromjson("{ x : 4 }")));
        ASSERT_TRUE(e.matchesBSON(fromjson("{ x : 6 }")));
        ASSERT_FALSE(e.matchesBSON(fromjson("{ x : 'eliot' }")));
    }

    {
        GTMatchExpression e;
        e.init("x", temp["x"]);
        ASSERT_FALSE(e.matchesBSON(fromjson("{ x : 5 }")));
        ASSERT_FALSE(e.matchesBSON(fromjson("{ x : 4 }")));
        ASSERT_TRUE(e.matchesBSON(fromjson("{ x : 6 }")));
        ASSERT_FALSE(e.matchesBSON(fromjson("{ x : 'eliot' }")));
    }
}

TEST(MatchesBSONElement, ScalarEquality) {
    auto filterObj = fromjson("{i: 5}");
    EqualityMatchExpression filter;
    ASSERT_OK(filter.init("i", filterObj["i"]));

    auto aFive = fromjson("{a: 5}");
    auto iFive = fromjson("{i: 5}");
    ASSERT_TRUE(filter.matchesBSONElement(aFive["a"]));
    ASSERT_TRUE(filter.matchesBSON(iFive));

    auto aSix = fromjson("{a: 6}");
    auto iSix = fromjson("{i: 6}");
    ASSERT_FALSE(filter.matchesBSONElement(aSix["a"]));
    ASSERT_FALSE(filter.matchesBSON(iSix));

    auto aArrMatch1 = fromjson("{a: [5, 6]}");
    auto iArrMatch1 = fromjson("{i: [5, 6]}");
    ASSERT_TRUE(filter.matchesBSONElement(aArrMatch1["a"]));
    ASSERT_TRUE(filter.matchesBSON(iArrMatch1));

    auto aArrMatch2 = fromjson("{a: [6, 5]}");
    auto iArrMatch2 = fromjson("{i: [6, 5]}");
    ASSERT_TRUE(filter.matchesBSONElement(aArrMatch2["a"]));
    ASSERT_TRUE(filter.matchesBSON(iArrMatch2));

    auto aArrNoMatch = fromjson("{a: [6, 6]}");
    auto iArrNoMatch = fromjson("{i: [6, 6]}");
    ASSERT_FALSE(filter.matchesBSONElement(aArrNoMatch["a"]));
    ASSERT_FALSE(filter.matchesBSON(iArrNoMatch));

    auto aObj = fromjson("{a: {i: 5}}");
    auto iObj = fromjson("{i: {i: 5}}");
    ASSERT_FALSE(filter.matchesBSONElement(aObj["a"]));
    ASSERT_FALSE(filter.matchesBSON(iObj));

    auto aObjArr = fromjson("{a: [{i: 5}]}");
    auto iObjArr = fromjson("{i: [{i: 5}]}");
    ASSERT_FALSE(filter.matchesBSONElement(aObjArr["a"]));
    ASSERT_FALSE(filter.matchesBSON(iObjArr));
}

TEST(MatchesBSONElement, DottedPathEquality) {
    auto filterObj = fromjson("{'i.a': 5}");
    EqualityMatchExpression filter;
    ASSERT_OK(filter.init("i.a", filterObj["i.a"]));

    auto aFive = fromjson("{a: 5}");
    auto iFive = fromjson("{i: 5}");
    ASSERT_FALSE(filter.matchesBSONElement(aFive["a"]));
    ASSERT_FALSE(filter.matchesBSON(iFive));

    auto aArr = fromjson("{a: [5]}");
    auto iArr = fromjson("{i: [5]}");
    ASSERT_FALSE(filter.matchesBSONElement(aArr["a"]));
    ASSERT_FALSE(filter.matchesBSON(iArr));

    auto aObjMatch = fromjson("{a: {a: 5, b: 6}}");
    auto iObjMatch = fromjson("{i: {a: 5, b: 6}}");
    ASSERT_TRUE(filter.matchesBSONElement(aObjMatch["a"]));
    ASSERT_TRUE(filter.matchesBSON(iObjMatch));

    auto aObjNoMatch1 = fromjson("{a: {a: 6}}");
    auto iObjNoMatch1 = fromjson("{i: {a: 6}}");
    ASSERT_FALSE(filter.matchesBSONElement(aObjNoMatch1["a"]));
    ASSERT_FALSE(filter.matchesBSON(iObjNoMatch1));

    auto aObjNoMatch2 = fromjson("{a: {b: 5}}");
    auto iObjNoMatch2 = fromjson("{i: {b: 5}}");
    ASSERT_FALSE(filter.matchesBSONElement(aObjNoMatch2["a"]));
    ASSERT_FALSE(filter.matchesBSON(iObjNoMatch2));

    auto aObjArrMatch1 = fromjson("{a: [{a: 5}, {a: 6}]}");
    auto iObjArrMatch1 = fromjson("{i: [{a: 5}, {a: 6}]}");
    ASSERT_TRUE(filter.matchesBSONElement(aObjArrMatch1["a"]));
    ASSERT_TRUE(filter.matchesBSON(iObjArrMatch1));

    auto aObjArrMatch2 = fromjson("{a: [{a: 6}, {a: 5}]}");
    auto iObjArrMatch2 = fromjson("{i: [{a: 6}, {a: 5}]}");
    ASSERT_TRUE(filter.matchesBSONElement(aObjArrMatch2["a"]));
    ASSERT_TRUE(filter.matchesBSON(iObjArrMatch2));

    auto aObjArrNoMatch1 = fromjson("{a: [{a: 6}, {a: 6}]}");
    auto iObjArrNoMatch1 = fromjson("{i: [{a: 6}, {a: 6}]}");
    ASSERT_FALSE(filter.matchesBSONElement(aObjArrNoMatch1["a"]));
    ASSERT_FALSE(filter.matchesBSON(iObjArrNoMatch1));

    auto aObjArrNoMatch2 = fromjson("{a: [{b: 5}, {b: 5}]}");
    auto iObjArrNoMatch2 = fromjson("{i: [{b: 5}, {b: 5}]}");
    ASSERT_FALSE(filter.matchesBSONElement(aObjArrNoMatch2["a"]));
    ASSERT_FALSE(filter.matchesBSON(iObjArrNoMatch2));
}

TEST(MatchesBSONElement, ArrayIndexEquality) {
    auto filterObj = fromjson("{'i.1': 5}");
    EqualityMatchExpression filter;
    ASSERT_OK(filter.init("i.1", filterObj["i.1"]));

    auto aFive = fromjson("{a: 5}");
    auto iFive = fromjson("{i: 5}");
    ASSERT_FALSE(filter.matchesBSONElement(aFive["a"]));
    ASSERT_FALSE(filter.matchesBSON(iFive));

    auto aArrMatch = fromjson("{a: [6, 5]}");
    auto iArrMatch = fromjson("{i: [6, 5]}");
    ASSERT_TRUE(filter.matchesBSONElement(aArrMatch["a"]));
    ASSERT_TRUE(filter.matchesBSON(iArrMatch));

    auto aArrNoMatch = fromjson("{a: [5, 6]}");
    auto iArrNoMatch = fromjson("{i: [5, 6]}");
    ASSERT_FALSE(filter.matchesBSONElement(aArrNoMatch["a"]));
    ASSERT_FALSE(filter.matchesBSON(iArrNoMatch));

    auto aObjMatch = fromjson("{a: {'1': 5}}");
    auto iObjMatch = fromjson("{i: {'1': 5}}");
    ASSERT_TRUE(filter.matchesBSONElement(aObjMatch["a"]));
    ASSERT_TRUE(filter.matchesBSON(iObjMatch));

    auto aObjNoMatch = fromjson("{a: {i: 5}}");
    auto iObjNoMatch = fromjson("{i: {i: 5}}");
    ASSERT_FALSE(filter.matchesBSONElement(aObjNoMatch["a"]));
    ASSERT_FALSE(filter.matchesBSON(iObjNoMatch));

    auto aObjArrMatch = fromjson("{a: [{'1': 5}]}");
    auto iObjArrMatch = fromjson("{i: [{'1': 5}]}");
    ASSERT_TRUE(filter.matchesBSONElement(aObjArrMatch["a"]));
    ASSERT_TRUE(filter.matchesBSON(iObjArrMatch));

    auto aObjArrNoMatch = fromjson("{a: [{i: 6}, {i: 5}]}");
    auto iObjArrNoMatch = fromjson("{i: [{i: 6}, {i: 5}]}");
    ASSERT_FALSE(filter.matchesBSONElement(aObjArrNoMatch["a"]));
    ASSERT_FALSE(filter.matchesBSON(iObjArrNoMatch));

    auto aArrArr = fromjson("{a: [[6, 5], [6, 5]]}");
    auto iArrArr = fromjson("{i: [[6, 5], [6, 5]]}");
    ASSERT_FALSE(filter.matchesBSONElement(aArrArr["a"]));
    ASSERT_FALSE(filter.matchesBSON(iArrArr));
}

TEST(MatchesBSONElement, ObjectEquality) {
    auto filterObj = fromjson("{i: {a: 5}}");
    EqualityMatchExpression filter;
    ASSERT_OK(filter.init("i", filterObj["i"]));

    auto aFive = fromjson("{a: 5}");
    auto iFive = fromjson("{i: 5}");
    ASSERT_FALSE(filter.matchesBSONElement(aFive["a"]));
    ASSERT_FALSE(filter.matchesBSON(iFive));

    auto aArr = fromjson("{a: [5]}");
    auto iArr = fromjson("{i: [5]}");
    ASSERT_FALSE(filter.matchesBSONElement(aArr["a"]));
    ASSERT_FALSE(filter.matchesBSON(iArr));

    auto aObjMatch = fromjson("{a: {a: 5}}");
    auto iObjMatch = fromjson("{i: {a: 5}}");
    ASSERT_TRUE(filter.matchesBSONElement(aObjMatch["a"]));
    ASSERT_TRUE(filter.matchesBSON(iObjMatch));

    auto aObjNoMatch1 = fromjson("{a: {a: 5, b: 6}}");
    auto iObjNoMatch1 = fromjson("{i: {a: 5, b: 6}}");
    ASSERT_FALSE(filter.matchesBSONElement(aObjNoMatch1["a"]));
    ASSERT_FALSE(filter.matchesBSON(iObjNoMatch1));

    auto aObjNoMatch2 = fromjson("{a: {a: 6}}");
    auto iObjNoMatch2 = fromjson("{i: {a: 6}}");
    ASSERT_FALSE(filter.matchesBSONElement(aObjNoMatch2["a"]));
    ASSERT_FALSE(filter.matchesBSON(iObjNoMatch2));

    auto aObjNoMatch3 = fromjson("{a: {b: 5}}");
    auto iObjNoMatch3 = fromjson("{i: {b: 5}}");
    ASSERT_FALSE(filter.matchesBSONElement(aObjNoMatch3["a"]));
    ASSERT_FALSE(filter.matchesBSON(iObjNoMatch3));

    auto aObjArrMatch1 = fromjson("{a: [{a: 5}, {a: 6}]}");
    auto iObjArrMatch1 = fromjson("{i: [{a: 5}, {a: 6}]}");
    ASSERT_TRUE(filter.matchesBSONElement(aObjArrMatch1["a"]));
    ASSERT_TRUE(filter.matchesBSON(iObjArrMatch1));

    auto aObjArrMatch2 = fromjson("{a: [{a: 6}, {a: 5}]}");
    auto iObjArrMatch2 = fromjson("{i: [{a: 6}, {a: 5}]}");
    ASSERT_TRUE(filter.matchesBSONElement(aObjArrMatch2["a"]));
    ASSERT_TRUE(filter.matchesBSON(iObjArrMatch2));

    auto aObjArrNoMatch = fromjson("{a: [{a: 6}, {a: 6}]}");
    auto iObjArrNoMatch = fromjson("{i: [{a: 6}, {a: 6}]}");
    ASSERT_FALSE(filter.matchesBSONElement(aObjArrNoMatch["a"]));
    ASSERT_FALSE(filter.matchesBSON(iObjArrNoMatch));
}

TEST(MatchesBSONElement, ArrayEquality) {
    auto filterObj = fromjson("{i: [5]}");
    EqualityMatchExpression filter;
    ASSERT_OK(filter.init("i", filterObj["i"]));

    auto aFive = fromjson("{a: 5}");
    auto iFive = fromjson("{i: 5}");
    ASSERT_FALSE(filter.matchesBSONElement(aFive["a"]));
    ASSERT_FALSE(filter.matchesBSON(iFive));

    auto aArrMatch = fromjson("{a: [5]}");
    auto iArrMatch = fromjson("{i: [5]}");
    ASSERT_TRUE(filter.matchesBSONElement(aArrMatch["a"]));
    ASSERT_TRUE(filter.matchesBSON(iArrMatch));

    auto aArrNoMatch = fromjson("{a: [5, 6]}");
    auto iArrNoMatch = fromjson("{i: [5, 6]}");
    ASSERT_FALSE(filter.matchesBSONElement(aArrNoMatch["a"]));
    ASSERT_FALSE(filter.matchesBSON(iArrNoMatch));

    auto aObj = fromjson("{a: {i: [5]}}");
    auto iObj = fromjson("{i: {i: [5]}}");
    ASSERT_FALSE(filter.matchesBSONElement(aObj["a"]));
    ASSERT_FALSE(filter.matchesBSON(iObj));

    auto aObjArr = fromjson("{a: [{i: [5]}]}");
    auto iObjArr = fromjson("{i: [{i: [5]}]}");
    ASSERT_FALSE(filter.matchesBSONElement(aObjArr["a"]));
    ASSERT_FALSE(filter.matchesBSON(iObjArr));
}

TEST(MatchesBSONElement, LogicalExpression) {
    auto clauseObj1 = fromjson("{i: 5}");
    auto clauseObj2 = fromjson("{'i.a': 6}");
    std::unique_ptr<ComparisonMatchExpression> clause1(new EqualityMatchExpression());
    ASSERT_OK(clause1->init("i", clauseObj1["i"]));
    std::unique_ptr<ComparisonMatchExpression> clause2(new EqualityMatchExpression());
    ASSERT_OK(clause2->init("i.a", clauseObj2["i.a"]));

    OrMatchExpression filter;
    filter.add(clause1.release());
    filter.add(clause2.release());

    auto aClause1 = fromjson("{a: 5}");
    auto iClause1 = fromjson("{i: 5}");
    ASSERT_TRUE(filter.matchesBSONElement(aClause1["a"]));
    ASSERT_TRUE(filter.matchesBSON(iClause1));

    auto aClause2 = fromjson("{a: {a: 6}}");
    auto iClause2 = fromjson("{i: {a: 6}}");
    ASSERT_TRUE(filter.matchesBSONElement(aClause2["a"]));
    ASSERT_TRUE(filter.matchesBSON(iClause2));

    auto aNoMatch1 = fromjson("{a: 6}");
    auto iNoMatch1 = fromjson("{i: 6}");
    ASSERT_FALSE(filter.matchesBSONElement(aNoMatch1["a"]));
    ASSERT_FALSE(filter.matchesBSON(iNoMatch1));

    auto aNoMatch2 = fromjson("{a: {a: 5}}");
    auto iNoMatch2 = fromjson("{i: {a: 5}}");
    ASSERT_FALSE(filter.matchesBSONElement(aNoMatch2["a"]));
    ASSERT_FALSE(filter.matchesBSON(iNoMatch2));
}
}  // namespace mongo
