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

#include "mongo/db/query/timeseries/bucket_level_comparison_predicate_generator.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/oid.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"


namespace mongo {
namespace timeseries {

namespace {
static const long long max32BitEpochMillis =
    static_cast<long long>(std::numeric_limits<uint32_t>::max()) * 1000;

/**
 * Helper function to make comparison match expressions.
 */
template <typename T, typename V>
auto makeCmpMatchExpr(StringData path, V val) {
    return std::make_unique<T>(path, val);
}

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
    // Make an ObjectId corresponding to a date value adjusted by the max bucket value for the
    // time series view that this query operates on. This predicate can be used in a comparison
    // to gauge a max value for a given bucket, rather than a min value.
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

// Checks for the situations when it's not possible to create a bucket-level predicate (against the
// computed control values) for the given event-level predicate ('matchExpr').
boost::optional<StringData> checkComparisonPredicateEligibility(
    const ComparisonMatchExpressionBase* matchExpr,
    const StringData matchExprPath,
    const BSONElement& matchExprData,
    const BucketSpec& bucketSpec,
    ExpressionContext::CollationMatchesDefault collationMatchesDefault) {
    // The control field's min and max are chosen using a field-order insensitive comparator, while
    // MatchExpressions use a comparator that treats field-order as significant. Because of this we
    // will not perform this optimization on queries with operands of compound types.
    if (matchExprData.type() == BSONType::Object || matchExprData.type() == BSONType::Array)
        return "operand can't be an object or array"_sd;

    const auto isTimeField = (matchExprPath == bucketSpec.timeField());

    // Even when assumeNoMixedSchemaData is true, a bucket might contain events with the missing
    // fields. These events aren't taken in account when computing the control values for those
    // fields. This design has two repercussions:
    // 1. MatchExpressions have special comparison semantics regarding null, in that {$eq: null}
    //    will match all documents where the field is either null or missing. This semantics cannot
    //    be represented in terms of comparisons against the min/max control values.
    // 2. Non-type-bracketing predicates, such as {$expr: {$lt(e): ['$x', 42]}} should evaluate to
    //    "true" if "x" is missing, which also cannot be represented as a bucket-level predicate.
    //    1) time field cannot be empty.
    //    2) the only type less than null is MinKey, which is internal, so we don't need to guard
    //       GT and GTE.
    //    3) for the buckets that might have mixed schema data, we'll compare the types of min and
    //       max when _creating_ the bucket-level predicate (that check won't help with missing).
    if (matchExprData.type() == BSONType::jstNULL)
        return "can't handle comparison to null"_sd;
    if (!isTimeField &&
        (matchExpr->matchType() == MatchExpression::INTERNAL_EXPR_LTE ||
         matchExpr->matchType() == MatchExpression::INTERNAL_EXPR_LT)) {
        return "can't handle a non-type-bracketing LT or LTE comparisons"_sd;
    }

    // The control field's min and max are chosen based on the collation of the collection. If the
    // query's collation does not match the collection's collation and the query operand is a
    // string or compound type (skipped above) we will not perform this optimization.
    if (collationMatchesDefault == ExpressionContext::CollationMatchesDefault::kNo &&
        matchExprData.type() == BSONType::String) {
        return "can't handle string comparison with a non-default collation"_sd;
    }

    // This function only handles time and measurement predicates--not metadata.
    if (bucketSpec.metaField() &&
        (matchExprPath == bucketSpec.metaField().value() ||
         expression::isPathPrefixOf(bucketSpec.metaField().value(), matchExprPath))) {
        tasserted(6707200,
                  str::stream() << "createPredicate() does not handle metadata predicates: "
                                << matchExpr);
    }

    // We must avoid mapping predicates on fields computed via $addFields or a computed $project.
    if (bucketSpec.fieldIsComputed(matchExprPath.toString())) {
        return "can't handle a computed field"_sd;
    }

    // We must avoid mapping predicates on fields removed by $project.
    if (!determineIncludeField(matchExprPath, bucketSpec.behavior(), bucketSpec.fieldSet())) {
        return "can't handle a field removed by projection"_sd;
    }

    if (isTimeField && matchExprData.type() != BSONType::Date) {
        // Users are not allowed to insert non-date measurements into time field. So this query
        // would not match anything. We do not need to optimize for this case.
        // TODO SERVER-84207: right now we will end up unpacking everything and applying the event
        // filter, which indeed would be either trivially true or trivially false but it won't be
        // optimized away.
        return "can't handle comparison of time field to a non-Date type"_sd;
    }

    return boost::none;
}

/**
 * Makes a disjunction of the given predicates.
 *
 * - The result is non-null; it may be an OrMatchExpression with zero children.
 * - Any trivially-false arguments are omitted.
 * - If only one argument is nontrivial, returns that argument rather than adding an extra
 *   OrMatchExpression around it.
 */
std::unique_ptr<MatchExpression> makeOr(std::vector<std::unique_ptr<MatchExpression>> predicates) {
    std::vector<std::unique_ptr<MatchExpression>> nontrivial;
    for (auto&& p : predicates) {
        if (!p->isTriviallyFalse())
            nontrivial.push_back(std::move(p));
    }

    if (nontrivial.size() == 1)
        return std::move(nontrivial[0]);

    return std::make_unique<OrMatchExpression>(std::move(nontrivial));
}

/*
 * Creates a predicate that ensures that if there exists a subpath of matchExprPath such that the
 * type of `control.min.subpath` is not the same as `control.max.subpath` then we will match that
 * document.
 *
 * However, if the buckets collection has no mixed-schema data then this type-equality predicate is
 * unnecessary. In that case this function returns an empty, always-true predicate.
 */
std::unique_ptr<MatchExpression> createTypeEqualityPredicate(
    boost::intrusive_ptr<ExpressionContext> pExpCtx,
    StringData matchExprPath,
    bool assumeNoMixedSchemaData) {

    std::vector<std::unique_ptr<MatchExpression>> typeEqualityPredicates;

    if (assumeNoMixedSchemaData)
        return makeOr(std::move(typeEqualityPredicates));

    FieldPath matchExprField(matchExprPath);
    using namespace timeseries;

    // Assume that we're generating a predicate on "a.b"
    for (size_t i = 0; i < matchExprField.getPathLength(); i++) {
        auto minPath =
            std::string{timeseries::kControlMinFieldNamePrefix} + matchExprField.getSubpath(i);
        auto maxPath =
            std::string{timeseries::kControlMaxFieldNamePrefix} + matchExprField.getSubpath(i);

        // This whole block adds
        // {$expr: {$ne: [{$type: "$control.min.a"}, {$type: "$control.max.a"}]}}
        // in order to ensure that the type of `control.min.a` and `control.max.a` are the same.

        // This produces {$expr: ... }
        typeEqualityPredicates.push_back(std::make_unique<ExprMatchExpression>(
            // This produces {$ne: ... }
            make_intrusive<ExpressionCompare>(
                pExpCtx.get(),
                ExpressionCompare::CmpOp::NE,
                // This produces [...]
                makeVector<boost::intrusive_ptr<Expression>>(
                    // This produces {$type: ... }
                    make_intrusive<ExpressionType>(
                        pExpCtx.get(),
                        // This produces [...]
                        makeVector<boost::intrusive_ptr<Expression>>(
                            // This produces "$control.min.a"
                            ExpressionFieldPath::createPathFromString(
                                pExpCtx.get(), minPath, pExpCtx->variablesParseState))),
                    // This produces {$type: ... }
                    make_intrusive<ExpressionType>(
                        pExpCtx.get(),
                        // This produces [...]
                        makeVector<boost::intrusive_ptr<Expression>>(
                            // This produces "$control.max.a"
                            ExpressionFieldPath::createPathFromString(
                                pExpCtx.get(), maxPath, pExpCtx->variablesParseState))))),
            pExpCtx));
    }
    return makeOr(std::move(typeEqualityPredicates));
}

BucketLevelComparisonPredicateGeneratorBase::Output generateNonTimeFieldPredicate(
    const ComparisonMatchExpressionBase* matchExpr,
    const BucketLevelComparisonPredicateGeneratorBase::Params params,
    StringData minPathStringData,
    StringData maxPathStringData,
    StringData matchExprPath,
    const BSONElement& matchExprData) {
    switch (matchExpr->matchType()) {
        case MatchExpression::EQ:
        case MatchExpression::INTERNAL_EXPR_EQ:
            return {makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                makeAnd(makeCmpMatchExpr<InternalExprLTEMatchExpression>(minPathStringData,
                                                                         matchExprData),
                        makeCmpMatchExpr<InternalExprGTEMatchExpression>(maxPathStringData,
                                                                         matchExprData)),
                createTypeEqualityPredicate(
                    params.pExpCtx, matchExprPath, params.assumeNoMixedSchemaData)))};
        case MatchExpression::GT:
        case MatchExpression::INTERNAL_EXPR_GT:
            return {makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                makeCmpMatchExpr<InternalExprGTMatchExpression>(maxPathStringData, matchExprData),
                createTypeEqualityPredicate(
                    params.pExpCtx, matchExprPath, params.assumeNoMixedSchemaData)))};
        case MatchExpression::GTE:
        case MatchExpression::INTERNAL_EXPR_GTE:
            return {makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                makeCmpMatchExpr<InternalExprGTEMatchExpression>(maxPathStringData, matchExprData),
                createTypeEqualityPredicate(
                    params.pExpCtx, matchExprPath, params.assumeNoMixedSchemaData)))};
        case MatchExpression::LT:
        case MatchExpression::INTERNAL_EXPR_LT:
            return {makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                makeCmpMatchExpr<InternalExprLTMatchExpression>(minPathStringData, matchExprData),
                createTypeEqualityPredicate(
                    params.pExpCtx, matchExprPath, params.assumeNoMixedSchemaData)))};
        case MatchExpression::LTE:
        case MatchExpression::INTERNAL_EXPR_LTE:
            return {makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                makeCmpMatchExpr<InternalExprLTEMatchExpression>(minPathStringData, matchExprData),
                createTypeEqualityPredicate(
                    params.pExpCtx, matchExprPath, params.assumeNoMixedSchemaData)))};
        default:
            MONGO_UNREACHABLE_TASSERT(5348302);
    }
}

