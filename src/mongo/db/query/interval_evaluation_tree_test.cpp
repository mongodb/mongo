/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <cstddef>
#include <memory>

#include <boost/optional/optional.hpp>

#include "mongo/base/checked_cast.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/interval_evaluation_tree.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
class IntervalEvaluationTreeTest : public unittest::Test {
public:
    IntervalEvaluationTreeTest() : _index{buildSimpleIndexEntry(BSON("a" << 1))} {}

    /**
     * Asserts that a list of predicates produces expected list of Interval Evaluation Trees. It
     * takes a list of test cases, where a test case is a pair of a predicate and expected IET
     * serialised as a string.
     */
    void assertMany(const std::vector<std::pair<BSONObj, std::string>>& testCases) {
        for (const auto& [predicate, result] : testCases) {
            assertOne(predicate, result);
            // Test on equality of the tree.
            auto actualResultLhs = createIET(predicate);
            auto actualResultRhs = createIET(predicate);

            ASSERT_EQ(actualResultRhs, actualResultLhs);
        }
    }

    /**
     * Creates an IET based on the predicate.
     */
    interval_evaluation_tree::IET createIET(const BSONObj& predicate) {
        BSONObj obj = BSON("a" << predicate);
        auto expr = parseMatchExpression(obj);
        expr = MatchExpression::normalize(std::move(expr));
        auto inputParamIdMap = MatchExpression::parameterize(expr.get());
        BSONElement elt = obj.firstElement();
        return build(expr.get(), elt, inputParamIdMap);
    }

    /**
     * Asserts that a predicates produces expected Interval Evaluation Tree. expectedResult is an
     * IET serialised as a string.
     */
    void assertOne(const BSONObj& predicate, const std::string& expectedResult) const {
        BSONObj obj = BSON("a" << predicate);
        auto expr = parseMatchExpression(obj);
        expr = MatchExpression::normalize(std::move(expr));
        auto inputParamIdMap = MatchExpression::parameterize(expr.get());
        BSONElement elt = obj.firstElement();

        std::string actualResult = buildStr(expr.get(), elt, inputParamIdMap);

        ASSERT_EQ(expectedResult, actualResult);
    }

    /**
     * Takes a predicate represented in a match expression and builds an Interval Evaluation Tree.
     *
     * Returns the built IET.
     */
    interval_evaluation_tree::IET build(
        const MatchExpression* expr,
        BSONElement elt,
        const std::vector<const MatchExpression*>& inputParamIdMap) const {
        if (expr->matchType() == MatchExpression::AND) {
            return buildIntersect(
                checked_cast<const AndMatchExpression*>(expr), elt, inputParamIdMap);
        } else {
            return buildPredicate(expr, elt, inputParamIdMap);
        }
    }

    /**
     * Takes a predicate represented in a match expression and builds an Interval Evaluation Tree.
     *
     * Returns the built IET serialised as a string.
     */
    std::string buildStr(const MatchExpression* expr,
                         BSONElement elt,
                         const std::vector<const MatchExpression*>& inputParamIdMap) const {
        return ietToString(build(expr, elt, inputParamIdMap));
    }

    /**
     * Takes a list of intersected predicates represented in an AND expression and builds an
     * Interval Evaluation Tree.
     *
     * Returns the built IET.
     */
    interval_evaluation_tree::IET buildIntersect(
        const AndMatchExpression* expr,
        BSONElement elt,
        const std::vector<const MatchExpression*>& inputParamIdMap) const {
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        interval_evaluation_tree::Builder ietBuilder{};

        for (size_t i = 0; i < expr->numChildren(); ++i) {
            auto* child = expr->getChild(i);
            if (i == 0) {
                IndexBoundsBuilder::translate(child, elt, _index, &oil, &tightness, &ietBuilder);
            } else {
                IndexBoundsBuilder::translateAndIntersect(
                    child, elt, _index, &oil, &tightness, &ietBuilder);
            }
        }

        auto iet = ietBuilder.done().get();

        auto restoredOil =
            interval_evaluation_tree::evaluateIntervals(iet, inputParamIdMap, elt, _index);
        ASSERT(oil == restoredOil);

        return iet;
    }

    /**
     * Takes a simple predicate represented in a match expression and builds an Interval Evaluation
     * Tree.
     *
     * Returns the built IET serialised as a string.
     */
    interval_evaluation_tree::IET buildPredicate(
        const MatchExpression* expr,
        BSONElement elt,
        const std::vector<const MatchExpression*>& inputParamIdMap) const {
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        interval_evaluation_tree::Builder ietBuilder{};

        IndexBoundsBuilder::translate(expr, elt, _index, &oil, &tightness, &ietBuilder);

        auto iet = ietBuilder.done().get();

        auto restoredOil =
            interval_evaluation_tree::evaluateIntervals(iet, inputParamIdMap, elt, _index);
        ASSERT(oil == restoredOil);

        return iet;
    }

