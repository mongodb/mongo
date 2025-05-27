/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/timeseries/bucket_spec.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace timeseries {


/**
 * An abstract class responsible for building a comparison predicate from a time-series query.
 */
class BucketLevelComparisonPredicateGeneratorBase {
public:
    struct Params {
        const BucketSpec& bucketSpec;
        int bucketMaxSpanSeconds;
        boost::intrusive_ptr<ExpressionContext> pExpCtx;
        bool assumeNoMixedSchemaData;
        BucketSpec::IneligiblePredicatePolicy policy;
        bool fixedBuckets;
    };

    struct Output {
        std::unique_ptr<MatchExpression> matchExpr;
        bool rewriteProvidesExactMatchPredicate = false;
    };

    BucketLevelComparisonPredicateGeneratorBase(Params params) : _params(std::move(params)) {};
    virtual ~BucketLevelComparisonPredicateGeneratorBase() {}

    virtual Output generateTimeFieldPredicate(const ComparisonMatchExpressionBase* matchExpr,
                                              StringData minPathStringData,
                                              StringData maxPathStringData,
                                              Date_t timeField,
                                              BSONObj maxTime,
                                              StringData matchExprPath,
                                              const BSONElement& matchExprData) const = 0;

    Output createLoosePredicate(const ComparisonMatchExpressionBase* matchExpr) const;
    Output createTightPredicate(const ComparisonMatchExpressionBase* matchExpr) const;


protected:
    Params _params;
};

/**
 * The predicate generator class to be used for creating loose predicates for match expressions with
 * buckets that do not use extended range nor are fixed.
 */
class DefaultBucketLevelComparisonPredicateGenerator final
    : public BucketLevelComparisonPredicateGeneratorBase {
public:
    DefaultBucketLevelComparisonPredicateGenerator(Params params)
        : BucketLevelComparisonPredicateGeneratorBase(std::move(params)) {};

    Output generateTimeFieldPredicate(const ComparisonMatchExpressionBase* matchExpr,
                                      StringData minPathStringData,
                                      StringData maxPathStringData,
                                      Date_t timeField,
                                      BSONObj maxTime,
                                      StringData matchExprPath,
                                      const BSONElement& matchExprData) const override;
};

/**
 * The predicate generator class to be used to be used for creating loose predicates for match
 * expressions for fixed buckets.
 */
class FixedBucketsLevelComparisonPredicateGenerator final
    : public BucketLevelComparisonPredicateGeneratorBase {
public:
    FixedBucketsLevelComparisonPredicateGenerator(Params params)
        : BucketLevelComparisonPredicateGeneratorBase(std::move(params)) {};

    Output generateTimeFieldPredicate(const ComparisonMatchExpressionBase* matchExpr,
                                      StringData minPathStringData,
                                      StringData maxPathStringData,
                                      Date_t timeField,
                                      BSONObj maxTime,
                                      StringData matchExprPath,
                                      const BSONElement& matchExprData) const override;
};

/**
 * Responsible for deciding which 'ComparisonPredicateGeneratorBase' implementation to use for a
 * given bucket and match expression. Also holds the ownership of all the
 * 'BucketLevelComparisonPredicateGeneratorBase' implementations.
 */
class BucketLevelComparisonPredicateGenerator {
public:
    static std::unique_ptr<BucketLevelComparisonPredicateGeneratorBase> getBuilder(
        BucketLevelComparisonPredicateGeneratorBase::Params params);

    static BucketLevelComparisonPredicateGeneratorBase::Output createPredicate(
        const ComparisonMatchExpressionBase* matchExpr,
        BucketLevelComparisonPredicateGeneratorBase::Params params,
        bool tight) {
        auto builder = getBuilder(std::move(params));
        return tight ? builder->createTightPredicate(matchExpr)
                     : builder->createLoosePredicate(matchExpr);
    }
};

/**
 * Helper function to make comparison match expressions.
 */
template <typename T, typename V>
static auto makeCmpMatchExpr(StringData path, V val) {
    return std::make_unique<T>(path, val);
}
}  // namespace timeseries

}  // namespace mongo