BucketLevelComparisonPredicateGeneratorBase::Output handleExtendedDateRanges(
    const ComparisonMatchExpressionBase* matchExpr, long long timestamp) {
    tassert(
        7823306,
        "Expected extended date range timestamp, but received a timestamp within the date range.",
        timestamp < 0LL || timestamp > max32BitEpochMillis);

    switch (matchExpr->matchType()) {
            // Since by this point we know that no time value has been inserted which is
            // outside the epoch range, we know that no document can meet this criteria
        case MatchExpression::EQ:
        case MatchExpression::INTERNAL_EXPR_EQ:
            return {std::make_unique<AlwaysFalseMatchExpression>()};
        case MatchExpression::GT:
        case MatchExpression::INTERNAL_EXPR_GT:
        case MatchExpression::GTE:
        case MatchExpression::INTERNAL_EXPR_GTE:
            if (timestamp < 0LL) {
                // Since by this point we know that no time value has been inserted < 0,
                // every document must meet this criteria
                return {std::make_unique<AlwaysTrueMatchExpression>()};
            }
            // If we are here we are guaranteed that 'timestamp > max32BitEpochMillis'. Since by
            // this point we know that no time value has been inserted > max32BitEpochMillis, we
            // know that no document can meet this criteria
            return {std::make_unique<AlwaysFalseMatchExpression>()};
        case MatchExpression::LT:
        case MatchExpression::INTERNAL_EXPR_LT:
        case MatchExpression::LTE:
        case MatchExpression::INTERNAL_EXPR_LTE:
            if (timestamp < 0LL) {
                // Since by this point we know that no time value has been inserted < 0,
                // we know that no document can meet this criteria
                return {std::make_unique<AlwaysFalseMatchExpression>()};
            }
            // If we are here we are guaranteed that 'timestamp > max32BitEpochMillis'. Since by
            // this point we know that no time value has been inserted > 0xffffffff every time value
            // must be less than this value
            return {std::make_unique<AlwaysTrueMatchExpression>()};
        default:
            MONGO_UNREACHABLE_TASSERT(7823305);
    }
}
}  // namespace

