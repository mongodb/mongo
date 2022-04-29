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

#include "mongo/db/exec/bucket_unpacker.h"

#include <algorithm>

#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"

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
    // Make an ObjectId cooresponding to a date value adjusted by the max bucket value for the
    // time series view that this query operates on. This predicate can be used in a comparison
    // to gauge a max value for a given bucket, rather than a min value.
    auto makeMaxAdjustedDateOID = [&](auto&& date, auto&& maxOrMin) {
        // Ensure we don't underflow.
        if (date.toDurationSinceEpoch() >= Seconds{bucketMaxSpanSeconds})
            // Subtract max bucket range.
            return makeDateOID(date - Seconds{bucketMaxSpanSeconds}, maxOrMin);
        else
            // Since we're out of range, just make a predicate that is true for all date types.
            return makeDateOID(Date_t::min(), OIDInit::min);
    };
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

std::unique_ptr<MatchExpression> handleIneligible(IneligiblePredicatePolicy policy,
                                                  const MatchExpression* matchExpr,
                                                  StringData message) {
    switch (policy) {
        case IneligiblePredicatePolicy::kError:
            uasserted(
                5916301,
                "Error translating non-metadata time-series predicate to operate on buckets: " +
                    message + ": " + matchExpr->serialize().toString());
        case IneligiblePredicatePolicy::kIgnore:
            return nullptr;
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

std::unique_ptr<MatchExpression> createComparisonPredicate(
    const ComparisonMatchExpressionBase* matchExpr,
    const BucketSpec& bucketSpec,
    int bucketMaxSpanSeconds,
    ExpressionContext::CollationMatchesDefault collationMatchesDefault,
    boost::intrusive_ptr<ExpressionContext> pExpCtx,
    bool haveComputedMetaField,
    bool includeMetaField,
    bool assumeNoMixedSchemaData,
    IneligiblePredicatePolicy policy) {
    using namespace timeseries;
    const auto matchExprPath = matchExpr->path();
    const auto matchExprData = matchExpr->getData();

    // The control field's min and max are chosen using a field-order insensitive comparator, while
    // MatchExpressions use a comparator that treats field-order as significant. Because of this we
    // will not perform this optimization on queries with operands of compound types.
    if (matchExprData.type() == BSONType::Object || matchExprData.type() == BSONType::Array)
        return handleIneligible(policy, matchExpr, "operand can't be an object or array"_sd);

    // MatchExpressions have special comparison semantics regarding null, in that {$eq: null} will
    // match all documents where the field is either null or missing. Because this is different
    // from both the comparison semantics that InternalExprComparison expressions and the control's
    // min and max fields use, we will not perform this optimization on queries with null operands.
    if (matchExprData.type() == BSONType::jstNULL)
        return handleIneligible(policy, matchExpr, "can't handle {$eq: null}"_sd);

    // The control field's min and max are chosen based on the collation of the collection. If the
    // query's collation does not match the collection's collation and the query operand is a
    // string or compound type (skipped above) we will not perform this optimization.
    if (collationMatchesDefault == ExpressionContext::CollationMatchesDefault::kNo &&
        matchExprData.type() == BSONType::String) {
        return handleIneligible(
            policy, matchExpr, "can't handle string comparison with a non-default collation"_sd);
    }

    // We must avoid mapping predicates on the meta field onto the control field. These should be
    // mapped to the meta field instead.
    //
    // You might think these were handled earlier, by splitting the match expression into a
    // metadata-only part, and measurement/time-only part. However, splitting a $match into two
    // sequential $matches only works when splitting a conjunction. A predicate like
    // {$or: [ {a: 5}, {meta.b: 5} ]} cannot be split, and can't be metadata-only, so we have to
    // handle it here.
    if (bucketSpec.metaField() &&
        (matchExprPath == bucketSpec.metaField().get() ||
         expression::isPathPrefixOf(bucketSpec.metaField().get(), matchExprPath))) {

        if (haveComputedMetaField)
            return handleIneligible(policy, matchExpr, "can't handle a computed meta field");

        if (!includeMetaField)
            return handleIneligible(policy, matchExpr, "cannot handle an excluded meta field");

        auto result = matchExpr->shallowClone();
        expression::applyRenamesToExpression(
            result.get(),
            {{bucketSpec.metaField().get(), timeseries::kBucketMetaFieldName.toString()}});
        return result;
    }

    // We must avoid mapping predicates on fields computed via $addFields or a computed $project.
    if (bucketSpec.fieldIsComputed(matchExprPath.toString())) {
        return handleIneligible(policy, matchExpr, "can't handle a computed field");
    }

    const auto isTimeField = (matchExprPath == bucketSpec.timeField());
    if (isTimeField && matchExprData.type() != BSONType::Date) {
        // Users are not allowed to insert non-date measurements into time field. So this query
        // would not match anything. We do not need to optimize for this case.
        return handleIneligible(
            policy,
            matchExpr,
            "This predicate will never be true, because the time field always contains a Date");
    }

    BSONObj minTime;
    BSONObj maxTime;
    if (isTimeField) {
        auto timeField = matchExprData.Date();
        minTime = BSON("" << timeField - Seconds(bucketMaxSpanSeconds));
        maxTime = BSON("" << timeField + Seconds(bucketMaxSpanSeconds));
    }

    auto minPath = std::string{kControlMinFieldNamePrefix} + matchExprPath;
    auto maxPath = std::string{kControlMaxFieldNamePrefix} + matchExprPath;

    switch (matchExpr->matchType()) {
        case MatchExpression::EQ:
        case MatchExpression::INTERNAL_EXPR_EQ:
            // For $eq, make both a $lte against 'control.min' and a $gte predicate against
            // 'control.max'.
            //
            // If the comparison is against the 'time' field, include a predicate against the _id
            // field which is converted to the maximum for the corresponding range of ObjectIds and
            // is adjusted by the max range for a bucket to approximate the max bucket value given
            // the min. Also include a predicate against the _id field which is converted to the
            // minimum for the range of ObjectIds corresponding to the given date. In
            // addition, we include a {'control.min' : {$gte: 'time - bucketMaxSpanSeconds'}} and
            // a {'control.max' : {$lte: 'time + bucketMaxSpanSeconds'}} predicate which will be
            // helpful in reducing bounds for index scans on 'time' field and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$eq: [...]}} that can be rewritten to use $_internalExprEq.
            return isTimeField
                ? makePredicate(
                      MatchExprPredicate<InternalExprLTEMatchExpression>(minPath, matchExprData),
                      MatchExprPredicate<InternalExprGTEMatchExpression>(minPath,
                                                                         minTime.firstElement()),
                      MatchExprPredicate<InternalExprGTEMatchExpression>(maxPath, matchExprData),
                      MatchExprPredicate<InternalExprLTEMatchExpression>(maxPath,
                                                                         maxTime.firstElement()),
                      MatchExprPredicate<LTEMatchExpression, Value>(
                          kBucketIdFieldName,
                          constructObjectIdValue<LTEMatchExpression>(matchExprData,
                                                                     bucketMaxSpanSeconds)),
                      MatchExprPredicate<GTEMatchExpression, Value>(
                          kBucketIdFieldName,
                          constructObjectIdValue<GTEMatchExpression>(matchExprData,
                                                                     bucketMaxSpanSeconds)))
                : makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                      makePredicate(MatchExprPredicate<InternalExprLTEMatchExpression>(
                                        minPath, matchExprData),
                                    MatchExprPredicate<InternalExprGTEMatchExpression>(
                                        maxPath, matchExprData)),
                      createTypeEqualityPredicate(
                          pExpCtx, matchExprPath, assumeNoMixedSchemaData)));

        case MatchExpression::GT:
        case MatchExpression::INTERNAL_EXPR_GT:
            // For $gt, make a $gt predicate against 'control.max'. In addition, if the comparison
            // is against the 'time' field, include a predicate against the _id field which is
            // converted to the maximum for the corresponding range of ObjectIds and is adjusted
            // by the max range for a bucket to approximate the max bucket value given the min. In
            // addition, we include a {'control.min' : {$gt: 'time - bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time' field
            // and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$gt: [...]}} that can be rewritten to use $_internalExprGt.
            return isTimeField
                ? makePredicate(
                      MatchExprPredicate<InternalExprGTMatchExpression>(maxPath, matchExprData),
                      MatchExprPredicate<InternalExprGTMatchExpression>(minPath,
                                                                        minTime.firstElement()),
                      MatchExprPredicate<GTMatchExpression, Value>(
                          kBucketIdFieldName,
                          constructObjectIdValue<GTMatchExpression>(matchExprData,
                                                                    bucketMaxSpanSeconds)))
                : makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                      std::make_unique<InternalExprGTMatchExpression>(maxPath, matchExprData),
                      createTypeEqualityPredicate(
                          pExpCtx, matchExprPath, assumeNoMixedSchemaData)));

        case MatchExpression::GTE:
        case MatchExpression::INTERNAL_EXPR_GTE:
            // For $gte, make a $gte predicate against 'control.max'. In addition, if the comparison
            // is against the 'time' field, include a predicate against the _id field which is
            // converted to the minimum for the corresponding range of ObjectIds and is adjusted
            // by the max range for a bucket to approximate the max bucket value given the min. In
            // addition, we include a {'control.min' : {$gte: 'time - bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time' field
            // and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$gte: [...]}} that can be rewritten to use $_internalExprGte.
            return isTimeField
                ? makePredicate(
                      MatchExprPredicate<InternalExprGTEMatchExpression>(maxPath, matchExprData),
                      MatchExprPredicate<InternalExprGTEMatchExpression>(minPath,
                                                                         minTime.firstElement()),
                      MatchExprPredicate<GTEMatchExpression, Value>(
                          kBucketIdFieldName,
                          constructObjectIdValue<GTEMatchExpression>(matchExprData,
                                                                     bucketMaxSpanSeconds)))
                : makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                      std::make_unique<InternalExprGTEMatchExpression>(maxPath, matchExprData),
                      createTypeEqualityPredicate(
                          pExpCtx, matchExprPath, assumeNoMixedSchemaData)));

        case MatchExpression::LT:
        case MatchExpression::INTERNAL_EXPR_LT:
            // For $lt, make a $lt predicate against 'control.min'. In addition, if the comparison
            // is against the 'time' field, include a predicate against the _id field which is
            // converted to the minimum for the corresponding range of ObjectIds. In
            // addition, we include a {'control.max' : {$lt: 'time + bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time' field
            // and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$lt: [...]}} that can be rewritten to use $_internalExprLt.
            return isTimeField
                ? makePredicate(
                      MatchExprPredicate<InternalExprLTMatchExpression>(minPath, matchExprData),
                      MatchExprPredicate<InternalExprLTMatchExpression>(maxPath,
                                                                        maxTime.firstElement()),
                      MatchExprPredicate<LTMatchExpression, Value>(
                          kBucketIdFieldName,
                          constructObjectIdValue<LTMatchExpression>(matchExprData,
                                                                    bucketMaxSpanSeconds)))
                : makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                      std::make_unique<InternalExprLTMatchExpression>(minPath, matchExprData),
                      createTypeEqualityPredicate(
                          pExpCtx, matchExprPath, assumeNoMixedSchemaData)));

        case MatchExpression::LTE:
        case MatchExpression::INTERNAL_EXPR_LTE:
            // For $lte, make a $lte predicate against 'control.min'. In addition, if the comparison
            // is against the 'time' field, include a predicate against the _id field which is
            // converted to the maximum for the corresponding range of ObjectIds. In
            // addition, we include a {'control.max' : {$lte: 'time + bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time' field
            // and routing on mongos.
            //
            // The same procedure applies to aggregation expressions of the form
            // {$expr: {$lte: [...]}} that can be rewritten to use $_internalExprLte.
            return isTimeField
                ? makePredicate(
                      MatchExprPredicate<InternalExprLTEMatchExpression>(minPath, matchExprData),
                      MatchExprPredicate<InternalExprLTEMatchExpression>(maxPath,
                                                                         maxTime.firstElement()),
                      MatchExprPredicate<LTEMatchExpression, Value>(
                          kBucketIdFieldName,
                          constructObjectIdValue<LTEMatchExpression>(matchExprData,
                                                                     bucketMaxSpanSeconds)))
                : makeOr(makeVector<std::unique_ptr<MatchExpression>>(
                      std::make_unique<InternalExprLTEMatchExpression>(minPath, matchExprData),
                      createTypeEqualityPredicate(
                          pExpCtx, matchExprPath, assumeNoMixedSchemaData)));

        default:
            MONGO_UNREACHABLE_TASSERT(5348302);
    }

    MONGO_UNREACHABLE_TASSERT(5348303);
}

}  // namespace

