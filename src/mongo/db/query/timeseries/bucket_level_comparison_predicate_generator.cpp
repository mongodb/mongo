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

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


namespace mongo {
namespace timeseries {

namespace {
static const long long max32BitEpochMillis =
    static_cast<long long>(std::numeric_limits<uint32_t>::max()) * 1000;

// Checks for the situations when it's not possible to create a bucket-level predicate (against the
// computed control values) for the given event-level predicate ('matchExpr').
boost::optional<StringData> checkComparisonPredicateEligibility(
    const ComparisonMatchExpressionBase* matchExpr,
    const StringData matchExprPath,
    const BSONElement& matchExprData,
    const BucketSpec& bucketSpec,
    ExpressionContextCollationMatchesDefault collationMatchesDefault) {
    // The control field's min and max are chosen using a field-order insensitive comparator, while
    // MatchExpressions use a comparator that treats field-order as significant. Because of this we
    // will not perform this optimization on queries with operands of compound types.
    if (matchExprData.type() == BSONType::object || matchExprData.type() == BSONType::array)
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
    //    3) if the collection might have mixed schema data, we'll compare the types of min and
    //       max when _creating_ the bucket-level predicate (that check won't help with missing).
    if (matchExprData.type() == BSONType::null)
        return "can't handle comparison to null"_sd;
    if (!isTimeField &&
        (matchExpr->matchType() == MatchExpression::INTERNAL_EXPR_LTE ||
         matchExpr->matchType() == MatchExpression::INTERNAL_EXPR_LT)) {
        return "can't handle a non-type-bracketing LT or LTE comparisons"_sd;
    }

    // The control field's min and max are chosen based on the collation of the collection. If the
    // query's collation does not match the collection's collation and the query operand is a
    // string or compound type (skipped above) we will not perform this optimization.
    if (collationMatchesDefault == ExpressionContextCollationMatchesDefault::kNo &&
        matchExprData.type() == BSONType::string) {
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
    if (bucketSpec.fieldIsComputed(std::string{matchExprPath})) {
        return "can't handle a computed field"_sd;
    }

    // We must avoid mapping predicates on fields removed by $project.
    if (!determineIncludeField(matchExprPath, bucketSpec.behavior(), bucketSpec.fieldSet())) {
        return "can't handle a field removed by projection"_sd;
    }

    if (isTimeField && matchExprData.type() != BSONType::date) {
        // Users are not allowed to insert non-date measurements into the time field. So this query
        // would not match anything. We do not need to optimize for this case.
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
 * However, if the timeseries collection has no mixed-schema data then this type-equality predicate
 * is unnecessary. In that case this function returns an empty, always-true predicate.
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
                                            _params.pExpCtx->getCollationMatchesDefault());
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
                                            _params.pExpCtx->getCollationMatchesDefault());
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
                                                                 maxTime.firstElement()))};
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
                                                                minTime.firstElement()))};
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
                                                                 minTime.firstElement()))};

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
                                                                maxTime.firstElement()))};

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
                                                                 maxTime.firstElement()))};
        default:
            MONGO_UNREACHABLE_TASSERT(7823301);
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
                                                                 maxTime.firstElement()))};
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
                                                                 minTime.firstElement()))};
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
                                minPathStringData, minTime.firstElement())),
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
                            makeCmpMatchExpr<InternalExprLTMatchExpression>(
                                maxPathStringData, maxTime.firstElement())),
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
                                                                 maxTime.firstElement()))};
        default:
            MONGO_UNREACHABLE_TASSERT(7823303);
    }
}

std::unique_ptr<BucketLevelComparisonPredicateGeneratorBase>
BucketLevelComparisonPredicateGenerator::getBuilder(
    BucketLevelComparisonPredicateGeneratorBase::Params params) {
    if (!params.bucketSpec.usesExtendedRange() && params.fixedBuckets) {
        // Fixed bucket optimizations are not compatible with extended range data
        return std::make_unique<FixedBucketsLevelComparisonPredicateGenerator>(std::move(params));
    }

    return std::make_unique<DefaultBucketLevelComparisonPredicateGenerator>(std::move(params));
}

}  // namespace timeseries
}  // namespace mongo