BucketLevelComparisonPredicateGeneratorBase::Output
BucketLevelComparisonPredicateGeneratorBase::createTightPredicate(
    const ComparisonMatchExpressionBase* matchExpr) const {
    const auto matchExprPath = matchExpr->path();
    const auto matchExprData = matchExpr->getData();

    const auto error =
        checkComparisonPredicateEligibility(matchExpr,
                                            matchExprPath,
                                            matchExprData,
                                            _params.bucketSpec,
                                            _params.pExpCtx->collationMatchesDefault);
    if (error) {
        return {BucketSpec::handleIneligible(_params.policy, std::move(matchExpr), *error)
                    .loosePredicate};
    }

    // We have to disable the tight predicate for the measurement field. There might be missing
    // values in the measurements and the control fields ignore them on insertion. So we cannot
    // use bucket min and max to determine the property of all events in the bucket. For
    // measurement fields, there's a further problem that if the control field is an array, we
    // cannot generate the tight predicate because the predicate will be implicitly mapped over
    // the array elements.
    if (matchExprPath != _params.bucketSpec.timeField()) {
        return {BucketSpec::handleIneligible(BucketSpec::IneligiblePredicatePolicy::kIgnore,
                                             matchExpr,
                                             "can't create tight predicate on non-time field")
                    .tightPredicate};
    }
    auto minPath = std::string{timeseries::kControlMinFieldNamePrefix} + matchExprPath;
    const StringData minPathStringData(minPath);
    auto maxPath = std::string{timeseries::kControlMaxFieldNamePrefix} + matchExprPath;
    const StringData maxPathStringData(maxPath);

    switch (matchExpr->matchType()) {
        // All events satisfy $eq if bucket min and max both satisfy $eq.
        case MatchExpression::EQ:
            return {makeAnd(
                makeCmpMatchExpr<EqualityMatchExpression>(minPathStringData, matchExprData),
                makeCmpMatchExpr<EqualityMatchExpression>(maxPathStringData, matchExprData))};
        case MatchExpression::INTERNAL_EXPR_EQ:
            return {makeAnd(
                makeCmpMatchExpr<InternalExprEqMatchExpression>(minPathStringData, matchExprData),
                makeCmpMatchExpr<InternalExprEqMatchExpression>(maxPathStringData, matchExprData))};

        // All events satisfy $gt if bucket min satisfy $gt.
        case MatchExpression::GT:
            return {makeCmpMatchExpr<GTMatchExpression>(minPathStringData, matchExprData)};
        case MatchExpression::INTERNAL_EXPR_GT:
            return {
                makeCmpMatchExpr<InternalExprGTMatchExpression>(minPathStringData, matchExprData)};

        // All events satisfy $gte if bucket min satisfy $gte.
        case MatchExpression::GTE:
            return {makeCmpMatchExpr<GTEMatchExpression>(minPathStringData, matchExprData)};
        case MatchExpression::INTERNAL_EXPR_GTE:
            return {
                makeCmpMatchExpr<InternalExprGTEMatchExpression>(minPathStringData, matchExprData)};

        // All events satisfy $lt if bucket max satisfy $lt.
        case MatchExpression::LT:
            return {makeCmpMatchExpr<LTMatchExpression>(maxPathStringData, matchExprData)};
        case MatchExpression::INTERNAL_EXPR_LT:
            return {
                makeCmpMatchExpr<InternalExprLTMatchExpression>(maxPathStringData, matchExprData)};

        // All events satisfy $lte if bucket max satisfy $lte.
        case MatchExpression::LTE:
            return {makeCmpMatchExpr<LTEMatchExpression>(maxPathStringData, matchExprData)};
        case MatchExpression::INTERNAL_EXPR_LTE:
            return {
                makeCmpMatchExpr<InternalExprLTEMatchExpression>(maxPathStringData, matchExprData)};
        default:
            MONGO_UNREACHABLE_TASSERT(7026901);
    }
}

