/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/timeseries/bucket_spec.h"

#include <algorithm>

#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/rewrite_expr.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using IneligiblePredicatePolicy = BucketSpec::IneligiblePredicatePolicy;

bool BucketSpec::fieldIsComputed(StringData field) const {
    return std::any_of(
        _computedMetaProjFields.begin(), _computedMetaProjFields.end(), [&](auto& s) {
            return s == field || expression::isPathPrefixOf(field, s) ||
                expression::isPathPrefixOf(s, field);
        });
}

namespace {
constexpr long long max32BitEpochMillis =
    static_cast<long long>(std::numeric_limits<uint32_t>::max()) * 1000;

/**
 * Creates an ObjectId initialized with an appropriate timestamp corresponding to 'rhs' and
 * returns it as a Value.
 */
template <typename MatchType>
auto constructObjectIdValue(const BSONElement& rhs, int bucketMaxSpanSeconds) {
    // Indicates whether to initialize an ObjectId with a max or min value for the non-date bytes.
    enum class OIDInit : bool { max, min };
    // Make an ObjectId cooresponding to a date value. As a conversion from date to ObjectId will
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

BucketSpec::BucketPredicate handleIneligible(IneligiblePredicatePolicy policy,
                                             const MatchExpression* matchExpr,
                                             StringData message) {
    switch (policy) {
        case IneligiblePredicatePolicy::kError:
            uasserted(
                5916301,
                "Error translating non-metadata time-series predicate to operate on buckets: " +
                    message + ": " + matchExpr->serialize().toString());
        case IneligiblePredicatePolicy::kIgnore:
            return {};
    }
    MONGO_UNREACHABLE_TASSERT(5916307);
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
    const StringData& matchExprPath,
    bool assumeNoMixedSchemaData) {

    std::vector<std::unique_ptr<MatchExpression>> typeEqualityPredicates;

    if (assumeNoMixedSchemaData)
        return makeOr(std::move(typeEqualityPredicates));

    FieldPath matchExprField(matchExprPath);
    using namespace timeseries;

    // Assume that we're generating a predicate on "a.b"
    for (size_t i = 0; i < matchExprField.getPathLength(); i++) {
        auto minPath = std::string{kControlMinFieldNamePrefix} + matchExprField.getSubpath(i);
        auto maxPath = std::string{kControlMaxFieldNamePrefix} + matchExprField.getSubpath(i);

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

boost::optional<StringData> checkComparisonPredicateErrors(
    const MatchExpression* matchExpr,
    const StringData matchExprPath,
    const BSONElement& matchExprData,
    const BucketSpec& bucketSpec,
    ExpressionContext::CollationMatchesDefault collationMatchesDefault) {
    using namespace timeseries;
    // The control field's min and max are chosen using a field-order insensitive comparator, while
    // MatchExpressions use a comparator that treats field-order as significant. Because of this we
    // will not perform this optimization on queries with operands of compound types.
    if (matchExprData.type() == BSONType::Object || matchExprData.type() == BSONType::Array)
        return "operand can't be an object or array"_sd;

    // MatchExpressions have special comparison semantics regarding null, in that {$eq: null} will
    // match all documents where the field is either null or missing. Because this is different
    // from both the comparison semantics that InternalExprComparison expressions and the control's
    // min and max fields use, we will not perform this optimization on queries with null operands.
    if (matchExprData.type() == BSONType::jstNULL)
        return "can't handle {$eq: null}"_sd;

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
        tasserted(
            6707200,
            str::stream() << "createComparisonPredicate() does not handle metadata predicates: "
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

    const auto isTimeField = (matchExprPath == bucketSpec.timeField());
    if (isTimeField && matchExprData.type() != BSONType::Date) {
        // Users are not allowed to insert non-date measurements into time field. So this query
        // would not match anything. We do not need to optimize for this case.
        return "This predicate will never be true, because the time field always contains a Date"_sd;
    }

    return boost::none;
}

std::unique_ptr<MatchExpression> createComparisonPredicate(
    const ComparisonMatchExpressionBase* matchExpr,
    const BucketSpec& bucketSpec,
    int bucketMaxSpanSeconds,
    boost::intrusive_ptr<ExpressionContext> pExpCtx,
    bool haveComputedMetaField,
    bool includeMetaField,
    bool assumeNoMixedSchemaData,
    IneligiblePredicatePolicy policy) {
    using namespace timeseries;
    const auto matchExprPath = matchExpr->path();
    const auto matchExprData = matchExpr->getData();

    const auto error = checkComparisonPredicateErrors(
        matchExpr, matchExprPath, matchExprData, bucketSpec, pExpCtx->collationMatchesDefault);
    if (error) {
        return handleIneligible(policy, matchExpr, *error).loosePredicate;
    }

    const auto isTimeField = (matchExprPath == bucketSpec.timeField());
    auto minPath = std::string{kControlMinFieldNamePrefix} + matchExprPath;
    const StringData minPathStringData(minPath);
    auto maxPath = std::string{kControlMaxFieldNamePrefix} + matchExprPath;
    const StringData maxPathStringData(maxPath);

    BSONObj minTime;
    BSONObj maxTime;
    bool dateIsExtended = false;
    if (isTimeField) {
        auto timeField = matchExprData.Date();
        minTime = BSON("" << timeField - Seconds(bucketMaxSpanSeconds));
        maxTime = BSON("" << timeField + Seconds(bucketMaxSpanSeconds));

        // The date is in the "extended" range if it doesn't fit into the bottom
        // 32 bits.
        long long timestamp = timeField.toMillisSinceEpoch();
        dateIsExtended = timestamp < 0LL || timestamp > max32BitEpochMillis;
    }

    switch (matchExpr->matchType()) {
        case MatchExpression::EQ:
        case MatchExpression::INTERNAL_EXPR_EQ:
            // For $eq, make both a $lte against 'control.min' and a $gte predicate against
            // 'control.max'.
            //
            // If the comparison is against the 'time' field and we haven't stored a time outside of
            // the 32 bit range, include a predicate against the _id field which is converted to
            // the maximum for the corresponding range of ObjectIds and
            // is adjusted by the max range for a bucket to approximate the max bucket value given
            // the min. Also include a predicate against the _id field which is converted to the
            // minimum for the range of ObjectIds corresponding to the given date. In
            // addition, we include a {'control.min' : {$gte: 'time - bucketMaxSpanSeconds'}} and
            // a {'control.max' : {$lte: 'time + bucketMaxSpanSeconds'}} predicate which will be
            // helpful in reducing bounds for index scans on 'time' field and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$eq: [...]}} that can be rewritten to use $_internalExprEq.
            if (!isTimeField) {
                return makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                    makePredicate(MatchExprPredicate<InternalExprLTEMatchExpression>(
                                      minPathStringData, matchExprData),
                                  MatchExprPredicate<InternalExprGTEMatchExpression>(
                                      maxPathStringData, matchExprData)),
                    createTypeEqualityPredicate(pExpCtx, matchExprPath, assumeNoMixedSchemaData)));
            } else if (bucketSpec.usesExtendedRange()) {
                return makePredicate(
                    MatchExprPredicate<InternalExprLTEMatchExpression>(minPath, matchExprData),
                    MatchExprPredicate<InternalExprGTEMatchExpression>(minPath,
                                                                       minTime.firstElement()),
                    MatchExprPredicate<InternalExprGTEMatchExpression>(maxPath, matchExprData),
                    MatchExprPredicate<InternalExprLTEMatchExpression>(maxPath,
                                                                       maxTime.firstElement()));
            } else if (dateIsExtended) {
                // Since by this point we know that no time value has been inserted which is
                // outside the epoch range, we know that no document can meet this criteria
                return std::make_unique<AlwaysFalseMatchExpression>();
            } else {
                return makePredicate(
                    MatchExprPredicate<InternalExprLTEMatchExpression>(minPathStringData,
                                                                       matchExprData),
                    MatchExprPredicate<InternalExprGTEMatchExpression>(minPathStringData,
                                                                       minTime.firstElement()),
                    MatchExprPredicate<InternalExprGTEMatchExpression>(maxPathStringData,
                                                                       matchExprData),
                    MatchExprPredicate<InternalExprLTEMatchExpression>(maxPathStringData,
                                                                       maxTime.firstElement()),
                    MatchExprPredicate<LTEMatchExpression, Value>(
                        kBucketIdFieldName,
                        constructObjectIdValue<LTEMatchExpression>(matchExprData,
                                                                   bucketMaxSpanSeconds)),
                    MatchExprPredicate<GTEMatchExpression, Value>(
                        kBucketIdFieldName,
                        constructObjectIdValue<GTEMatchExpression>(matchExprData,
                                                                   bucketMaxSpanSeconds)));
            }
            MONGO_UNREACHABLE_TASSERT(6646903);

        case MatchExpression::GT:
        case MatchExpression::INTERNAL_EXPR_GT:
            // For $gt, make a $gt predicate against 'control.max'. In addition, if the comparison
            // is against the 'time' field, and the collection doesn't contain times outside the
            // 32 bit range, include a predicate against the _id field which is converted to the
            // maximum for the corresponding range of ObjectIds and is adjusted by the max range
            // for a bucket to approximate the max bucket value given the min.
            //
            // In addition, we include a {'control.min' : {$gt: 'time - bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time' field
            // and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$gt: [...]}} that can be rewritten to use $_internalExprGt.
            if (!isTimeField) {
                return makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                    std::make_unique<InternalExprGTMatchExpression>(maxPathStringData,
                                                                    matchExprData),
                    createTypeEqualityPredicate(pExpCtx, matchExprPath, assumeNoMixedSchemaData)));
            } else if (bucketSpec.usesExtendedRange()) {
                return makePredicate(
                    MatchExprPredicate<InternalExprGTMatchExpression>(maxPath, matchExprData),
                    MatchExprPredicate<InternalExprGTMatchExpression>(minPath,
                                                                      minTime.firstElement()));
            } else if (matchExprData.Date().toMillisSinceEpoch() < 0LL) {
                // Since by this point we know that no time value has been inserted < 0,
                // every document must meet this criteria
                return std::make_unique<AlwaysTrueMatchExpression>();
            } else if (matchExprData.Date().toMillisSinceEpoch() > max32BitEpochMillis) {
                // Since by this point we know that no time value has been inserted >
                // max32BitEpochMillis, we know that no document can meet this criteria
                return std::make_unique<AlwaysFalseMatchExpression>();
            } else {
                return makePredicate(MatchExprPredicate<InternalExprGTMatchExpression>(
                                         maxPathStringData, matchExprData),
                                     MatchExprPredicate<InternalExprGTMatchExpression>(
                                         minPathStringData, minTime.firstElement()),
                                     MatchExprPredicate<GTMatchExpression, Value>(
                                         kBucketIdFieldName,
                                         constructObjectIdValue<GTMatchExpression>(
                                             matchExprData, bucketMaxSpanSeconds)));
            }
            MONGO_UNREACHABLE_TASSERT(6646904);

        case MatchExpression::GTE:
        case MatchExpression::INTERNAL_EXPR_GTE:
            // For $gte, make a $gte predicate against 'control.max'. In addition, if the comparison
            // is against the 'time' field, and the collection doesn't contain times outside the
            // 32 bit range, include a predicate against the _id field which is
            // converted to the minimum for the corresponding range of ObjectIds and is adjusted
            // by the max range for a bucket to approximate the max bucket value given the min. In
            // addition, we include a {'control.min' : {$gte: 'time - bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time' field
            // and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$gte: [...]}} that can be rewritten to use $_internalExprGte.
            if (!isTimeField) {
                return makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                    std::make_unique<InternalExprGTEMatchExpression>(maxPathStringData,
                                                                     matchExprData),
                    createTypeEqualityPredicate(pExpCtx, matchExprPath, assumeNoMixedSchemaData)));
            } else if (bucketSpec.usesExtendedRange()) {
                return makePredicate(
                    MatchExprPredicate<InternalExprGTEMatchExpression>(maxPath, matchExprData),
                    MatchExprPredicate<InternalExprGTEMatchExpression>(minPath,
                                                                       minTime.firstElement()));
            } else if (matchExprData.Date().toMillisSinceEpoch() < 0LL) {
                // Since by this point we know that no time value has been inserted < 0,
                // every document must meet this criteria
                return std::make_unique<AlwaysTrueMatchExpression>();
            } else if (matchExprData.Date().toMillisSinceEpoch() > max32BitEpochMillis) {
                // Since by this point we know that no time value has been inserted > 0xffffffff,
                // we know that no value can meet this criteria
                return std::make_unique<AlwaysFalseMatchExpression>();
            } else {
                return makePredicate(MatchExprPredicate<InternalExprGTEMatchExpression>(
                                         maxPathStringData, matchExprData),
                                     MatchExprPredicate<InternalExprGTEMatchExpression>(
                                         minPathStringData, minTime.firstElement()),
                                     MatchExprPredicate<GTEMatchExpression, Value>(
                                         kBucketIdFieldName,
                                         constructObjectIdValue<GTEMatchExpression>(
                                             matchExprData, bucketMaxSpanSeconds)));
            }
            MONGO_UNREACHABLE_TASSERT(6646905);