std::unique_ptr<MatchExpression> BucketSpec::createPredicatesOnBucketLevelField(
    const MatchExpression* matchExpr,
    const BucketSpec& bucketSpec,
    int bucketMaxSpanSeconds,
    ExpressionContext::CollationMatchesDefault collationMatchesDefault,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    bool haveComputedMetaField,
    bool includeMetaField,
    bool assumeNoMixedSchemaData,
    IneligiblePredicatePolicy policy) {

    tassert(5916304, "BucketSpec::createPredicatesOnBucketLevelField nullptr", matchExpr);

    if (matchExpr->matchType() == MatchExpression::AND) {
        auto nextAnd = static_cast<const AndMatchExpression*>(matchExpr);
        auto andMatchExpr = std::make_unique<AndMatchExpression>();

        for (size_t i = 0; i < nextAnd->numChildren(); i++) {
            if (auto child = createPredicatesOnBucketLevelField(nextAnd->getChild(i),
                                                                bucketSpec,
                                                                bucketMaxSpanSeconds,
                                                                collationMatchesDefault,
                                                                pExpCtx,
                                                                haveComputedMetaField,
                                                                includeMetaField,
                                                                assumeNoMixedSchemaData,
                                                                policy)) {
                andMatchExpr->add(std::move(child));
            }
        }
        if (andMatchExpr->numChildren() == 1) {
            return andMatchExpr->releaseChild(0);
        }
        if (andMatchExpr->numChildren() > 0) {
            return andMatchExpr;
        }

        // No error message here: an empty AND is valid.
        return nullptr;
    } else if (matchExpr->matchType() == MatchExpression::OR) {
        // Given {$or: [A, B]}, suppose A, B can be pushed down as A', B'.
        // If an event matches {$or: [A, B]} then either:
        //     - it matches A, which means any bucket containing it matches A'
        //     - it matches B, which means any bucket containing it matches B'
        // So {$or: [A', B']} will capture all the buckets we need to satisfy {$or: [A, B]}.
        auto nextOr = static_cast<const OrMatchExpression*>(matchExpr);
        auto result = std::make_unique<OrMatchExpression>();

        bool alwaysTrue = false;
        for (size_t i = 0; i < nextOr->numChildren(); i++) {
            auto child = createPredicatesOnBucketLevelField(nextOr->getChild(i),
                                                            bucketSpec,
                                                            bucketMaxSpanSeconds,
                                                            collationMatchesDefault,
                                                            pExpCtx,
                                                            haveComputedMetaField,
                                                            includeMetaField,
                                                            assumeNoMixedSchemaData,
                                                            policy);
            if (child) {
                result->add(std::move(child));
            } else {
                // Since this argument is always-true, the entire OR is always-true.
                alwaysTrue = true;

                // Only short circuit if we're uninterested in reporting errors.
                if (policy == IneligiblePredicatePolicy::kIgnore)
                    break;
            }
        }
        if (alwaysTrue)
            return nullptr;

        // No special case for an empty OR: returning nullptr would be incorrect because it
        // means 'always-true', here.
        return result;
    } else if (ComparisonMatchExpression::isComparisonMatchExpression(matchExpr) ||
               ComparisonMatchExpressionBase::isInternalExprComparison(matchExpr->matchType())) {
        return createComparisonPredicate(
            checked_cast<const ComparisonMatchExpressionBase*>(matchExpr),
            bucketSpec,
            bucketMaxSpanSeconds,
            collationMatchesDefault,
            pExpCtx,
            haveComputedMetaField,
            includeMetaField,
            assumeNoMixedSchemaData,
            policy);
    } else if (matchExpr->matchType() == MatchExpression::GEO) {
        auto& geoExpr = static_cast<const GeoMatchExpression*>(matchExpr)->getGeoExpression();
        if (geoExpr.getPred() == GeoExpression::WITHIN ||
            geoExpr.getPred() == GeoExpression::INTERSECT) {
            return std::make_unique<InternalBucketGeoWithinMatchExpression>(
                geoExpr.getGeometryPtr(), geoExpr.getField());
        }
    } else if (matchExpr->matchType() == MatchExpression::EXISTS) {
        if (assumeNoMixedSchemaData) {
            // We know that every field that appears in an event will also appear in the min/max.
            auto result = std::make_unique<AndMatchExpression>();
            result->add(std::make_unique<ExistsMatchExpression>(
                std::string{timeseries::kControlMinFieldNamePrefix} + matchExpr->path()));
            result->add(std::make_unique<ExistsMatchExpression>(
                std::string{timeseries::kControlMaxFieldNamePrefix} + matchExpr->path()));
            return result;
        } else {
            // At time of writing, we only pass 'kError' when creating a partial index, and
            // we know the collection will have no mixed-schema buckets by the time the index is
            // done building.
            tassert(5916305,
                    "Can't push down {$exists: true} when the collection may have mixed-schema "
                    "buckets.",
                    policy != IneligiblePredicatePolicy::kError);
            return nullptr;
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
                                                   collationMatchesDefault,
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
            return nullptr;

        // As above, no special case for an empty IN: returning nullptr would be incorrect because
        // it means 'always-true', here.
        return result;
    }
    return handleIneligible(policy, matchExpr, "can't handle this predicate");
}

BSONObj BucketSpec::pushdownPredicate(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const TimeseriesOptions& tsOptions,
    ExpressionContext::CollationMatchesDefault collationMatchesDefault,
    const BSONObj& predicate,
    bool haveComputedMetaField,
    bool includeMetaField,
    bool assumeNoMixedSchemaData,
    IneligiblePredicatePolicy policy) {

    auto allowedFeatures = MatchExpressionParser::kDefaultSpecialFeatures;
    auto matchExpr = uassertStatusOK(
        MatchExpressionParser::parse(predicate, expCtx, ExtensionsCallbackNoop(), allowedFeatures));

    auto metaField = haveComputedMetaField ? boost::none : tsOptions.getMetaField();
    auto [metaOnlyPredicate, metricPredicate] = [&] {
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

    int maxSpanSeconds = tsOptions.getBucketMaxSpanSeconds()
        ? *tsOptions.getBucketMaxSpanSeconds()
        : timeseries::getMaxSpanSecondsFromGranularity(tsOptions.getGranularity());

    std::unique_ptr<MatchExpression> bucketMetricPredicate = metricPredicate
        ? createPredicatesOnBucketLevelField(
              metricPredicate.get(),
              BucketSpec{
                  tsOptions.getTimeField().toString(),
                  metaField.map([](StringData s) { return s.toString(); }),
                  // Since we are operating on a collection, not a query-result, there are no
                  // inclusion/exclusion projections we need to apply to the buckets before
                  // unpacking.
                  {},
                  // And there are no computed projections.
                  {},
              },
              maxSpanSeconds,
              collationMatchesDefault,
              expCtx,
              haveComputedMetaField,
              includeMetaField,
              assumeNoMixedSchemaData,
              policy)
        : nullptr;

    BSONObjBuilder result;
    if (metaOnlyPredicate)
        metaOnlyPredicate->serialize(&result);
    if (bucketMetricPredicate)
        bucketMetricPredicate->serialize(&result);
    return result.obj();
}

class BucketUnpacker::UnpackingImpl {
public:
    UnpackingImpl() = default;
    virtual ~UnpackingImpl() = default;

    virtual void addField(const BSONElement& field) = 0;
    virtual int measurementCount(const BSONElement& timeField) const = 0;
    virtual bool getNext(MutableDocument& measurement,
                         const BucketSpec& spec,
                         const Value& metaValue,
                         bool includeTimeField,
                         bool includeMetaField) = 0;
    virtual void extractSingleMeasurement(MutableDocument& measurement,
                                          int j,
                                          const BucketSpec& spec,
                                          const std::set<std::string>& unpackFieldsToIncludeExclude,
                                          BucketUnpacker::Behavior behavior,
                                          const BSONObj& bucket,
                                          const Value& metaValue,
                                          bool includeTimeField,
                                          bool includeMetaField) = 0;

    // Provides an upper bound on the number of fields in each measurement.
    virtual std::size_t numberOfFields() = 0;

protected:
    // Data field count is variable, but time and metadata are fixed.
    constexpr static std::size_t kFixedFieldNumber = 2;
};

namespace {


// Unpacker for V1 uncompressed buckets
class BucketUnpackerV1 : public BucketUnpacker::UnpackingImpl {
public:
    // A table that is useful for interpolations between the number of measurements in a bucket and
    // the byte size of a bucket's data section timestamp column. Each table entry is a pair (b_i,
    // S_i), where b_i is the number of measurements in the bucket and S_i is the byte size of the
    // timestamp BSONObj. The table is bounded by 16 MB (2 << 23 bytes) where the table entries are
    // pairs of b_i and S_i for the lower bounds of the row key digit intervals [0, 9], [10, 99],
    // [100, 999], [1000, 9999] and so on. The last entry in the table, S7, is the first entry to
    // exceed the server BSON object limit of 16 MB.
    static constexpr std::array<std::pair<int32_t, int32_t>, 8> kTimestampObjSizeTable{
        {{0, BSONObj::kMinBSONLength},
         {10, 115},
         {100, 1195},
         {1000, 12895},
         {10000, 138895},
         {100000, 1488895},
         {1000000, 15888895},
         {10000000, 168888895}}};

    static int computeElementCountFromTimestampObjSize(int targetTimestampObjSize);

    BucketUnpackerV1(const BSONElement& timeField);

    void addField(const BSONElement& field) override;
    int measurementCount(const BSONElement& timeField) const override;
    bool getNext(MutableDocument& measurement,
                 const BucketSpec& spec,
                 const Value& metaValue,
                 bool includeTimeField,
                 bool includeMetaField) override;
    void extractSingleMeasurement(MutableDocument& measurement,
                                  int j,
                                  const BucketSpec& spec,
                                  const std::set<std::string>& unpackFieldsToIncludeExclude,
                                  BucketUnpacker::Behavior behavior,
                                  const BSONObj& bucket,
                                  const Value& metaValue,
                                  bool includeTimeField,
                                  bool includeMetaField) override;
    std::size_t numberOfFields() override;

private:
    // Iterates the timestamp section of the bucket to drive the unpacking iteration.
    BSONObjIterator _timeFieldIter;

    // Iterators used to unpack the columns of the above bucket that are populated during the reset
    // phase according to the provided 'Behavior' and 'BucketSpec'.
    std::vector<std::pair<std::string, BSONObjIterator>> _fieldIters;
};

// Calculates the number of measurements in a bucket given the 'targetTimestampObjSize' using the
// 'BucketUnpacker::kTimestampObjSizeTable' table. If the 'targetTimestampObjSize' hits a record in
// the table, this helper returns the measurement count corresponding to the table record.
// Otherwise, the 'targetTimestampObjSize' is used to probe the table for the smallest {b_i, S_i}
// pair such that 'targetTimestampObjSize' < S_i. Once the interval is found, the upper bound of the
// pair for the interval is computed and then linear interpolation is used to compute the
// measurement count corresponding to the 'targetTimestampObjSize' provided.
int BucketUnpackerV1::computeElementCountFromTimestampObjSize(int targetTimestampObjSize) {
    auto currentInterval =
        std::find_if(std::begin(BucketUnpackerV1::kTimestampObjSizeTable),
                     std::end(BucketUnpackerV1::kTimestampObjSizeTable),
                     [&](const auto& entry) { return targetTimestampObjSize <= entry.second; });

    if (currentInterval->second == targetTimestampObjSize) {
        return currentInterval->first;
    }
    // This points to the first interval larger than the target 'targetTimestampObjSize', the actual
    // interval that will cover the object size is the interval before the current one.
    tassert(5422104,
            "currentInterval should not point to the first table entry",
            currentInterval > BucketUnpackerV1::kTimestampObjSizeTable.begin());
    --currentInterval;

    auto nDigitsInRowKey = 1 + (currentInterval - BucketUnpackerV1::kTimestampObjSizeTable.begin());

    return currentInterval->first +
        ((targetTimestampObjSize - currentInterval->second) / (10 + nDigitsInRowKey));
}

BucketUnpackerV1::BucketUnpackerV1(const BSONElement& timeField)
    : _timeFieldIter(BSONObjIterator{timeField.Obj()}) {}

void BucketUnpackerV1::addField(const BSONElement& field) {
    _fieldIters.emplace_back(field.fieldNameStringData(), BSONObjIterator{field.Obj()});
}

int BucketUnpackerV1::measurementCount(const BSONElement& timeField) const {
    return computeElementCountFromTimestampObjSize(timeField.objsize());
}

bool BucketUnpackerV1::getNext(MutableDocument& measurement,
                               const BucketSpec& spec,
                               const Value& metaValue,
                               bool includeTimeField,
                               bool includeMetaField) {
    auto&& timeElem = _timeFieldIter.next();
    if (includeTimeField) {
        measurement.addField(spec.timeFieldHashed(), Value{timeElem});
    }

    // Includes metaField when we're instructed to do so and metaField value exists.
    if (includeMetaField && !metaValue.missing()) {
        measurement.addField(*spec.metaFieldHashed(), metaValue);
    }

    const auto& currentIdx = timeElem.fieldNameStringData();
    for (auto&& [colName, colIter] : _fieldIters) {
        if (auto&& elem = *colIter; colIter.more() && elem.fieldNameStringData() == currentIdx) {
            measurement.addField(colName, Value{elem});
            colIter.advance(elem);
        }
    }

    return _timeFieldIter.more();
}

void BucketUnpackerV1::extractSingleMeasurement(
    MutableDocument& measurement,
    int j,
    const BucketSpec& spec,
    const std::set<std::string>& unpackFieldsToIncludeExclude,
    BucketUnpacker::Behavior behavior,
    const BSONObj& bucket,
    const Value& metaValue,
    bool includeTimeField,
    bool includeMetaField) {
    auto rowKey = std::to_string(j);
    auto targetIdx = StringData{rowKey};
    auto&& dataRegion = bucket.getField(timeseries::kBucketDataFieldName).Obj();

    if (includeMetaField && !metaValue.missing()) {
        measurement.addField(*spec.metaFieldHashed(), metaValue);
    }

    for (auto&& dataElem : dataRegion) {
        const auto& colName = dataElem.fieldNameStringData();
        if (!determineIncludeField(colName, behavior, unpackFieldsToIncludeExclude)) {
            continue;
        }
        auto value = dataElem[targetIdx];
        if (value) {
            measurement.addField(dataElem.fieldNameStringData(), Value{value});
        }
    }
}

std::size_t BucketUnpackerV1::numberOfFields() {
    // The data fields are tracked by _fieldIters, but we need to account also for the time field
    // and possibly the meta field.
    return kFixedFieldNumber + _fieldIters.size();
}

// Unpacker for V2 compressed buckets
class BucketUnpackerV2 : public BucketUnpacker::UnpackingImpl {
public:
    BucketUnpackerV2(const BSONElement& timeField, int elementCount);

    void addField(const BSONElement& field) override;
    int measurementCount(const BSONElement& timeField) const override;
    bool getNext(MutableDocument& measurement,
                 const BucketSpec& spec,
                 const Value& metaValue,
                 bool includeTimeField,
                 bool includeMetaField) override;
    void extractSingleMeasurement(MutableDocument& measurement,
                                  int j,
                                  const BucketSpec& spec,
                                  const std::set<std::string>& unpackFieldsToIncludeExclude,
                                  BucketUnpacker::Behavior behavior,
                                  const BSONObj& bucket,
                                  const Value& metaValue,
                                  bool includeTimeField,
                                  bool includeMetaField) override;
    std::size_t numberOfFields() override;

private:
    struct ColumnStore {
        ColumnStore(BSONElement elem)
            : column(elem),
              it(column.begin()),
              end(column.end()),
              hashedName(FieldNameHasher{}(column.name())) {}
        ColumnStore(ColumnStore&& other)
            : column(std::move(other.column)),
              it(other.it.moveTo(column)),
              end(other.end),
              hashedName(other.hashedName) {}

        BSONColumn column;
        BSONColumn::Iterator it;
        BSONColumn::Iterator end;
        size_t hashedName;
    };

    // Iterates the timestamp section of the bucket to drive the unpacking iteration.
    ColumnStore _timeColumn;

    // Iterators used to unpack the columns of the above bucket that are populated during the reset
    // phase according to the provided 'Behavior' and 'BucketSpec'.
    std::vector<ColumnStore> _fieldColumns;

    // Element count
    int _elementCount;
};

BucketUnpackerV2::BucketUnpackerV2(const BSONElement& timeField, int elementCount)
    : _timeColumn(timeField), _elementCount(elementCount) {
    if (_elementCount == -1) {
        _elementCount = _timeColumn.column.size();
    }
}

void BucketUnpackerV2::addField(const BSONElement& field) {
    _fieldColumns.emplace_back(field);
}

int BucketUnpackerV2::measurementCount(const BSONElement& timeField) const {
    return _elementCount;
}

bool BucketUnpackerV2::getNext(MutableDocument& measurement,
                               const BucketSpec& spec,
                               const Value& metaValue,
                               bool includeTimeField,
                               bool includeMetaField) {
    // Get element and increment iterator
    const auto& timeElem = *_timeColumn.it;
    if (includeTimeField) {
        measurement.addField(spec.timeFieldHashed(), Value{timeElem});
    }
    ++_timeColumn.it;

    // Includes metaField when we're instructed to do so and metaField value exists.
    if (includeMetaField && !metaValue.missing()) {
        measurement.addField(*spec.metaFieldHashed(), metaValue);
    }

    for (auto& fieldColumn : _fieldColumns) {
        uassert(6067601,
                "Bucket unexpectedly contained fewer values than count",
                fieldColumn.it != fieldColumn.end);
        const BSONElement& elem = *fieldColumn.it;
        // EOO represents missing field
        if (!elem.eoo()) {
            measurement.addField(HashedFieldName{fieldColumn.column.name(), fieldColumn.hashedName},
                                 Value{elem});
        }
        ++fieldColumn.it;
    }

    return _timeColumn.it != _timeColumn.end;
}

void BucketUnpackerV2::extractSingleMeasurement(
    MutableDocument& measurement,
    int j,
    const BucketSpec& spec,
    const std::set<std::string>& unpackFieldsToIncludeExclude,
    BucketUnpacker::Behavior behavior,
    const BSONObj& bucket,
    const Value& metaValue,
    bool includeTimeField,
    bool includeMetaField) {
    if (includeTimeField) {
        auto val = _timeColumn.column[j];
        uassert(
            6067500, "Bucket unexpectedly contained fewer values than count", val && !val->eoo());
        measurement.addField(spec.timeFieldHashed(), Value{*val});
    }

    if (includeMetaField && !metaValue.missing()) {
        measurement.addField(*spec.metaFieldHashed(), metaValue);
    }

    if (includeTimeField) {
        for (auto& fieldColumn : _fieldColumns) {
            auto val = fieldColumn.column[j];
            uassert(6067600, "Bucket unexpectedly contained fewer values than count", val);
            measurement.addField(HashedFieldName{fieldColumn.column.name(), fieldColumn.hashedName},
                                 Value{*val});
        }
    }
}

std::size_t BucketUnpackerV2::numberOfFields() {
    // The data fields are tracked by _fieldColumns, but we need to account also for the time field
    // and possibly the meta field.
    return kFixedFieldNumber + _fieldColumns.size();
}
}  // namespace

BucketSpec::BucketSpec(const std::string& timeField,
                       const boost::optional<std::string>& metaField,
                       const std::set<std::string>& fields,
                       const std::set<std::string>& computedProjections)
    : _fieldSet(fields),
      _computedMetaProjFields(computedProjections),
      _timeField(timeField),
      _timeFieldHashed(FieldNameHasher().hashedFieldName(_timeField)),
      _metaField(metaField) {
    if (_metaField) {
        _metaFieldHashed = FieldNameHasher().hashedFieldName(*_metaField);
    }
}

BucketSpec::BucketSpec(const BucketSpec& other)
    : _fieldSet(other._fieldSet),
      _computedMetaProjFields(other._computedMetaProjFields),
      _timeField(other._timeField),
      _timeFieldHashed(HashedFieldName{_timeField, other._timeFieldHashed->hash()}),
      _metaField(other._metaField) {
    if (_metaField) {
        _metaFieldHashed = HashedFieldName{*_metaField, other._metaFieldHashed->hash()};
    }
}

BucketSpec::BucketSpec(BucketSpec&& other)
    : _fieldSet(std::move(other._fieldSet)),
      _computedMetaProjFields(std::move(other._computedMetaProjFields)),
      _timeField(std::move(other._timeField)),
      _timeFieldHashed(HashedFieldName{_timeField, other._timeFieldHashed->hash()}),
      _metaField(std::move(other._metaField)) {
    if (_metaField) {
        _metaFieldHashed = HashedFieldName{*_metaField, other._metaFieldHashed->hash()};
    }
}

BucketSpec& BucketSpec::operator=(const BucketSpec& other) {
    if (&other != this) {
        _fieldSet = other._fieldSet;
        _computedMetaProjFields = other._computedMetaProjFields;
        _timeField = other._timeField;
        _timeFieldHashed = HashedFieldName{_timeField, other._timeFieldHashed->hash()};
        _metaField = other._metaField;
        if (_metaField) {
            _metaFieldHashed = HashedFieldName{*_metaField, other._metaFieldHashed->hash()};
        }
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

BucketUnpacker::BucketUnpacker() = default;
BucketUnpacker::BucketUnpacker(BucketUnpacker&& other) = default;
BucketUnpacker::~BucketUnpacker() = default;
BucketUnpacker& BucketUnpacker::operator=(BucketUnpacker&& rhs) = default;

BucketUnpacker::BucketUnpacker(BucketSpec spec, Behavior unpackerBehavior) {
    setBucketSpecAndBehavior(std::move(spec), unpackerBehavior);
}

void BucketUnpacker::addComputedMetaProjFields(const std::vector<StringData>& computedFieldNames) {
    for (auto&& field : computedFieldNames) {
        _spec.addComputedMetaProjFields(field);

        // If we're already specifically including fields, we need to add the computed fields to
        // the included field set to indicate they're in the output doc.
        if (_unpackerBehavior == BucketUnpacker::Behavior::kInclude) {
            _spec.addIncludeExcludeField(field);
        } else {
            // Since exclude is applied after addComputedMetaProjFields, we must erase the new field
            // from the include/exclude fields so this doesn't get removed.
            _spec.removeIncludeExcludeField(field.toString());
        }
    }

    // Recalculate _includeTimeField, since both computedMetaProjFields and fieldSet may have
    // changed.
    determineIncludeTimeField();
}

Document BucketUnpacker::getNext() {
    tassert(5521503, "'getNext()' requires the bucket to be owned", _bucket.isOwned());
    tassert(5422100, "'getNext()' was called after the bucket has been exhausted", hasNext());

    // MutableDocument reserves memory based on the number of fields, but uses a fixed size of 25
    // bytes plus an allowance of 7 characters for the field name. Doubling the number of fields
    // should give us enough overhead for longer field names without wasting too much memory.
    auto measurement = MutableDocument{2 * _unpackingImpl->numberOfFields()};
    _hasNext = _unpackingImpl->getNext(
        measurement, _spec, _metaValue, _includeTimeField, _includeMetaField);

    // Add computed meta projections.
    for (auto&& name : _spec.computedMetaProjFields()) {
        measurement.addField(name, Value{_computedMetaProjections[name]});
    }

    if (_includeMinTimeAsMetadata && _minTime) {
        measurement.metadata().setTimeseriesBucketMinTime(*_minTime);
    }

    if (_includeMaxTimeAsMetadata && _maxTime) {
        measurement.metadata().setTimeseriesBucketMaxTime(*_maxTime);
    }

    return measurement.freeze();
}

Document BucketUnpacker::extractSingleMeasurement(int j) {
    tassert(5422101,
            "'extractSingleMeasurment' expects j to be greater than or equal to zero and less than "
            "or equal to the number of measurements in a bucket",
            j >= 0 && j < _numberOfMeasurements);

    auto measurement = MutableDocument{};
    _unpackingImpl->extractSingleMeasurement(measurement,
                                             j,
                                             _spec,
                                             fieldsToIncludeExcludeDuringUnpack(),
                                             _unpackerBehavior,
                                             _bucket,
                                             _metaValue,
                                             _includeTimeField,
                                             _includeMetaField);

    // Add computed meta projections.
    for (auto&& name : _spec.computedMetaProjFields()) {
        measurement.addField(name, Value{_computedMetaProjections[name]});
    }

    return measurement.freeze();
}

void BucketUnpacker::reset(BSONObj&& bucket) {
    _unpackingImpl.reset();
    _bucket = std::move(bucket);
    uassert(5346510, "An empty bucket cannot be unpacked", !_bucket.isEmpty());

    auto&& dataRegion = _bucket.getField(timeseries::kBucketDataFieldName).Obj();
    if (dataRegion.isEmpty()) {
        // If the data field of a bucket is present but it holds an empty object, there's nothing to
        // unpack.
        return;
    }

    auto&& timeFieldElem = dataRegion.getField(_spec.timeField());
    uassert(5346700,
            "The $_internalUnpackBucket stage requires the data region to have a timeField object",
            timeFieldElem);

    _metaValue = Value{_bucket[timeseries::kBucketMetaFieldName]};
    if (_spec.metaField()) {
        // The spec indicates that there might be a metadata region. Missing metadata in
        // measurements is expressed with missing metadata in a bucket. But we disallow undefined
        // since the undefined BSON type is deprecated.
        uassert(5369600,
                "The $_internalUnpackBucket stage allows metadata to be absent or otherwise, it "
                "must not be the deprecated undefined bson type",
                _metaValue.missing() || _metaValue.getType() != BSONType::Undefined);
    } else {
        // If the spec indicates that the time series collection has no metadata field, then we
        // should not find a metadata region in the underlying bucket documents.
        uassert(5369601,
                "The $_internalUnpackBucket stage expects buckets to have missing metadata regions "
                "if the metaField parameter is not provided",
                _metaValue.missing());
    }

    auto&& controlField = _bucket[timeseries::kBucketControlFieldName];
    uassert(5857902,
            "The $_internalUnpackBucket stage requires 'control' object to be present",
            controlField && controlField.type() == BSONType::Object);

    if (_includeMinTimeAsMetadata) {
        auto&& controlMin = controlField.Obj()[timeseries::kBucketControlMinFieldName];
        uassert(6460203,
                str::stream() << "The $_internalUnpackBucket stage requires '"
                              << timeseries::kControlMinFieldNamePrefix << "' object to be present",
                controlMin && controlMin.type() == BSONType::Object);
        auto&& minTime = controlMin.Obj()[_spec.timeField()];
        uassert(6460204,
                str::stream() << "The $_internalUnpackBucket stage requires '"
                              << timeseries::kControlMinFieldNamePrefix << "." << _spec.timeField()
                              << "' to be a date",
                minTime && minTime.type() == BSONType::Date);
        _minTime = minTime.date();
    }

    if (_includeMaxTimeAsMetadata) {
        auto&& controlMax = controlField.Obj()[timeseries::kBucketControlMaxFieldName];
        uassert(6460205,
                str::stream() << "The $_internalUnpackBucket stage requires '"
                              << timeseries::kControlMaxFieldNamePrefix << "' object to be present",
                controlMax && controlMax.type() == BSONType::Object);
        auto&& maxTime = controlMax.Obj()[_spec.timeField()];
        uassert(6460206,
                str::stream() << "The $_internalUnpackBucket stage requires '"
                              << timeseries::kControlMaxFieldNamePrefix << "." << _spec.timeField()
                              << "' to be a date",
                maxTime && maxTime.type() == BSONType::Date);
        _maxTime = maxTime.date();
    }

    auto&& versionField = controlField.Obj()[timeseries::kBucketControlVersionFieldName];
    uassert(5857903,
            "The $_internalUnpackBucket stage requires 'control.version' field to be present",
            versionField && isNumericBSONType(versionField.type()));
    auto version = versionField.Number();

    if (version == 1) {
        _unpackingImpl = std::make_unique<BucketUnpackerV1>(timeFieldElem);
    } else if (version == 2) {
        auto countField = controlField.Obj()[timeseries::kBucketControlCountFieldName];
        _unpackingImpl =
            std::make_unique<BucketUnpackerV2>(timeFieldElem,
                                               countField && isNumericBSONType(countField.type())
                                                   ? static_cast<int>(countField.Number())
                                                   : -1);
    } else {
        uasserted(5857900, "Invalid bucket version");
    }

    // Walk the data region of the bucket, and decide if an iterator should be set up based on the
    // include or exclude case.
    for (auto&& elem : dataRegion) {
        auto colName = elem.fieldNameStringData();
        if (colName == _spec.timeField()) {
            // Skip adding a FieldIterator for the timeField since the timestamp value from
            // _timeFieldIter can be placed accordingly in the materialized measurement.
            continue;
        }

        // Includes a field when '_unpackerBehavior' is 'kInclude' and it's found in 'fieldSet' or
        // _unpackerBehavior is 'kExclude' and it's not found in 'fieldSet'.
        if (determineIncludeField(
                colName, _unpackerBehavior, fieldsToIncludeExcludeDuringUnpack())) {
            _unpackingImpl->addField(elem);
        }
    }

    // Update computed meta projections with values from this bucket.
    for (auto&& name : _spec.computedMetaProjFields()) {
        _computedMetaProjections[name] = _bucket[name];
    }

    // Save the measurement count for the bucket.
    _numberOfMeasurements = _unpackingImpl->measurementCount(timeFieldElem);
    _hasNext = _numberOfMeasurements > 0;
}

int BucketUnpacker::computeMeasurementCount(const BSONObj& bucket, StringData timeField) {
    auto&& controlField = bucket[timeseries::kBucketControlFieldName];
    uassert(5857904,
            "The $_internalUnpackBucket stage requires 'control' object to be present",
            controlField && controlField.type() == BSONType::Object);

    auto&& versionField = controlField.Obj()[timeseries::kBucketControlVersionFieldName];
    uassert(5857905,
            "The $_internalUnpackBucket stage requires 'control.version' field to be present",
            versionField && isNumericBSONType(versionField.type()));

    auto&& dataField = bucket[timeseries::kBucketDataFieldName];
    if (!dataField || dataField.type() != BSONType::Object)
        return 0;

    auto&& time = dataField.Obj()[timeField];
    if (!time) {
        return 0;
    }

    auto version = versionField.Number();
    if (version == 1) {
        return BucketUnpackerV1::computeElementCountFromTimestampObjSize(time.objsize());
    } else if (version == 2) {
        auto countField = controlField.Obj()[timeseries::kBucketControlCountFieldName];
        if (countField && isNumericBSONType(countField.type())) {
            return static_cast<int>(countField.Number());
        }

        return BSONColumn(time).size();
    } else {
        uasserted(5857901, "Invalid bucket version");
    }
}

void BucketUnpacker::determineIncludeTimeField() {
    const bool isInclude = _unpackerBehavior == BucketUnpacker::Behavior::kInclude;
    const bool fieldSetContainsTime =
        _spec.fieldSet().find(_spec.timeField()) != _spec.fieldSet().end();

    const auto& metaProjFields = _spec.computedMetaProjFields();
    const bool metaProjContains = metaProjFields.find(_spec.timeField()) != metaProjFields.cend();

    // If computedMetaProjFields contains the time field, we exclude it from unpacking no matter
    // what, since it will be overwritten anyway.
    _includeTimeField = isInclude == fieldSetContainsTime && !metaProjContains;
}

void BucketUnpacker::eraseMetaFromFieldSetAndDetermineIncludeMeta() {
    if (!_spec.metaField() ||
        _spec.computedMetaProjFields().find(*_spec.metaField()) !=
            _spec.computedMetaProjFields().cend()) {
        _includeMetaField = false;
    } else if (auto itr = _spec.fieldSet().find(*_spec.metaField());
               itr != _spec.fieldSet().end()) {
        _spec.removeIncludeExcludeField(*_spec.metaField());
        _includeMetaField = _unpackerBehavior == BucketUnpacker::Behavior::kInclude;
    } else {
        _includeMetaField = _unpackerBehavior == BucketUnpacker::Behavior::kExclude;
    }
}

void BucketUnpacker::eraseExcludedComputedMetaProjFields() {
    if (_unpackerBehavior == BucketUnpacker::Behavior::kExclude) {
        for (const auto& field : _spec.fieldSet()) {
            _spec.eraseFromComputedMetaProjFields(field);
        }
    }
}

void BucketUnpacker::setBucketSpecAndBehavior(BucketSpec&& bucketSpec, Behavior behavior) {
    _unpackerBehavior = behavior;
    _spec = std::move(bucketSpec);

    eraseMetaFromFieldSetAndDetermineIncludeMeta();
    determineIncludeTimeField();
    eraseExcludedComputedMetaProjFields();

    _includeMinTimeAsMetadata = _spec.includeMinTimeAsMetadata;
    _includeMaxTimeAsMetadata = _spec.includeMaxTimeAsMetadata;
}

void BucketUnpacker::setIncludeMinTimeAsMetadata() {
    _includeMinTimeAsMetadata = true;
}

void BucketUnpacker::setIncludeMaxTimeAsMetadata() {
    _includeMaxTimeAsMetadata = true;
}

const std::set<std::string>& BucketUnpacker::fieldsToIncludeExcludeDuringUnpack() {
    if (_unpackFieldsToIncludeExclude) {
        return *_unpackFieldsToIncludeExclude;
    }

    _unpackFieldsToIncludeExclude = std::set<std::string>();
    const auto& metaProjFields = _spec.computedMetaProjFields();
    if (_unpackerBehavior == BucketUnpacker::Behavior::kInclude) {
        // For include, we unpack fieldSet - metaProjFields.
        for (auto&& field : _spec.fieldSet()) {
            if (metaProjFields.find(field) == metaProjFields.cend()) {
                _unpackFieldsToIncludeExclude->insert(field);
            }
        }
    } else {
        // For exclude, we unpack everything but fieldSet + metaProjFields.
        _unpackFieldsToIncludeExclude->insert(_spec.fieldSet().begin(), _spec.fieldSet().end());
        _unpackFieldsToIncludeExclude->insert(metaProjFields.begin(), metaProjFields.end());
    }

    return *_unpackFieldsToIncludeExclude;
}

const std::set<StringData> BucketUnpacker::reservedBucketFieldNames = {
    timeseries::kBucketIdFieldName,
    timeseries::kBucketDataFieldName,
    timeseries::kBucketMetaFieldName,
    timeseries::kBucketControlFieldName};

}  // namespace mongo