BucketLevelComparisonPredicateGeneratorBase::Output
BucketLevelComparisonPredicateGeneratorBase::createLoosePredicate(
    const ComparisonMatchExpressionBase* matchExpr) const {
    const auto matchExprPath = matchExpr->path();
    const auto matchExprData = matchExpr->getData();

    const auto error =
        checkComparisonPredicateEligibility(matchExpr,
                                            matchExprPath,
                                            matchExprData,
                                            _params.bucketSpec,
                                            _params.pExpCtx->collationMatchesDefault);
    if (error) {
        return {BucketSpec::handleIneligible(_params.policy, std::move(matchExpr), *error)
                    .loosePredicate};
    }
    const bool isTimeField = (matchExprPath == _params.bucketSpec.timeField());
    const auto minPath = std::string{timeseries::kControlMinFieldNamePrefix} + matchExprPath;
    const StringData minPathStringData(minPath);
    const auto maxPath = std::string{timeseries::kControlMaxFieldNamePrefix} + matchExprPath;
    const StringData maxPathStringData(maxPath);

    if (isTimeField) {
        Date_t timeField = matchExprData.Date();
        BSONObj maxTime = BSON("" << timeField + Seconds(_params.bucketMaxSpanSeconds));
        return generateTimeFieldPredicate(std::move(matchExpr),
                                          minPathStringData,
                                          maxPathStringData,
                                          timeField,
                                          maxTime,
                                          matchExprPath,
                                          matchExprData);
    } else {
        return generateNonTimeFieldPredicate(std::move(matchExpr),
                                             _params,
                                             minPathStringData,
                                             maxPathStringData,
                                             matchExprPath,
                                             matchExprData);
    }
}