    static IndexEntry buildSimpleIndexEntry(const BSONObj& kp) {
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

    static std::unique_ptr<MatchExpression> parseMatchExpression(const BSONObj& obj) {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        StatusWithMatchExpression status = MatchExpressionParser::parse(obj, std::move(expCtx));
        ASSERT_TRUE(status.isOK());
        return std::move(status.getValue());
    }

    const IndexEntry _index;
};

TEST_F(IntervalEvaluationTreeTest, TranslateToEval) {
    std::vector<std::pair<BSONObj, std::string>> testCases = {
        {BSON("$lt" << 10), "(eval $lt #0)"},
        {BSON("$lte" << 10), "(eval $lte #0)"},
        {BSON("$gt" << 10), "(eval $gt #0)"},
        {BSON("$gte" << 10), "(eval $gte #0)"},
        {BSON("$eq" << 10), "(eval $eq #0)"},
        {BSON("$elemMatch" << BSON("$lt" << 10 << "$gt" << 1)),
         "(intersect (eval $lt #0) (eval $gt #1))"},
        {BSON("$in" << BSON_ARRAY(5 << 10 << 15)), "(eval $in #0)"},
        {BSON("$regex"
              << "aaa"),
         "(eval $regex #0)"},
        // TODO SERVER-64776: fix the $type test cases below
        {BSON("$type"
              << "int"),
         "(const [nan.0, inf.0])"},
        {BSON("$type" << BSON_ARRAY("string"
                                    << "double")),
         "(const [nan.0, inf.0] [\"\", {}))"},
    };

    assertMany(testCases);
}

TEST_F(IntervalEvaluationTreeTest, TranslateToConst) {
    std::vector<std::pair<BSONObj, std::string>> testCases = {
        {BSON("$mod" << BSON_ARRAY(2 << 3)), "(const [nan.0, inf.0])"},
        {BSON("$lt" << MAXKEY), "(const [MinKey, MaxKey))"},
        {BSON("$exists" << true), "(const [MinKey, MaxKey])"},
        {BSON("$exists" << false), "(const [null, null])"},
        {fromjson("{$in: [/alpha/i, /beta/]}"),
         "(const [\"\", {}) [/alpha/i, /alpha/i] [/beta/, /beta/])"},
        {BSON("$in" << BSON_ARRAY(5 << 10 << BSON_ARRAY(11))),
         "(const [5, 5] [10, 10] [11, 11] [[ 11 ], [ 11 ]])"},
        {fromjson("{$in: [/alpha/i, 101]}"), "(const [101, 101] [\"\", {}) [/alpha/i, /alpha/i])"},
        {BSON("$eq" << BSONNULL), "(const [undefined, undefined] [null, null])"},
        {fromjson("{$not: {$in: [null, []]}}"),
         "(const [MinKey, undefined) (null, []) ([], MaxKey])"},
        {BSON("$type"
              << "array"),
         "(const [MinKey, MaxKey])"},
        {BSON("$type" << BSON_ARRAY("int"
                                    << "array")),
         "(const [MinKey, MaxKey])"},
        {BSON("$gt" << BSONNULL), "(const)"},
        {BSON("$_internalExprEq" << 4), "(const [4, 4])"},
        {BSON("$_internalExprGt" << 4), "(const (4, MaxKey])"},
    };

    assertMany(testCases);
}

TEST_F(IntervalEvaluationTreeTest, TranslateToComplement) {
    std::vector<std::pair<BSONObj, std::string>> testCases = {
        {BSON("$not" << BSON("$lt" << 10)), "(not (eval $lt #0))"},
        {BSON("$not" << BSON("$in" << BSON_ARRAY(1 << 2))), "(not (eval $in #0))"},
        {BSON("$not" << BSON("$exists" << true)), "(const [null, null])"},
    };

    assertMany(testCases);
}

TEST_F(IntervalEvaluationTreeTest, Intersect) {
    std::vector<std::pair<BSONObj, std::string>> testCases = {
        {fromjson("{$lt: 100, $gt: 10, $eq: 11}"),
         "(intersect (intersect (eval $eq #0) (eval $lt #1)) (eval $gt #2))"},
    };

    assertMany(testCases);
}

TEST_F(IntervalEvaluationTreeTest, InEquality) {
    // Simple test.
    auto actualResultLhs = createIET(BSON("$not" << BSON("$lt" << 10)));
    auto actualResultRhs = createIET(BSON("$not" << BSON("$gt" << 11)));
    ASSERT_NOT_EQUALS(actualResultLhs, actualResultRhs);

    // Different operators.
    actualResultLhs = createIET(BSON("$in" << BSON_ARRAY(5 << 10 << BSON_ARRAY(11))));
    actualResultRhs = createIET(BSON("$mod" << BSON_ARRAY(2 << 3)));
    ASSERT_NOT_EQUALS(actualResultRhs, actualResultLhs);

    // Difference in the interval
    actualResultLhs = createIET(BSON("$_internalExprEq" << 4));
    actualResultRhs = createIET(BSON("$_internalExprGt" << 4));
    ASSERT_NOT_EQUALS(actualResultRhs, actualResultLhs);
}
}  // namespace mongo
