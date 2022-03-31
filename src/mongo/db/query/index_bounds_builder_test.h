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
#include "mongo/db/query/interval_evaluation_tree.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class IndexBoundsBuilderTest : public unittest::Test {
public:
    /**
     * Utility function to create MatchExpression
     */
    std::pair<std::unique_ptr<MatchExpression>, std::vector<const MatchExpression*>>
    parseMatchExpression(const BSONObj& obj, bool shouldNormalize = true) {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression status = MatchExpressionParser::parse(obj, std::move(expCtx));
        ASSERT_OK(status.getStatus()) << obj;
        auto expr = std::move(status.getValue());
        if (shouldNormalize) {
            expr = MatchExpression::normalize(std::move(expr));
        }
        auto inputParamIdMap = MatchExpression::parameterize(expr.get());
        return {std::move(expr), inputParamIdMap};
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
     * Make a multikey IndexEntry with the provided key pattern and multikey paths.
     */
    IndexEntry buildMultikeyIndexEntry(const BSONObj& kp, const MultikeyPaths& mkp) {
        return {kp,
                IndexNames::nameToType(IndexNames::findPluginName(kp)),
                IndexDescriptor::kLatestIndexVersion,
                true,  // multikey
                mkp,   // multikey paths
                {},
                false,
                false,
                IndexEntry::Identifier{"test_multikey"},
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
                               IndexBoundsBuilder::BoundsTightness* tightnessOut,
                               bool shouldNormalize = true) {
        auto testIndex = buildSimpleIndexEntry();
        auto obj = BSON("$or" << toUnion);
        auto [expr, inputParamIdMap] = parseMatchExpression(obj, shouldNormalize);
        BSONElement elt = toUnion[0].firstElement();
        interval_evaluation_tree::Builder ietBuilder{};

        ASSERT_EQ(MatchExpression::OR, expr->matchType()) << expr->debugString();
        for (size_t childIndex = 0; childIndex < expr->numChildren(); ++childIndex) {
            auto child = expr->getChild(childIndex);
            if (childIndex == 0) {
                IndexBoundsBuilder::translate(
                    child, elt, testIndex, oilOut, tightnessOut, &ietBuilder);
            } else {
                IndexBoundsBuilder::translateAndUnion(
                    child, elt, testIndex, oilOut, tightnessOut, &ietBuilder);
            }
        }

        assertIET(inputParamIdMap, ietBuilder, elt, testIndex, *oilOut);
    }

    /**
     * Given a list of queries in 'toUnion', translate into index bounds and return
     * the intersection of these bounds in the out-parameter 'oilOut'.
     */
    void testTranslateAndIntersect(const std::vector<BSONObj>& toIntersect,
                                   OrderedIntervalList* oilOut,
                                   IndexBoundsBuilder::BoundsTightness* tightnessOut) {
        auto testIndex = buildSimpleIndexEntry();
        auto obj = BSON("$and" << toIntersect);
        auto [expr, inputParamIdMap] = parseMatchExpression(obj);
        BSONElement elt = toIntersect[0].firstElement();
        interval_evaluation_tree::Builder ietBuilder{};

        ASSERT_EQ(MatchExpression::AND, expr->matchType());
        for (size_t childIndex = 0; childIndex < expr->numChildren(); ++childIndex) {
            auto child = expr->getChild(childIndex);
            if (childIndex == 0) {
                IndexBoundsBuilder::translate(
                    child, elt, testIndex, oilOut, tightnessOut, &ietBuilder);
            } else {
                IndexBoundsBuilder::translateAndIntersect(
                    child, elt, testIndex, oilOut, tightnessOut, &ietBuilder);
            }
        }

        assertIET(inputParamIdMap, ietBuilder, elt, testIndex, *oilOut);
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

    /**
     * Evaluates index index intervals from the given Interval Evaluation Trees (IET) and asserts
     * the the result is equal to the given OrderedIntervalList.
     */
    static void assertIET(const std::vector<const MatchExpression*>& inputParamIdMap,
                          const interval_evaluation_tree::Builder& ietBuilder,
                          const BSONElement& elt,
                          const IndexEntry& index,
                          const OrderedIntervalList& oil) {
        auto iet = ietBuilder.done();
        ASSERT(iet);

        auto restoredOil =
            interval_evaluation_tree::evaluateIntervals(*iet, inputParamIdMap, elt, index);
        ASSERT(oil == restoredOil);
    }
};

}  // namespace mongo