BucketLevelComparisonPredicateGeneratorBase::Output
DefaultBucketLevelComparisonPredicateGenerator::generateTimeFieldPredicate(
    const ComparisonMatchExpressionBase* matchExpr,
    StringData minPathStringData,
    StringData maxPathStringData,
    Date_t timeField,
    BSONObj maxTime,
    StringData matchExprPath,
    const BSONElement& matchExprData) const {
    BSONObj minTime = BSON("" << timeField - Seconds(_params.bucketMaxSpanSeconds));
    // The date is in the "extended" range if it doesn't fit into the bottom 32 bits.
    long long timestamp = timeField.toMillisSinceEpoch();
    bool dateIsExtended = timestamp < 0LL || timestamp > max32BitEpochMillis;
    if (dateIsExtended) {
        return handleExtendedDateRanges(std::move(matchExpr), timestamp);
    };

    switch (matchExpr->matchType()) {
        case MatchExpression::EQ:
        case MatchExpression::INTERNAL_EXPR_EQ:
            // For $eq, make both a $lte against 'control.min' and a $gte predicate against
            // 'control.max'.
            //
            // Since we haven't stored a time outside of the 32 bit range, we include a
            // predicate against the _id field which is converted to the maximum for the
            // corresponding range of ObjectIds and is adjusted by the max range for a bucket to
            // approximate the max bucket value given the min. Also include a predicate against
            // the _id field which is converted to the minimum for the range of ObjectIds
            // corresponding to the given date. In addition, we include a {'control.min' :
            // {$gte: 'time - bucketMaxSpanSeconds'}} and a {'control.max' : {$lte: 'time +
            // bucketMaxSpanSeconds'}} predicate which will be helpful in reducing bounds for
            // index scans on 'time' field and routing on mongos.
            return {makeAnd(
                makeCmpMatchExpr<InternalExprLTEMatchExpression>(minPathStringData, matchExprData),
                makeCmpMatchExpr<InternalExprGTEMatchExpression>(minPathStringData,
                                                                 minTime.firstElement()),
                makeCmpMatchExpr<InternalExprGTEMatchExpression>(maxPathStringData, matchExprData),
                makeCmpMatchExpr<InternalExprLTEMatchExpression>(maxPathStringData,
                                                                 maxTime.firstElement()),
                makeCmpMatchExpr<LTEMatchExpression, Value>(
                    timeseries::kBucketIdFieldName,
                    constructObjectIdValue<LTEMatchExpression>(matchExprData,
                                                               _params.bucketMaxSpanSeconds)),
                makeCmpMatchExpr<GTEMatchExpression, Value>(
                    timeseries::kBucketIdFieldName,
                    constructObjectIdValue<GTEMatchExpression>(matchExprData,
                                                               _params.bucketMaxSpanSeconds)))};
        case MatchExpression::GT:
        case MatchExpression::INTERNAL_EXPR_GT:
            // For $gt, make a $gt predicate against 'control.max'. If the collection doesn't
            // contain times outside the 32 bit range, include a predicate against the _id field
            // which is converted to the maximum for the corresponding range of ObjectIds and is
            // adjusted by the max range for a bucket to approximate the max bucket value given
            // the min.
            //
            // In addition, we include a {'control.min' : {$gt: 'time - bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time'
            // field and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$gt: [...]}} that can be rewritten to use $_internalExprGt.
            return {makeAnd(
                makeCmpMatchExpr<InternalExprGTMatchExpression>(maxPathStringData, matchExprData),
                makeCmpMatchExpr<InternalExprGTMatchExpression>(minPathStringData,
                                                                minTime.firstElement()),
                makeCmpMatchExpr<GTMatchExpression, Value>(
                    timeseries::kBucketIdFieldName,
                    constructObjectIdValue<GTMatchExpression>(matchExprData,
                                                              _params.bucketMaxSpanSeconds)))};
        case MatchExpression::GTE:
        case MatchExpression::INTERNAL_EXPR_GTE:
            // For $gte, make a $gte predicate against 'control.max'. In addition, since the
            // collection doesn't contain times outside the 32 bit range, include a predicate
            // against the _id field which is converted to the minimum for the corresponding
            // range of ObjectIds and is adjusted by the max range for a bucket to approximate
            // the max bucket value given the min. In addition, we include a {'control.min' :
            // {$gte: 'time - bucketMaxSpanSeconds'}} predicate which will be helpful in
            // reducing bounds for index scans on 'time' field and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$gte: [...]}} that can be rewritten to use $_internalExprGte.
            return {makeAnd(
                makeCmpMatchExpr<InternalExprGTEMatchExpression>(maxPathStringData, matchExprData),
                makeCmpMatchExpr<InternalExprGTEMatchExpression>(minPathStringData,
                                                                 minTime.firstElement()),
                makeCmpMatchExpr<GTEMatchExpression, Value>(
                    timeseries::kBucketIdFieldName,
                    constructObjectIdValue<GTEMatchExpression>(matchExprData,
                                                               _params.bucketMaxSpanSeconds)))};

        case MatchExpression::LT:
        case MatchExpression::INTERNAL_EXPR_LT:
            // For $lt, make a $lt predicate against 'control.min'. The comparison is against
            // the 'time' field, so we include a predicate against the _id field which is
            // converted to the minimum for the corresponding range of ObjectIds, unless the
            // collection contain extended range dates which won't fit int the 32 bits allocated
            // for _id.
            //
            // In addition, we include a {'control.max' : {$lt: 'time + bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time'
            // field and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$lt: [...]}} that can be rewritten to use $_internalExprLt.

            return {makeAnd(
                makeCmpMatchExpr<InternalExprLTMatchExpression>(minPathStringData, matchExprData),
                makeCmpMatchExpr<InternalExprLTMatchExpression>(maxPathStringData,
                                                                maxTime.firstElement()),
                makeCmpMatchExpr<LTMatchExpression, Value>(
                    timeseries::kBucketIdFieldName,
                    constructObjectIdValue<LTMatchExpression>(matchExprData,
                                                              _params.bucketMaxSpanSeconds)))};

        case MatchExpression::LTE:
        case MatchExpression::INTERNAL_EXPR_LTE:
            // For $lte, make a $lte predicate against 'control.min'. In addition, if the
            // collection doesn't contain times outside the 32 bit range, include a predicate
            // against the _id field which is converted to the maximum for the corresponding
            // range of ObjectIds. In addition, we include a {'control.max' : {$lte: 'time +
            // bucketMaxSpanSeconds'}} predicate which will be helpful in reducing bounds for
            // index scans on 'time' field and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$lte: [...]}} that can be rewritten to use $_internalExprLte.

            return {makeAnd(
                makeCmpMatchExpr<InternalExprLTEMatchExpression>(minPathStringData, matchExprData),
                makeCmpMatchExpr<InternalExprLTEMatchExpression>(maxPathStringData,
                                                                 maxTime.firstElement()),
                makeCmpMatchExpr<LTEMatchExpression, Value>(
                    timeseries::kBucketIdFieldName,
                    constructObjectIdValue<LTEMatchExpression>(matchExprData,
                                                               _params.bucketMaxSpanSeconds)))};
        default:
            MONGO_UNREACHABLE_TASSERT(7823301);
    }
}