        case MatchExpression::LT:
        case MatchExpression::INTERNAL_EXPR_LT:
            // For $lt, make a $lt predicate against 'control.min'. In addition, if the comparison
            // is against the 'time' field, include a predicate against the _id field which is
            // converted to the minimum for the corresponding range of ObjectIds, unless the
            // collection contain extended range dates which won't fit int the 32 bits allocated
            // for _id.
            //
            // In addition, we include a {'control.max' : {$lt: 'time + bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time' field
            // and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$lt: [...]}} that can be rewritten to use $_internalExprLt.
            if (!isTimeField) {
                return makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                    std::make_unique<InternalExprLTMatchExpression>(minPathStringData,
                                                                    matchExprData),
                    createTypeEqualityPredicate(pExpCtx, matchExprPath, assumeNoMixedSchemaData)));
            } else if (bucketSpec.usesExtendedRange()) {
                return makePredicate(
                    MatchExprPredicate<InternalExprLTMatchExpression>(minPath, matchExprData),
                    MatchExprPredicate<InternalExprLTMatchExpression>(maxPath,
                                                                      maxTime.firstElement()));
            } else if (matchExprData.Date().toMillisSinceEpoch() < 0LL) {
                // Since by this point we know that no time value has been inserted < 0,
                // we know that no document can meet this criteria
                return std::make_unique<AlwaysFalseMatchExpression>();
            } else if (matchExprData.Date().toMillisSinceEpoch() > max32BitEpochMillis) {
                // Since by this point we know that no time value has been inserted > 0xffffffff
                // every time value must be less than this value
                return std::make_unique<AlwaysTrueMatchExpression>();
            } else {
                return makePredicate(MatchExprPredicate<InternalExprLTMatchExpression>(
                                         minPathStringData, matchExprData),
                                     MatchExprPredicate<InternalExprLTMatchExpression>(
                                         maxPathStringData, maxTime.firstElement()),
                                     MatchExprPredicate<LTMatchExpression, Value>(
                                         kBucketIdFieldName,
                                         constructObjectIdValue<LTMatchExpression>(
                                             matchExprData, bucketMaxSpanSeconds)));
            }
            MONGO_UNREACHABLE_TASSERT(6646906);

        case MatchExpression::LTE:
        case MatchExpression::INTERNAL_EXPR_LTE:
            // For $lte, make a $lte predicate against 'control.min'. In addition, if the comparison
            // is against the 'time' field, and the collection doesn't contain times outside the
            // 32 bit range, include a predicate against the _id field which is
            // converted to the maximum for the corresponding range of ObjectIds. In
            // addition, we include a {'control.max' : {$lte: 'time + bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time' field
            // and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$lte: [...]}} that can be rewritten to use $_internalExprLte.
            if (!isTimeField) {
                return makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                    std::make_unique<InternalExprLTEMatchExpression>(minPathStringData,
                                                                     matchExprData),
                    createTypeEqualityPredicate(pExpCtx, matchExprPath, assumeNoMixedSchemaData)));
            } else if (bucketSpec.usesExtendedRange()) {
                return makePredicate(
                    MatchExprPredicate<InternalExprLTEMatchExpression>(minPath, matchExprData),
                    MatchExprPredicate<InternalExprLTEMatchExpression>(maxPath,
                                                                       maxTime.firstElement()));
            } else if (matchExprData.Date().toMillisSinceEpoch() < 0LL) {
                // Since by this point we know that no time value has been inserted < 0,
                // we know that no document can meet this criteria
                return std::make_unique<AlwaysFalseMatchExpression>();
            } else if (matchExprData.Date().toMillisSinceEpoch() > max32BitEpochMillis) {
                // Since by this point we know that no time value has been inserted > 0xffffffff
                // every document must be less than this value
                return std::make_unique<AlwaysTrueMatchExpression>();
            } else {
                return makePredicate(MatchExprPredicate<InternalExprLTEMatchExpression>(
                                         minPathStringData, matchExprData),
                                     MatchExprPredicate<InternalExprLTEMatchExpression>(
                                         maxPathStringData, maxTime.firstElement()),
                                     MatchExprPredicate<LTEMatchExpression, Value>(
                                         kBucketIdFieldName,
                                         constructObjectIdValue<LTEMatchExpression>(
                                             matchExprData, bucketMaxSpanSeconds)));
            }
            MONGO_UNREACHABLE_TASSERT(6646907);

        default:
            MONGO_UNREACHABLE_TASSERT(5348302);
    }

    MONGO_UNREACHABLE_TASSERT(5348303);
}

