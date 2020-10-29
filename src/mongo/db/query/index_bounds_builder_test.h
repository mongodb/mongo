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

#pragma once

#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class IndexBoundsBuilderTest : public unittest::Test {
public:
    /**
     * Utility function to create MatchExpression
     */
    std::unique_ptr<MatchExpression> parseMatchExpression(const BSONObj& obj) {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression status = MatchExpressionParser::parse(obj, std::move(expCtx));
        ASSERT_TRUE(status.isOK());
        return std::unique_ptr<MatchExpression>(status.getValue().release());
    }

    /**
     * Make a minimal IndexEntry from just an optional key pattern. A dummy name will be added. An
     * empty key pattern will be used if none is provided.
     */
    IndexEntry buildSimpleIndexEntry(const BSONObj& kp = BSONObj()) {
        return {kp,
                IndexNames::nameToType(IndexNames::findPluginName(kp)),
                IndexDescriptor::kLatestIndexVersion,
                false,
                {},
                {},
                false,
                false,
                CoreIndexInfo::Identifier("test_foo"),
                nullptr,
                {},
                nullptr,
                nullptr};
    }

    /**
     * Given a list of queries in 'toUnion', translate into index bounds and return
     * the union of these bounds in the out-parameter 'oilOut'.
     */
    void testTranslateAndUnion(const std::vector<BSONObj>& toUnion,
                               OrderedIntervalList* oilOut,
                               IndexBoundsBuilder::BoundsTightness* tightnessOut) {
        auto testIndex = buildSimpleIndexEntry();

        for (auto it = toUnion.begin(); it != toUnion.end(); ++it) {
            auto expr = parseMatchExpression(*it);
            BSONElement elt = it->firstElement();
            if (toUnion.begin() == it) {
                IndexBoundsBuilder::translate(expr.get(), elt, testIndex, oilOut, tightnessOut);
            } else {
                IndexBoundsBuilder::translateAndUnion(
                    expr.get(), elt, testIndex, oilOut, tightnessOut);
            }
        }
    }

    /**
     * Given a list of queries in 'toUnion', translate into index bounds and return
     * the intersection of these bounds in the out-parameter 'oilOut'.
     */
    void testTranslateAndIntersect(const std::vector<BSONObj>& toIntersect,
                                   OrderedIntervalList* oilOut,
                                   IndexBoundsBuilder::BoundsTightness* tightnessOut) {
        auto testIndex = buildSimpleIndexEntry();

        for (auto it = toIntersect.begin(); it != toIntersect.end(); ++it) {
            auto expr = parseMatchExpression(*it);
            BSONElement elt = it->firstElement();
            if (toIntersect.begin() == it) {
                IndexBoundsBuilder::translate(expr.get(), elt, testIndex, oilOut, tightnessOut);
            } else {
                IndexBoundsBuilder::translateAndIntersect(
                    expr.get(), elt, testIndex, oilOut, tightnessOut);
            }
        }
    }

    /**
     * 'constraints' is a vector of BSONObj's representing match expressions, where
     * each filter is paired with a boolean. If the boolean is true, then the filter's
     * index bounds should be intersected with the other constraints; if false, then
     * they should be unioned. The resulting bounds are returned in the
     * out-parameter 'oilOut'.
     */
    void testTranslate(const std::vector<std::pair<BSONObj, bool>>& constraints,
                       OrderedIntervalList* oilOut,
                       IndexBoundsBuilder::BoundsTightness* tightnessOut) {
        auto testIndex = buildSimpleIndexEntry();

        for (auto it = constraints.begin(); it != constraints.end(); ++it) {
            BSONObj obj = it->first;
            bool isIntersect = it->second;
            auto expr = parseMatchExpression(obj);
            BSONElement elt = obj.firstElement();
            if (constraints.begin() == it) {
                IndexBoundsBuilder::translate(expr.get(), elt, testIndex, oilOut, tightnessOut);
            } else if (isIntersect) {
                IndexBoundsBuilder::translateAndIntersect(
                    expr.get(), elt, testIndex, oilOut, tightnessOut);
            } else {
                IndexBoundsBuilder::translateAndUnion(
                    expr.get(), elt, testIndex, oilOut, tightnessOut);
            }
        }
    }

    /**
     * Get a BSONObj which represents the interval from MinKey to 'end'.
     */
    BSONObj minKeyIntObj(int end) {
        BSONObjBuilder bob;
        bob.appendMinKey("");
        bob.appendNumber("", end);
        return bob.obj();
    }

    /**
     * Get a BSONObj which represents the interval from 'start' to MaxKey.
     */
    BSONObj maxKeyIntObj(int start) {
        BSONObjBuilder bob;
        bob.appendNumber("", start);
        bob.appendMaxKey("");
        return bob.obj();
    }
};

}  // namespace mongo