BucketLevelComparisonPredicateGeneratorBase::Output
ExtendedRangeBucketLevelComparisonPredicateGenerator::generateTimeFieldPredicate(
    const ComparisonMatchExpressionBase* matchExpr,
    StringData minPathStringData,
    StringData maxPathStringData,
    Date_t timeField,
    BSONObj maxTime,
    StringData matchExprPath,
    const BSONElement& matchExprData) const {
    BSONObj minTime = BSON("" << timeField - Seconds(_params.bucketMaxSpanSeconds));
    std::string minPath = minPathStringData.toString();
    std::string maxPath = maxPathStringData.toString();

    switch (matchExpr->matchType()) {
        case MatchExpression::EQ:
        case MatchExpression::INTERNAL_EXPR_EQ:
            return {makeAnd(
                makeCmpMatchExpr<InternalExprLTEMatchExpression>(minPath, matchExprData),
                makeCmpMatchExpr<InternalExprGTEMatchExpression>(minPath, minTime.firstElement()),
                makeCmpMatchExpr<InternalExprGTEMatchExpression>(maxPath, matchExprData),
                makeCmpMatchExpr<InternalExprLTEMatchExpression>(maxPath, maxTime.firstElement()))};

        case MatchExpression::GT:
        case MatchExpression::INTERNAL_EXPR_GT:
            return {makeAnd(
                makeCmpMatchExpr<InternalExprGTMatchExpression>(maxPath, matchExprData),
                makeCmpMatchExpr<InternalExprGTMatchExpression>(minPath, minTime.firstElement()))};

        case MatchExpression::GTE:
        case MatchExpression::INTERNAL_EXPR_GTE:
            return {makeAnd(
                makeCmpMatchExpr<InternalExprGTEMatchExpression>(maxPath, matchExprData),
                makeCmpMatchExpr<InternalExprGTEMatchExpression>(minPath, minTime.firstElement()))};

        case MatchExpression::LT:
        case MatchExpression::INTERNAL_EXPR_LT:
            return {makeAnd(
                makeCmpMatchExpr<InternalExprLTMatchExpression>(minPath, matchExprData),
                makeCmpMatchExpr<InternalExprLTMatchExpression>(maxPath, maxTime.firstElement()))};
        case MatchExpression::LTE:
        case MatchExpression::INTERNAL_EXPR_LTE:
            return {makeAnd(
                makeCmpMatchExpr<InternalExprLTEMatchExpression>(minPath, matchExprData),
                makeCmpMatchExpr<InternalExprLTEMatchExpression>(maxPath, maxTime.firstElement()))};
        default:
            MONGO_UNREACHABLE_TASSERT(7823302);
    }
}