std::unique_ptr<MatchExpression> createTightComparisonPredicate(
    const ComparisonMatchExpressionBase* matchExpr,
    const BucketSpec& bucketSpec,
    ExpressionContext::CollationMatchesDefault collationMatchesDefault) {
    using namespace timeseries;
    const auto matchExprPath = matchExpr->path();
    const auto matchExprData = matchExpr->getData();

    const auto error = checkComparisonPredicateErrors(
        matchExpr, matchExprPath, matchExprData, bucketSpec, collationMatchesDefault);
    if (error) {
        return handleIneligible(BucketSpec::IneligiblePredicatePolicy::kIgnore, matchExpr, *error)
            .loosePredicate;
    }

    // We have to disable the tight predicate for the measurement field. There might be missing
    // values in the measurements and the control fields ignore them on insertion. So we cannot use
    // bucket min and max to determine the property of all events in the bucket. For measurement
    // fields, there's a further problem that if the control field is an array, we cannot generate
    // the tight predicate because the predicate will be implicitly mapped over the array elements.
    if (matchExprPath != bucketSpec.timeField()) {
        return handleIneligible(BucketSpec::IneligiblePredicatePolicy::kIgnore,
                                matchExpr,
                                "can't create tight predicate on non-time field")
            .tightPredicate;
    }

    auto minPath = std::string{kControlMinFieldNamePrefix} + matchExprPath;
    const StringData minPathStringData(minPath);
    auto maxPath = std::string{kControlMaxFieldNamePrefix} + matchExprPath;
    const StringData maxPathStringData(maxPath);

    switch (matchExpr->matchType()) {
        // All events satisfy $eq if bucket min and max both satisfy $eq.
        case MatchExpression::EQ:
            return makePredicate(
                MatchExprPredicate<EqualityMatchExpression>(minPathStringData, matchExprData),
                MatchExprPredicate<EqualityMatchExpression>(maxPathStringData, matchExprData));
        case MatchExpression::INTERNAL_EXPR_EQ:
            return makePredicate(
                MatchExprPredicate<InternalExprEqMatchExpression>(minPathStringData, matchExprData),
                MatchExprPredicate<InternalExprEqMatchExpression>(maxPathStringData,
                                                                  matchExprData));

        // All events satisfy $gt if bucket min satisfy $gt.
        case MatchExpression::GT:
            return std::make_unique<GTMatchExpression>(minPathStringData, matchExprData);
        case MatchExpression::INTERNAL_EXPR_GT:
            return std::make_unique<InternalExprGTMatchExpression>(minPathStringData,
                                                                   matchExprData);

        // All events satisfy $gte if bucket min satisfy $gte.
        case MatchExpression::GTE:
            return std::make_unique<GTEMatchExpression>(minPathStringData, matchExprData);
        case MatchExpression::INTERNAL_EXPR_GTE:
            return std::make_unique<InternalExprGTEMatchExpression>(minPathStringData,
                                                                    matchExprData);

        // All events satisfy $lt if bucket max satisfy $lt.
        case MatchExpression::LT:
            return std::make_unique<LTMatchExpression>(maxPathStringData, matchExprData);
        case MatchExpression::INTERNAL_EXPR_LT:
            return std::make_unique<InternalExprLTMatchExpression>(maxPathStringData,
                                                                   matchExprData);

        // All events satisfy $lte if bucket max satisfy $lte.
        case MatchExpression::LTE:
            return std::make_unique<LTEMatchExpression>(maxPathStringData, matchExprData);
        case MatchExpression::INTERNAL_EXPR_LTE:
            return std::make_unique<InternalExprLTEMatchExpression>(maxPathStringData,
                                                                    matchExprData);

        default:
            MONGO_UNREACHABLE_TASSERT(7026901);
    }
}

