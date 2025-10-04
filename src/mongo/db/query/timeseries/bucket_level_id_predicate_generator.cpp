/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/timeseries/bucket_level_id_predicate_generator.h"

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/query/timeseries/bucket_level_comparison_predicate_generator.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/db/timeseries/timeseries_constants.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::timeseries {

namespace {

/**
 * Contextual information needed to rewrite AND expressions.
 */
struct Params {
    const ExpressionContext& pExpCtx;
    const BucketSpec& bucketSpec;
    const int bucketMaxSpanSeconds;
    const std::string controlMinTimePath;
    const std::string controlMaxTimePath;
    MatchExpression* matchExpr;
};

/**
 * This constant is used to detect extended range dates. "Extended range date" here refers to a
 * timestamp that cannot be in a bucket whose min time (as seconds after the epoch) is between 0 and
 * 2^31 - 1 inclusive. Thus an "extended range date" is dependent on the bucket rounding and max
 * span.
 *
 * Because this constant is used to generate 'alwaysTrue' and 'alwaysFalse' predicates when
 * comparing against extended range values, it is set conservatively here (based on 2^32), since
 * we only want to generate 'always' predicates when it is safe to do so.
 *
 * TODO SERVER-94723: Use a more accurate constant here that is based on 2^31 and takes into account
 * bucket rounding and max span.
 */
static const long long max32BitEpochMillis =
    static_cast<long long>(std::numeric_limits<uint32_t>::max()) * 1000;

/**
 * Creates an ObjectId initialized with an appropriate timestamp corresponding to 'rhs' and
 * returns it as a Value.
 */
template <typename MatchType>
auto constructObjectIdValue(const BSONElement& rhs, int bucketMaxSpanSeconds) {
    // Indicates whether to initialize an ObjectId with a max or min value for the non-date bytes.
    enum class OIDInit : bool { max, min };
    // Make an ObjectId corresponding to a date value. As a conversion from date to ObjectId will
    // truncate milliseconds, we round up when needed to prevent missing results.
    auto makeDateOID = [](auto&& date, auto&& maxOrMin, bool roundMillisUpToSecond = false) {
        if (roundMillisUpToSecond && (date.toMillisSinceEpoch() % 1000 != 0)) {
            date += Seconds{1};
        }

        auto oid = OID{};
        oid.init(date, maxOrMin == OIDInit::max);
        return oid;
    };
    // Make an ObjectId corresponding to a date value adjusted by the max bucket value. This
    // predicate can be used in a comparison to gauge a max value for a given bucket, rather than a
    // min value.
    auto makeMaxAdjustedDateOID = [&](auto&& date, auto&& maxOrMin) {
        // Ensure we don't underflow.
        if (date.toDurationSinceEpoch() >= Seconds{bucketMaxSpanSeconds})
            // Subtract max bucket range.
            return makeDateOID(date - Seconds{bucketMaxSpanSeconds}, maxOrMin);
        else
            // Since we're out of range, just make a predicate that is true for all dates.
            // We'll never use an OID for a date < 0 due to OID range limitations, so we set the
            // minimum date to 0.
            return makeDateOID(Date_t::fromMillisSinceEpoch(0LL), OIDInit::min);
    };

    // Because the OID timestamp is only 4 bytes, we can't convert larger dates
    invariant(rhs.date().toMillisSinceEpoch() >= 0LL);
    invariant(rhs.date().toMillisSinceEpoch() <= max32BitEpochMillis);

    // An ObjectId consists of a 4-byte timestamp, as well as a unique value and a counter, thus
    // two ObjectIds initialized with the same date will have different values. To ensure that we
    // do not incorrectly include or exclude any buckets, depending on the operator we will
    // construct either the largest or the smallest ObjectId possible with the corresponding date.
    // If the query operand is not of type Date, the original query will not match on any documents
    // because documents in a time-series collection must have a timeField of type Date. We will
    // make this case faster by keeping the ObjectId as the lowest or highest possible value so as
    // to eliminate all buckets.
    if constexpr (std::is_same_v<MatchType, LTMatchExpression>) {
        return Value{makeDateOID(rhs.date(), OIDInit::min, true /*roundMillisUpToSecond*/)};
    } else if constexpr (std::is_same_v<MatchType, LTEMatchExpression>) {
        return Value{makeDateOID(rhs.date(), OIDInit::max, true /*roundMillisUpToSecond*/)};
    } else if constexpr (std::is_same_v<MatchType, GTMatchExpression>) {
        return Value{makeMaxAdjustedDateOID(rhs.date(), OIDInit::max)};
    } else if constexpr (std::is_same_v<MatchType, GTEMatchExpression>) {
        return Value{makeMaxAdjustedDateOID(rhs.date(), OIDInit::min)};
    }
    MONGO_UNREACHABLE_TASSERT(5756800);
}

/**
 * Given a comparison expression where the operand is an extended date, return an
 * 'AlwaysTrueMatchExpression' or an 'AlwaysFalseMatchExpression'. If we get here, we are guaranteed
 * not to have any extended dates, so this transformation is safe.
 */
std::unique_ptr<MatchExpression> handleExtendedDateRanges(
    const ComparisonMatchExpressionBase* matchExpr, int64_t timestamp) {
    tassert(
        7823306,
        "Expected extended date range timestamp, but received a timestamp within the date range.",
        timestamp < 0LL || timestamp > max32BitEpochMillis);

    switch (matchExpr->matchType()) {
            // Since by this point we know that no time value has been inserted which is
            // outside the epoch range, we know that no document can meet this criteria
        case MatchExpression::EQ:
        case MatchExpression::INTERNAL_EXPR_EQ:
            return std::make_unique<AlwaysFalseMatchExpression>();
        case MatchExpression::GT:
        case MatchExpression::INTERNAL_EXPR_GT:
        case MatchExpression::GTE:
        case MatchExpression::INTERNAL_EXPR_GTE:
            if (timestamp < 0LL) {
                // Since by this point we know that no time value has been inserted < 0,
                // every document must meet this criteria
                return std::make_unique<AlwaysTrueMatchExpression>();
            }
            // If we are here we are guaranteed that 'timestamp > max32BitEpochMillis'. Since by
            // this point we know that no time value has been inserted > max32BitEpochMillis, we
            // know that no document can meet this criteria
            return std::make_unique<AlwaysFalseMatchExpression>();
        case MatchExpression::LT:
        case MatchExpression::INTERNAL_EXPR_LT:
        case MatchExpression::LTE:
        case MatchExpression::INTERNAL_EXPR_LTE:
            if (timestamp < 0LL) {
                // Since by this point we know that no time value has been inserted < 0,
                // we know that no document can meet this criteria
                return std::make_unique<AlwaysFalseMatchExpression>();
            }
            // If we are here we are guaranteed that 'timestamp > max32BitEpochMillis'. Since by
            // this point we know that no time value has been inserted > 0xffffffff every time value
            // must be less than this value
            return std::make_unique<AlwaysTrueMatchExpression>();
        default:
            MONGO_UNREACHABLE_TASSERT(7823305);
    }
}

/**
 * Given 'andExpr', add comparisons to _id that correspond with any comparisons to control.min.time
 * or control.max.time that we find, or replace with an "always" expression as appropriate.
 */
bool augmentPredicates(const Params& params, AndMatchExpression* andExpr) {
    bool changesMade = false;
    std::vector<std::unique_ptr<MatchExpression>>* childVector = andExpr->getChildVector();
    for (size_t i = 0; i < andExpr->numChildren(); ++i) {
        const MatchExpression* expr = andExpr->getChild(i);
        if (!ComparisonMatchExpressionBase::isInternalExprComparison(expr->matchType())) {
            continue;
        }

        const ComparisonMatchExpressionBase* cmpExpr =
            dynamic_cast<const ComparisonMatchExpressionBase*>(expr);

        StringData path = cmpExpr->path();
        if (path != params.controlMaxTimePath && path != params.controlMinTimePath) {
            continue;
        }

        const BSONElement& data = cmpExpr->getData();
        int64_t timestamp = data.Date().toMillisSinceEpoch();
        bool dateIsExtended = timestamp < 0ll || timestamp > max32BitEpochMillis;
        if (dateIsExtended) {
            // We only make these transformations if there are no extended range dates, so if the
            // operand is such a date, we can replace it with "always true" or "always false".
            std::unique_ptr<MatchExpression> new_expr =
                handleExtendedDateRanges(cmpExpr, timestamp);
            (*childVector)[i].swap(new_expr);
            changesMade = true;
            continue;
        }

        switch (cmpExpr->matchType()) {
            case MatchExpression::MatchType::INTERNAL_EXPR_GT:
                if (path == params.controlMaxTimePath) {
                    childVector->push_back(makeCmpMatchExpr<GTMatchExpression, Value>(
                        timeseries::kBucketIdFieldName,
                        constructObjectIdValue<GTMatchExpression>(data,
                                                                  params.bucketMaxSpanSeconds)));
                    changesMade = true;
                }
                break;
            case MatchExpression::MatchType::INTERNAL_EXPR_GTE:
                if (path == params.controlMaxTimePath) {
                    childVector->push_back(makeCmpMatchExpr<GTEMatchExpression, Value>(
                        timeseries::kBucketIdFieldName,
                        constructObjectIdValue<GTEMatchExpression>(data,
                                                                   params.bucketMaxSpanSeconds)));
                    changesMade = true;
                }
                break;
            case MatchExpression::MatchType::INTERNAL_EXPR_LT:
                if (path == params.controlMinTimePath) {
                    childVector->push_back(makeCmpMatchExpr<LTMatchExpression, Value>(
                        timeseries::kBucketIdFieldName,
                        constructObjectIdValue<LTMatchExpression>(data,
                                                                  params.bucketMaxSpanSeconds)));
                    changesMade = true;
                }
                break;
            case MatchExpression::MatchType::INTERNAL_EXPR_LTE:
                if (path == params.controlMinTimePath) {
                    childVector->push_back(makeCmpMatchExpr<LTEMatchExpression, Value>(
                        timeseries::kBucketIdFieldName,
                        constructObjectIdValue<LTEMatchExpression>(data,
                                                                   params.bucketMaxSpanSeconds)));
                    changesMade = true;
                }
                break;
            default:
                break;
        }
    }

    return changesMade;
}

/**
 * A visitor for AND nodes in a match expression, which transforms them as needed with
 * augmentPredicates().
 */
class TimePredicateVisitor : public SelectiveMatchExpressionVisitorBase<false /* IsConst */> {
public:
    using SelectiveMatchExpressionVisitorBase<false>::visit;

    TimePredicateVisitor(const Params& params) : _params{params} {}

    void visit(MaybeConstPtr<AndMatchExpression> expr) override {
        _changesMade = augmentPredicates(_params, expr) || _changesMade;
    }

    bool changesMade() const {
        return _changesMade;
    }

private:
    const Params& _params;
    bool _changesMade = false;
};

/**
 * A walker that will traverse a match expression and invoke the visitor with a non-const pointer to
 * the expressions found.
 *
 * TODO(SERVER-93636): This can be removed once we have a generic mutable walker.
 */
class TimePredicateMutableWalker {
public:
    TimePredicateMutableWalker(TimePredicateVisitor* visitor) : _visitor{visitor} {
        invariant(_visitor);
    }

    void preVisit(MatchExpression* expr) {
        expr->acceptVisitor(_visitor);
    }

    void postVisit(MatchExpression* expr) {}

    void inVisit(long count, MatchExpression* expr) {}

private:
    TimePredicateVisitor* _visitor;
};

bool findAndModifyTimePredicates(const Params& params) {
    TimePredicateVisitor visitor{params};
    TimePredicateMutableWalker walker{&visitor};
    constexpr bool isConst = false;
    tree_walker::walk<isConst, MatchExpression>(params.matchExpr, &walker);
    return visitor.changesMade();
}


}  // namespace

bool BucketLevelIdPredicateGenerator::generateIdPredicates(const ExpressionContext& pExpCtx,
                                                           const BucketSpec& bucketSpec,
                                                           int bucketMaxSpanSeconds,
                                                           MatchExpression* matchExpr) {

    const Params params{
        pExpCtx,
        bucketSpec,
        bucketMaxSpanSeconds,
        std::string{timeseries::kControlMinFieldNamePrefix + bucketSpec.timeField()},
        std::string{timeseries::kControlMaxFieldNamePrefix + bucketSpec.timeField()},
        matchExpr,
    };

    return findAndModifyTimePredicates(params);
}

}  // namespace mongo::timeseries