BucketLevelComparisonPredicateGeneratorBase::Output
FixedBucketsLevelComparisonPredicateGenerator::generateTimeFieldPredicate(
    const ComparisonMatchExpressionBase* matchExpr,
    StringData minPathStringData,
    StringData maxPathStringData,
    Date_t timeField,
    BSONObj maxTime,
    StringData matchExprPath,
    const BSONElement& matchExprData) const {
    Date_t roundedTimeField =
        timeseries::roundTimestampBySeconds(timeField, _params.bucketMaxSpanSeconds);
    BSONObj minTime = BSON("" << roundedTimeField);
    // The date is in the "extended" range if it doesn't fit into the bottom 32 bits.
    long long timestamp = timeField.toMillisSinceEpoch();
    bool dateIsExtended = timestamp < 0LL || timestamp > max32BitEpochMillis;

    if (dateIsExtended) {
        return handleExtendedDateRanges(std::move(matchExpr), timestamp);
    }

    // For some queries with fixed buckets, we do not need to filter individual measurements or
    // whole buckets, because it is guaranteed that all buckets returned from the index scan
    // will only hold relevant measurements if the predicate aligns with the bucket boundaries.
    // This is only true for predicates that only use $gte, and $lt. For $gt, $lte, and $eq we
    // still need an eventFilter to correctly return relevant measurements. For example, for the
    // predicate {$gt: 10:00} with bucketMaxSpanSeconds at 3600, the bucket spanning from
    // 10:00-11:00 will hold relevant measurements, but we need the eventFilter to filter out values
    // equal to 10:00 in the bucket. Similarly, for {$lte: 10:00}, the bucket 10:00-11:00 could have
    // values equal to 10:00, but we need the eventFilter to remove values greater than 10:00.
    bool rewriteProvidesExactMatchPredicate =
        timeseries::roundTimestampBySeconds(timeField, _params.bucketMaxSpanSeconds) == timeField;

    switch (matchExpr->matchType()) {
        case MatchExpression::EQ:
        case MatchExpression::INTERNAL_EXPR_EQ:
            // For $eq, make both a $lte against 'control.min' and a $gte predicate against
            // 'control.max'.
            //
            // Since we haven't stored a time outside of the 32 bit range, we include a
            // predicate against the _id field which is converted to the maximum for the
            // corresponding range of ObjectIds and is adjusted by the max range for a bucket to
            // approximate the max bucket value given the min. Also include a predicate against
            // the _id field which is converted to the minimum for the range of ObjectIds
            // corresponding to the given date. In addition, we include a {'control.min' :
            // {$gte: 'time - bucketMaxSpanSeconds'}} and a {'control.max' : {$lte: 'time +
            // bucketMaxSpanSeconds'}} predicate which will be helpful in reducing bounds for
            // index scans on 'time' field and routing on mongos.
            return {makeAnd(
                makeCmpMatchExpr<InternalExprLTEMatchExpression>(minPathStringData, matchExprData),
                makeCmpMatchExpr<InternalExprGTEMatchExpression>(minPathStringData,
                                                                 minTime.firstElement()),
                makeCmpMatchExpr<InternalExprGTEMatchExpression>(maxPathStringData, matchExprData),
                makeCmpMatchExpr<InternalExprLTEMatchExpression>(maxPathStringData,
                                                                 maxTime.firstElement()),
                makeCmpMatchExpr<LTEMatchExpression, Value>(
                    timeseries::kBucketIdFieldName,
                    constructObjectIdValue<LTEMatchExpression>(matchExprData,
                                                               _params.bucketMaxSpanSeconds)),
                makeCmpMatchExpr<GTEMatchExpression, Value>(
                    timeseries::kBucketIdFieldName,
                    constructObjectIdValue<GTEMatchExpression>(matchExprData,
                                                               _params.bucketMaxSpanSeconds)))};
        case MatchExpression::GT:
        case MatchExpression::INTERNAL_EXPR_GT:
            // For $gt, make a $gt predicate against 'control.max'. If the collection doesn't
            // contain times outside the 32 bit range, include a predicate against the _id field
            // which is converted to the maximum for the corresponding range of ObjectIds and is
            // adjusted by the max range for a bucket to approximate the max bucket value given
            // the min.
            //
            // In addition, we include a {'control.min' : {$gt: 'roundedTime'}} where
            // 'roundedTime' is 'time' rounded down by bucketRoundingSeconds predicate which
            // will be helpful in reducing bounds for index scans on 'time' field and routing on
            // mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$gt: [...]}} that can be rewritten to use $_internalExprGt.
            return {makeAnd(
                makeCmpMatchExpr<InternalExprGTMatchExpression>(maxPathStringData, matchExprData),
                makeCmpMatchExpr<InternalExprGTEMatchExpression>(minPathStringData,
                                                                 minTime.firstElement()),
                makeCmpMatchExpr<GTMatchExpression, Value>(
                    timeseries::kBucketIdFieldName,
                    constructObjectIdValue<GTMatchExpression>(matchExprData,
                                                              _params.bucketMaxSpanSeconds)))};
        case MatchExpression::GTE:
        case MatchExpression::INTERNAL_EXPR_GTE:
            // For $gte, make a $gte predicate against 'control.max'. In addition, since the
            // collection doesn't contain times outside the 32 bit range, include a predicate
            // against the _id field which is converted to the minimum for the corresponding
            // range of ObjectIds and is adjusted by the max range for a bucket to approximate
            // the max bucket value given the min. In addition, we include a {'control.min' :
            // {$gt: 'roundedTime'}} where 'roundedTime' is 'time' rounded down by
            // bucketRoundingSeconds predicate which will be helpful in reducing bounds for
            // index scans on 'time' field and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$gte: [...]}} that can be rewritten to use $_internalExprGte.
            return {makeAnd(makeCmpMatchExpr<InternalExprGTEMatchExpression>(maxPathStringData,
                                                                             matchExprData),
                            makeCmpMatchExpr<InternalExprGTEMatchExpression>(
                                minPathStringData, minTime.firstElement()),
                            makeCmpMatchExpr<GTEMatchExpression, Value>(
                                timeseries::kBucketIdFieldName,
                                constructObjectIdValue<GTEMatchExpression>(
                                    matchExprData, _params.bucketMaxSpanSeconds))),
                    rewriteProvidesExactMatchPredicate};
        case MatchExpression::LT:
        case MatchExpression::INTERNAL_EXPR_LT:
            // For $lt, make a $lt predicate against 'control.min'. The comparison is against
            // the 'time' field, so we include a predicate against the _id field which is
            // converted to the minimum for the corresponding range of ObjectIds, unless the
            // collection contain extended range dates which won't fit int the 32 bits allocated
            // for _id.
            //
            // In addition, we include a {'control.max' : {$lt: 'time + bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time'
            // field and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$lt: [...]}} that can be rewritten to use $_internalExprLt.
            return {makeAnd(makeCmpMatchExpr<InternalExprLTMatchExpression>(minPathStringData,
                                                                            matchExprData),
                            makeCmpMatchExpr<InternalExprLTMatchExpression>(maxPathStringData,
                                                                            maxTime.firstElement()),
                            makeCmpMatchExpr<LTMatchExpression, Value>(
                                timeseries::kBucketIdFieldName,
                                constructObjectIdValue<LTMatchExpression>(
                                    matchExprData, _params.bucketMaxSpanSeconds))),
                    rewriteProvidesExactMatchPredicate};
        case MatchExpression::LTE:
        case MatchExpression::INTERNAL_EXPR_LTE:
            // For $lte, make a $lte predicate against 'control.min'. In addition, if the
            // collection doesn't contain times outside the 32 bit range, include a predicate
            // against the _id field which is converted to the maximum for the corresponding
            // range of ObjectIds. In addition, we include a {'control.max' : {$lte: 'time +
            // bucketMaxSpanSeconds'}} predicate which will be helpful in reducing bounds for
            // index scans on 'time' field and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$lte: [...]}} that can be rewritten to use $_internalExprLte.
            return {makeAnd(
                makeCmpMatchExpr<InternalExprLTEMatchExpression>(minPathStringData, matchExprData),
                makeCmpMatchExpr<InternalExprLTEMatchExpression>(maxPathStringData,
                                                                 maxTime.firstElement()),
                makeCmpMatchExpr<LTEMatchExpression, Value>(
                    timeseries::kBucketIdFieldName,
                    constructObjectIdValue<LTEMatchExpression>(matchExprData,
                                                               _params.bucketMaxSpanSeconds)))};
        default:
            MONGO_UNREACHABLE_TASSERT(7823303);
    }
}

std::unique_ptr<BucketLevelComparisonPredicateGeneratorBase>
BucketLevelComparisonPredicateGenerator::getBuilder(
    BucketLevelComparisonPredicateGeneratorBase::Params params) {
    if (params.bucketSpec.usesExtendedRange()) {
        return std::make_unique<ExtendedRangeBucketLevelComparisonPredicateGenerator>(
            std::move(params));
    }
    if (params.fixedBuckets) {
        return std::make_unique<FixedBucketsLevelComparisonPredicateGenerator>(std::move(params));
    }
    return std::make_unique<DefaultBucketLevelComparisonPredicateGenerator>(std::move(params));
}

}  // namespace timeseries
}  // namespace mongo