std::unique_ptr<MatchExpression> createTightExprComparisonPredicate(
    const ExprMatchExpression* matchExpr,
    const BucketSpec& bucketSpec,
    boost::intrusive_ptr<ExpressionContext> pExpCtx) {
    using namespace timeseries;
    auto rewriteMatchExpr = RewriteExpr::rewrite(matchExpr->getExpression(), pExpCtx->getCollator())
                                .releaseMatchExpression();
    if (rewriteMatchExpr &&
        ComparisonMatchExpressionBase::isInternalExprComparison(rewriteMatchExpr->matchType())) {
        auto compareMatchExpr =
            checked_cast<const ComparisonMatchExpressionBase*>(rewriteMatchExpr.get());
        return createTightComparisonPredicate(
            compareMatchExpr, bucketSpec, pExpCtx->collationMatchesDefault);
    }

    return handleIneligible(BucketSpec::IneligiblePredicatePolicy::kIgnore,
                            matchExpr,
                            "can't handle non-comparison $expr match expression")
        .tightPredicate;
}

}  // namespace

BucketSpec::BucketPredicate BucketSpec::createPredicatesOnBucketLevelField(
    const MatchExpression* matchExpr,
    const BucketSpec& bucketSpec,
    int bucketMaxSpanSeconds,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    bool haveComputedMetaField,
    bool includeMetaField,
    bool assumeNoMixedSchemaData,
    IneligiblePredicatePolicy policy) {

    tassert(5916304, "BucketSpec::createPredicatesOnBucketLevelField nullptr", matchExpr);

    // If we have a leaf predicate on a meta field, we can map it to the bucket's meta field.
    // This includes comparisons such as $eq and $lte, as well as other non-comparison predicates
    // such as $exists, or $mod. Unrenamable expressions can't be split into a whole bucket level
    // filter, when we should return nullptr.
    //
    // Metadata predicates are partially handled earlier, by splitting the match expression into a
    // metadata-only part, and measurement/time-only part. However, splitting a $match into two
    // sequential $matches only works when splitting a conjunction. A predicate like
    // {$or: [ {a: 5}, {meta.b: 5} ]} can't be split, and can't be metadata-only, so we have to
    // handle it here.
    const auto matchExprPath = matchExpr->path();
    if (!matchExprPath.empty() && bucketSpec.metaField() &&
        (matchExprPath == bucketSpec.metaField().value() ||
         expression::isPathPrefixOf(bucketSpec.metaField().value(), matchExprPath))) {

        if (haveComputedMetaField)
            return handleIneligible(policy, matchExpr, "can't handle a computed meta field");

        if (!includeMetaField)
            return handleIneligible(policy, matchExpr, "cannot handle an excluded meta field");

        if (expression::hasOnlyRenameableMatchExpressionChildren(*matchExpr)) {
            auto looseResult = matchExpr->clone();
            expression::applyRenamesToExpression(
                looseResult.get(),
                {{bucketSpec.metaField().value(), timeseries::kBucketMetaFieldName.toString()}});
            auto tightResult = looseResult->clone();
            return {std::move(looseResult), std::move(tightResult)};
        } else {
            return {nullptr, nullptr};
        }
    }

    if (matchExpr->matchType() == MatchExpression::AND) {
        auto nextAnd = static_cast<const AndMatchExpression*>(matchExpr);
        auto looseAndExpression = std::make_unique<AndMatchExpression>();
        auto tightAndExpression = std::make_unique<AndMatchExpression>();
        for (size_t i = 0; i < nextAnd->numChildren(); i++) {
            auto child = createPredicatesOnBucketLevelField(nextAnd->getChild(i),
                                                            bucketSpec,
                                                            bucketMaxSpanSeconds,
                                                            pExpCtx,
                                                            haveComputedMetaField,
                                                            includeMetaField,
                                                            assumeNoMixedSchemaData,
                                                            policy);
            if (child.loosePredicate) {
                looseAndExpression->add(std::move(child.loosePredicate));
            }

            if (tightAndExpression && child.tightPredicate) {
                tightAndExpression->add(std::move(child.tightPredicate));
            } else {
                // For tight expression, null means always false, we can short circuit here.
                tightAndExpression = nullptr;
            }
        }

        // For a loose predicate, if we are unable to generate an expression we can just treat it as
        // always true or an empty AND. This is because we are trying to generate a predicate that
        // will match the superset of our actual results.
        std::unique_ptr<MatchExpression> looseExpression = nullptr;
        if (looseAndExpression->numChildren() == 1) {
            looseExpression = looseAndExpression->releaseChild(0);
        } else if (looseAndExpression->numChildren() > 1) {
            looseExpression = std::move(looseAndExpression);
        }

        // For a tight predicate, if we are unable to generate an expression we can just treat it as
        // always false. This is because we are trying to generate a predicate that will match the
        // subset of our actual results.
        std::unique_ptr<MatchExpression> tightExpression = nullptr;
        if (tightAndExpression && tightAndExpression->numChildren() == 1) {
            tightExpression = tightAndExpression->releaseChild(0);
        } else {
            tightExpression = std::move(tightAndExpression);
        }

        return {std::move(looseExpression), std::move(tightExpression)};
    } else if (matchExpr->matchType() == MatchExpression::OR) {
        // Given {$or: [A, B]}, suppose A, B can be pushed down as A', B'.
        // If an event matches {$or: [A, B]} then either:
        //     - it matches A, which means any bucket containing it matches A'
        //     - it matches B, which means any bucket containing it matches B'
        // So {$or: [A', B']} will capture all the buckets we need to satisfy {$or: [A, B]}.
        auto nextOr = static_cast<const OrMatchExpression*>(matchExpr);
        auto looseOrExpression = std::make_unique<OrMatchExpression>();
        auto tightOrExpression = std::make_unique<OrMatchExpression>();

        for (size_t i = 0; i < nextOr->numChildren(); i++) {
            auto child = createPredicatesOnBucketLevelField(nextOr->getChild(i),
                                                            bucketSpec,
                                                            bucketMaxSpanSeconds,
                                                            pExpCtx,
                                                            haveComputedMetaField,
                                                            includeMetaField,
                                                            assumeNoMixedSchemaData,
                                                            policy);
            if (looseOrExpression && child.loosePredicate) {
                looseOrExpression->add(std::move(child.loosePredicate));
            } else {
                // For loose expression, null means always true, we can short circuit here.
                looseOrExpression = nullptr;
            }

            // For tight predicate, we give a tighter bound so that all events in the bucket
            // either all matches A or all matches B.
            if (child.tightPredicate) {
                tightOrExpression->add(std::move(child.tightPredicate));
            }
        }

        // For a loose predicate, if we are unable to generate an expression we can just treat it as
        // always true. This is because we are trying to generate a predicate that will match the
        // superset of our actual results.
        std::unique_ptr<MatchExpression> looseExpression = nullptr;
        if (looseOrExpression && looseOrExpression->numChildren() == 1) {
            looseExpression = looseOrExpression->releaseChild(0);
        } else {
            looseExpression = std::move(looseOrExpression);
        }

        // For a tight predicate, if we are unable to generate an expression we can just treat it as
        // always false or an empty OR. This is because we are trying to generate a predicate that
        // will match the subset of our actual results.
        std::unique_ptr<MatchExpression> tightExpression = nullptr;
        if (tightOrExpression->numChildren() == 1) {
            tightExpression = tightOrExpression->releaseChild(0);
        } else if (tightOrExpression->numChildren() > 1) {
            tightExpression = std::move(tightOrExpression);
        }

        return {std::move(looseExpression), std::move(tightExpression)};
    } else if (ComparisonMatchExpression::isComparisonMatchExpression(matchExpr) ||
               ComparisonMatchExpressionBase::isInternalExprComparison(matchExpr->matchType())) {
        return {
            createComparisonPredicate(checked_cast<const ComparisonMatchExpressionBase*>(matchExpr),
                                      bucketSpec,
                                      bucketMaxSpanSeconds,
                                      pExpCtx,
                                      haveComputedMetaField,
                                      includeMetaField,
                                      assumeNoMixedSchemaData,
                                      policy),
            createTightComparisonPredicate(
                checked_cast<const ComparisonMatchExpressionBase*>(matchExpr),
                bucketSpec,
                pExpCtx->collationMatchesDefault)};
    } else if (matchExpr->matchType() == MatchExpression::EXPRESSION) {
        return {
            // The loose predicate will be pushed before the unpacking which will be inspected by
            // the
            // query planner. Since the classic planner doesn't handle the $expr expression, we
            // don't
            // generate the loose predicate.
            nullptr,
            createTightExprComparisonPredicate(
                checked_cast<const ExprMatchExpression*>(matchExpr), bucketSpec, pExpCtx)};
    } else if (matchExpr->matchType() == MatchExpression::GEO) {
        auto& geoExpr = static_cast<const GeoMatchExpression*>(matchExpr)->getGeoExpression();
        if (geoExpr.getPred() == GeoExpression::WITHIN ||
            geoExpr.getPred() == GeoExpression::INTERSECT) {
            return {std::make_unique<InternalBucketGeoWithinMatchExpression>(
                        geoExpr.getGeometryPtr(), geoExpr.getField()),
                    nullptr};
        }
    } else if (matchExpr->matchType() == MatchExpression::EXISTS) {
        if (assumeNoMixedSchemaData) {
            // We know that every field that appears in an event will also appear in the min/max.
            auto result = std::make_unique<AndMatchExpression>();
            result->add(std::make_unique<ExistsMatchExpression>(StringData(
                std::string{timeseries::kControlMinFieldNamePrefix} + matchExpr->path())));
            result->add(std::make_unique<ExistsMatchExpression>(StringData(
                std::string{timeseries::kControlMaxFieldNamePrefix} + matchExpr->path())));
            return {std::move(result), nullptr};
        } else {
            // At time of writing, we only pass 'kError' when creating a partial index, and
            // we know the collection will have no mixed-schema buckets by the time the index is
            // done building.
            tassert(5916305,
                    "Can't push down {$exists: true} when the collection may have mixed-schema "
                    "buckets.",
                    policy != IneligiblePredicatePolicy::kError);
            return {};
        }
    } else if (matchExpr->matchType() == MatchExpression::MATCH_IN) {
        // {a: {$in: [X, Y]}} is equivalent to {$or: [ {a: X}, {a: Y} ]}.
        // {$in: [/a/]} is interpreted as a regex query.
        // {$in: [null]} matches any nullish value.
        const auto* inExpr = static_cast<const InMatchExpression*>(matchExpr);
        if (inExpr->hasRegex())
            return handleIneligible(
                policy, matchExpr, "can't handle $regex predicate (inside $in predicate)");
        if (inExpr->hasNull())
            return handleIneligible(
                policy, matchExpr, "can't handle {$eq: null} predicate (inside $in predicate)");

        auto result = std::make_unique<OrMatchExpression>();

        bool alwaysTrue = false;
        for (auto&& elem : inExpr->getEqualities()) {
            // If inExpr is {$in: [X, Y]} then the elems are '0: X' and '1: Y'.
            auto eq = std::make_unique<EqualityMatchExpression>(
                inExpr->path(), elem, nullptr /*annotation*/, inExpr->getCollator());
            auto child = createComparisonPredicate(eq.get(),
                                                   bucketSpec,
                                                   bucketMaxSpanSeconds,
                                                   pExpCtx,
                                                   haveComputedMetaField,
                                                   includeMetaField,
                                                   assumeNoMixedSchemaData,
                                                   policy);

            // As with OR, only add the child if it has been succesfully translated, otherwise the
            // $in cannot be correctly mapped to bucket level fields and we should return nullptr.
            if (child) {
                result->add(std::move(child));
            } else {
                alwaysTrue = true;
                if (policy == IneligiblePredicatePolicy::kIgnore)
                    break;
            }
        }
        if (alwaysTrue)
            return {};

        // As above, no special case for an empty IN: returning nullptr would be incorrect because
        // it means 'always-true', here.
        return {std::move(result), nullptr};
    }
    return handleIneligible(policy, matchExpr, "can't handle this predicate");
}

