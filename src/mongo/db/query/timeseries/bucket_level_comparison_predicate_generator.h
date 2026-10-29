// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/timeseries/bucket_spec.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::timeseries {

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
                                              std::string_view minPathStringData,
                                              std::string_view maxPathStringData,
                                              Date_t timeField,
                                              BSONObj maxTime,
                                              std::string_view matchExprPath,
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
class [[MONGO_MOD_FILE_PRIVATE]] DefaultBucketLevelComparisonPredicateGenerator final
    : public BucketLevelComparisonPredicateGeneratorBase {
public:
    DefaultBucketLevelComparisonPredicateGenerator(Params params)
        : BucketLevelComparisonPredicateGeneratorBase(std::move(params)) {};

    Output generateTimeFieldPredicate(const ComparisonMatchExpressionBase* matchExpr,
                                      std::string_view minPathStringData,
                                      std::string_view maxPathStringData,
                                      Date_t timeField,
                                      BSONObj maxTime,
                                      std::string_view matchExprPath,
                                      const BSONElement& matchExprData) const override;
};

/**
 * The predicate generator class to be used to be used for creating loose predicates for match
 * expressions for fixed buckets.
 */
class [[MONGO_MOD_FILE_PRIVATE]] FixedBucketsLevelComparisonPredicateGenerator final
    : public BucketLevelComparisonPredicateGeneratorBase {
public:
    FixedBucketsLevelComparisonPredicateGenerator(Params params)
        : BucketLevelComparisonPredicateGeneratorBase(std::move(params)) {};

    Output generateTimeFieldPredicate(const ComparisonMatchExpressionBase* matchExpr,
                                      std::string_view minPathStringData,
                                      std::string_view maxPathStringData,
                                      Date_t timeField,
                                      BSONObj maxTime,
                                      std::string_view matchExprPath,
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
static auto makeCmpMatchExpr(std::string_view path, V val) {
    return std::make_unique<T>(path, val);
}
}  // namespace mongo::timeseries