std::pair<bool, BSONObj> BucketSpec::pushdownPredicate(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const TimeseriesOptions& tsOptions,
    const BSONObj& predicate,
    bool haveComputedMetaField,
    bool includeMetaField,
    bool assumeNoMixedSchemaData,
    IneligiblePredicatePolicy policy) {
    auto [metaOnlyPred, bucketMetricPred, residualPred] =
        getPushdownPredicates(expCtx,
                              tsOptions,
                              predicate,
                              haveComputedMetaField,
                              includeMetaField,
                              assumeNoMixedSchemaData,
                              policy);
    BSONObjBuilder result;
    if (metaOnlyPred)
        metaOnlyPred->serialize(&result, {});
    if (bucketMetricPred)
        bucketMetricPred->serialize(&result, {});
    return std::make_pair(bucketMetricPred.get(), result.obj());
}

BucketSpec::SplitPredicates BucketSpec::getPushdownPredicates(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const TimeseriesOptions& tsOptions,
    const BSONObj& predicate,
    bool haveComputedMetaField,
    bool includeMetaField,
    bool assumeNoMixedSchemaData,
    IneligiblePredicatePolicy policy) {

    auto allowedFeatures = MatchExpressionParser::kDefaultSpecialFeatures;
    auto matchExpr = uassertStatusOK(
        MatchExpressionParser::parse(predicate, expCtx, ExtensionsCallbackNoop(), allowedFeatures));

    auto metaField = haveComputedMetaField ? boost::none : tsOptions.getMetaField();
    auto [metaOnlyPred, residualPred] = [&] {
        if (!metaField) {
            // If there's no metadata field, then none of the predicates are metadata-only
            // predicates.
            return std::make_pair(std::unique_ptr<MatchExpression>(nullptr), std::move(matchExpr));
        }

        return expression::splitMatchExpressionBy(
            std::move(matchExpr),
            {metaField->toString()},
            {{metaField->toString(), timeseries::kBucketMetaFieldName.toString()}},
            expression::isOnlyDependentOn);
    }();

    std::unique_ptr<MatchExpression> bucketMetricPred = residualPred
        ? createPredicatesOnBucketLevelField(
              residualPred.get(),
              BucketSpec{
                  tsOptions.getTimeField().toString(),
                  metaField.map([](StringData s) { return s.toString(); }),
                  // Since we are operating on a collection, not a query-result,
                  // there are no inclusion/exclusion projections we need to apply
                  // to the buckets before unpacking. So we can use default values for the rest of
                  // the arguments.
              },
              *tsOptions.getBucketMaxSpanSeconds(),
              expCtx,
              haveComputedMetaField,
              includeMetaField,
              assumeNoMixedSchemaData,
              policy)
              .loosePredicate
        : nullptr;

    return {std::move(metaOnlyPred), std::move(bucketMetricPred), std::move(residualPred)};
}

BucketSpec::BucketSpec(const std::string& timeField,
                       const boost::optional<std::string>& metaField,
                       const std::set<std::string>& fields,
                       Behavior behavior,
                       const std::set<std::string>& computedProjections,
                       bool usesExtendedRange)
    : _fieldSet(fields),
      _behavior(behavior),
      _computedMetaProjFields(computedProjections),
      _timeField(timeField),
      _timeFieldHashed(FieldNameHasher().hashedFieldName(_timeField)),
      _metaField(metaField),
      _usesExtendedRange(usesExtendedRange) {
    if (_metaField) {
        _metaFieldHashed = FieldNameHasher().hashedFieldName(*_metaField);
    }
}

BucketSpec::BucketSpec(const BucketSpec& other)
    : _fieldSet(other._fieldSet),
      _behavior(other._behavior),
      _computedMetaProjFields(other._computedMetaProjFields),
      _timeField(other._timeField),
      _timeFieldHashed(HashedFieldName{_timeField, other._timeFieldHashed->hash()}),
      _metaField(other._metaField),
      _usesExtendedRange(other._usesExtendedRange) {
    if (_metaField) {
        _metaFieldHashed = HashedFieldName{*_metaField, other._metaFieldHashed->hash()};
    }
}

BucketSpec::BucketSpec(BucketSpec&& other)
    : _fieldSet(std::move(other._fieldSet)),
      _behavior(other._behavior),
      _computedMetaProjFields(std::move(other._computedMetaProjFields)),
      _timeField(std::move(other._timeField)),
      _timeFieldHashed(HashedFieldName{_timeField, other._timeFieldHashed->hash()}),
      _metaField(std::move(other._metaField)),
      _usesExtendedRange(other._usesExtendedRange) {
    if (_metaField) {
        _metaFieldHashed = HashedFieldName{*_metaField, other._metaFieldHashed->hash()};
    }
}

BucketSpec::BucketSpec(const TimeseriesOptions& tsOptions)
    : BucketSpec(tsOptions.getTimeField().toString(),
                 tsOptions.getMetaField()
                     ? boost::optional<string>(tsOptions.getMetaField()->toString())
                     : boost::none) {}

BucketSpec& BucketSpec::operator=(const BucketSpec& other) {
    if (&other != this) {
        _fieldSet = other._fieldSet;
        _behavior = other._behavior;
        _computedMetaProjFields = other._computedMetaProjFields;
        _timeField = other._timeField;
        _timeFieldHashed = HashedFieldName{_timeField, other._timeFieldHashed->hash()};
        _metaField = other._metaField;
        if (_metaField) {
            _metaFieldHashed = HashedFieldName{*_metaField, other._metaFieldHashed->hash()};
        }
        _usesExtendedRange = other._usesExtendedRange;
    }
    return *this;
}

void BucketSpec::setTimeField(std::string&& name) {
    _timeField = std::move(name);
    _timeFieldHashed = FieldNameHasher().hashedFieldName(_timeField);
}

const std::string& BucketSpec::timeField() const {
    return _timeField;
}

HashedFieldName BucketSpec::timeFieldHashed() const {
    invariant(_timeFieldHashed->key().rawData() == _timeField.data());
    invariant(_timeFieldHashed->key() == _timeField);
    return *_timeFieldHashed;
}

void BucketSpec::setMetaField(boost::optional<std::string>&& name) {
    _metaField = std::move(name);
    if (_metaField) {
        _metaFieldHashed = FieldNameHasher().hashedFieldName(*_metaField);
    } else {
        _metaFieldHashed = boost::none;
    }
}

const boost::optional<std::string>& BucketSpec::metaField() const {
    return _metaField;
}

boost::optional<HashedFieldName> BucketSpec::metaFieldHashed() const {
    return _metaFieldHashed;
}
}  // namespace mongo
