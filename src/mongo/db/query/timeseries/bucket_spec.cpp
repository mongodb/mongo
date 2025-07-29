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

#include "mongo/db/query/timeseries/bucket_spec.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/db/query/compiler/rewrites/matcher/rewrite_expr.h"
#include "mongo/db/query/timeseries/bucket_level_comparison_predicate_generator.h"
#include "mongo/db/query/timeseries/bucket_level_id_predicate_generator.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <algorithm>
#include <cstddef>

#include <s2cellid.h>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::timeseries {

using IneligiblePredicatePolicy = BucketSpec::IneligiblePredicatePolicy;

bool BucketSpec::fieldIsComputed(StringData field) const {
    return std::any_of(
        _computedMetaProjFields.begin(), _computedMetaProjFields.end(), [&](auto& s) {
            return s == field || expression::isPathPrefixOf(field, s) ||
                expression::isPathPrefixOf(s, field);
        });
}

namespace {
std::unique_ptr<MatchExpression> createTightExprTimeFieldPredicate(
    const ExprMatchExpression* matchExpr,
    const BucketLevelComparisonPredicateGeneratorBase::Params params) {
    RewriteExpr::RewriteResult rewriteRes =
        RewriteExpr::rewrite(matchExpr->getExpression(), params.pExpCtx->getCollator());
    auto unownedExpr = rewriteRes.matchExpression();

    // There might be children in the $and expression that cannot be rewritten to a match
    // expression. If this is the case we cannot assume that the tight predicate or
    // wholeBucketFilter produced by the rewritten $and expression is correct. Measurements in the
    // bucket might fit the rewritten $and expression, but fail to fit the other children of the
    // $and expression and will be returned incorrectly.

    // It is an error to call 'createPredicate' on predicates on the meta field, and it only
    // returns a value for predicates on the 'timeField'.
    if (unownedExpr && rewriteRes.allSubExpressionsRewritten() &&
        unownedExpr->path() == params.bucketSpec.timeField() &&
        ComparisonMatchExpressionBase::isInternalExprComparison(unownedExpr->matchType())) {
        const auto compareMatchExpr =
            checked_cast<const ComparisonMatchExpressionBase*>(unownedExpr);
        return BucketLevelComparisonPredicateGenerator::createPredicate(
                   compareMatchExpr, params, true /* tight */)
            .matchExpr;
    }

    return BucketSpec::handleIneligible(
               BucketSpec::IneligiblePredicatePolicy::kIgnore,
               matchExpr,
               "can only handle comparison $expr match expressions on the timeField")
        .tightPredicate;
}

}  // namespace

BucketSpec::BucketPredicate BucketSpec::handleIneligible(IneligiblePredicatePolicy policy,
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

BucketSpec::BucketPredicate BucketSpec::createPredicatesOnBucketLevelField(
    const MatchExpression* matchExpr,
    const BucketSpec& bucketSpec,
    int bucketMaxSpanSeconds,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    bool haveComputedMetaField,
    bool includeMetaField,
    bool assumeNoMixedSchemaData,
    IneligiblePredicatePolicy policy,
    bool fixedBuckets) {

    tassert(5916304, "BucketSpec::createPredicatesOnBucketLevelField nullptr", matchExpr);

    auto bucketPredicateParams = BucketLevelComparisonPredicateGeneratorBase::Params{
        bucketSpec, bucketMaxSpanSeconds, pExpCtx, assumeNoMixedSchemaData, policy, fixedBuckets};
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

        if (auto looseResult = expression::copyExpressionAndApplyRenames(
                matchExpr, {{bucketSpec.metaField().value(), std::string{kBucketMetaFieldName}}});
            looseResult) {
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
        bool rewriteProvidesExactMatchPredicate = true;
        for (size_t i = 0; i < nextAnd->numChildren(); i++) {
            auto child = createPredicatesOnBucketLevelField(nextAnd->getChild(i),
                                                            bucketSpec,
                                                            bucketMaxSpanSeconds,
                                                            pExpCtx,
                                                            haveComputedMetaField,
                                                            includeMetaField,
                                                            assumeNoMixedSchemaData,
                                                            policy,
                                                            fixedBuckets);
            if (child.loosePredicate) {
                looseAndExpression->add(std::move(child.loosePredicate));
            }

            if (tightAndExpression && child.tightPredicate) {
                tightAndExpression->add(std::move(child.tightPredicate));
            } else {
                // For tight expression, null means always false, we can short circuit here.
                tightAndExpression = nullptr;
            }
            rewriteProvidesExactMatchPredicate =
                rewriteProvidesExactMatchPredicate && child.rewriteProvidesExactMatchPredicate;
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

        return {std::move(looseExpression),
                std::move(tightExpression),
                rewriteProvidesExactMatchPredicate};
    } else if (matchExpr->matchType() == MatchExpression::OR) {
        // Given {$or: [A, B]}, suppose A, B can be pushed down as A', B'.
        // If an event matches {$or: [A, B]} then either:
        //     - it matches A, which means any bucket containing it matches A'
        //     - it matches B, which means any bucket containing it matches B'
        // So {$or: [A', B']} will capture all the buckets we need to satisfy {$or: [A, B]}.
        auto nextOr = static_cast<const OrMatchExpression*>(matchExpr);
        auto looseOrExpression = std::make_unique<OrMatchExpression>();
        auto tightOrExpression = std::make_unique<OrMatchExpression>();
        bool rewriteProvidesExactMatchPredicate = true;

        for (size_t i = 0; i < nextOr->numChildren(); i++) {
            auto child = createPredicatesOnBucketLevelField(nextOr->getChild(i),
                                                            bucketSpec,
                                                            bucketMaxSpanSeconds,
                                                            pExpCtx,
                                                            haveComputedMetaField,
                                                            includeMetaField,
                                                            assumeNoMixedSchemaData,
                                                            policy,
                                                            fixedBuckets);
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
            rewriteProvidesExactMatchPredicate =
                rewriteProvidesExactMatchPredicate && child.rewriteProvidesExactMatchPredicate;
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

        return {std::move(looseExpression),
                std::move(tightExpression),
                rewriteProvidesExactMatchPredicate};
    } else if (ComparisonMatchExpression::isComparisonMatchExpression(matchExpr) ||
               ComparisonMatchExpressionBase::isInternalExprComparison(matchExpr->matchType())) {
        auto result = BucketLevelComparisonPredicateGenerator::createPredicate(
            checked_cast<const ComparisonMatchExpressionBase*>(matchExpr),
            bucketPredicateParams,
            false /* tight */);

        auto tightResult = BucketLevelComparisonPredicateGenerator::createPredicate(
            checked_cast<const ComparisonMatchExpressionBase*>(matchExpr),
            bucketPredicateParams,
            true /* tight */);
        return {std::move(result.matchExpr),
                std::move(tightResult.matchExpr),
                result.rewriteProvidesExactMatchPredicate};
    } else if (matchExpr->matchType() == MatchExpression::EXPRESSION) {
        return {
            // The loose predicate will be pushed before the unpacking which will be inspected by
            // the query planner. Since the classic planner doesn't handle the $expr expression, we
            // don't generate the loose predicate.
            nullptr,
            createTightExprTimeFieldPredicate(checked_cast<const ExprMatchExpression*>(matchExpr),
                                              bucketPredicateParams)};
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
            result->add(std::make_unique<ExistsMatchExpression>(
                StringData(std::string{kControlMinFieldNamePrefix} + matchExpr->path())));
            result->add(std::make_unique<ExistsMatchExpression>(
                StringData(std::string{kControlMaxFieldNamePrefix} + matchExpr->path())));
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
        bool rewriteProvidesExactMatchPredicate = true;
        for (auto&& elem : inExpr->getEqualities()) {
            // If inExpr is {$in: [X, Y]} then the elems are '0: X' and '1: Y'.
            auto eq = std::make_unique<EqualityMatchExpression>(
                inExpr->path(), elem, nullptr /*annotation*/, inExpr->getCollator());
            auto child = BucketLevelComparisonPredicateGenerator::createPredicate(
                eq.get(), bucketPredicateParams, false /* tight */);
            rewriteProvidesExactMatchPredicate =
                rewriteProvidesExactMatchPredicate && child.rewriteProvidesExactMatchPredicate;

            // As with OR, only add the child if it has been succesfully translated, otherwise
            // the $in cannot be correctly mapped to bucket level fields and we should return
            // nullptr.
            if (child.matchExpr) {
                result->add(std::move(child.matchExpr));
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
        return {std::move(result), nullptr, rewriteProvidesExactMatchPredicate};
    }
    return handleIneligible(policy, matchExpr, "can't handle this predicate");
}

bool BucketSpec::generateBucketLevelIdPredicates(const ExpressionContext& pExpCtx,
                                                 const BucketSpec& bucketSpec,
                                                 int bucketMaxSpanSeconds,
                                                 MatchExpression* matchExpr) {

    return BucketLevelIdPredicateGenerator::generateIdPredicates(
        pExpCtx, bucketSpec, bucketMaxSpanSeconds, matchExpr);
}

std::pair<bool, BSONObj> BucketSpec::pushdownPredicate(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const TimeseriesOptions& tsOptions,
    const BSONObj& predicate,
    bool haveComputedMetaField,
    bool includeMetaField,
    bool assumeNoMixedSchemaData,
    IneligiblePredicatePolicy policy,
    bool fixedBuckets) {
    auto [metaOnlyPred, bucketMetricPred, residualPred] =
        getPushdownPredicates(expCtx,
                              tsOptions,
                              predicate,
                              haveComputedMetaField,
                              includeMetaField,
                              assumeNoMixedSchemaData,
                              policy,
                              fixedBuckets);
    BSONObjBuilder result;
    if (metaOnlyPred)
        metaOnlyPred->serialize(&result, {});
    if (bucketMetricPred)
        bucketMetricPred->serialize(&result, {});
    return std::make_pair(bucketMetricPred.get(), result.obj());
}

std::pair<std::unique_ptr<MatchExpression>, std::unique_ptr<MatchExpression>>
BucketSpec::splitOutMetaOnlyPredicate(std::unique_ptr<MatchExpression> expr,
                                      boost::optional<StringData> metaField) {
    if (!metaField) {
        // If there's no metadata field, then none of the predicates are metadata-only
        // predicates.
        return std::make_pair(std::unique_ptr<MatchExpression>(nullptr), std::move(expr));
    }

    return expression::splitMatchExpressionBy(
        std::move(expr),
        {std::string{*metaField}},
        {{std::string{*metaField}, std::string{kBucketMetaFieldName}}},
        expression::isOnlyDependentOn);
}

BucketSpec::SplitPredicates BucketSpec::getPushdownPredicates(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const TimeseriesOptions& tsOptions,
    const BSONObj& predicate,
    bool haveComputedMetaField,
    bool includeMetaField,
    bool assumeNoMixedSchemaData,
    IneligiblePredicatePolicy policy,
    bool fixedBuckets) {

    auto allowedFeatures = MatchExpressionParser::kDefaultSpecialFeatures;
    auto matchExpr = uassertStatusOK(
        MatchExpressionParser::parse(predicate, expCtx, ExtensionsCallbackNoop(), allowedFeatures));

    auto metaField = haveComputedMetaField ? boost::none : tsOptions.getMetaField();
    auto [metaOnlyPred, residualPred] = splitOutMetaOnlyPredicate(std::move(matchExpr), metaField);

    std::unique_ptr<MatchExpression> bucketMetricPred = nullptr;
    if (residualPred) {
        BucketSpec bucketSpec{
            std::string{tsOptions.getTimeField()},
            metaField.map([](StringData s) { return std::string{s}; }),
            // Since we are operating on a collection, not a query-result,
            // there are no inclusion/exclusion projections we need to apply
            // to the buckets before unpacking. So we can use default values
            // for the rest of the arguments.
        };
        auto bucketPredicate =
            createPredicatesOnBucketLevelField(residualPred.get(),
                                               bucketSpec,
                                               *tsOptions.getBucketMaxSpanSeconds(),
                                               expCtx,
                                               haveComputedMetaField,
                                               includeMetaField,
                                               assumeNoMixedSchemaData,
                                               policy,
                                               fixedBuckets);
        bucketMetricPred = std::move(bucketPredicate.loosePredicate);
        if (!expCtx->getRequiresTimeseriesExtendedRangeSupport()) {
            // It may be possible to generate _id predicates or even '$alwaysTrue' or '$alwaysFalse'
            // predicates if we know there is no extended range data.
            bool changed = generateBucketLevelIdPredicates(
                *expCtx, bucketSpec, *tsOptions.getBucketMaxSpanSeconds(), bucketMetricPred.get());
            if (changed) {
                bucketMetricPred = normalizeMatchExpression(std::move(bucketMetricPred));
            }
        }
        if (bucketPredicate.rewriteProvidesExactMatchPredicate) {
            residualPred = nullptr;
        } else {
            residualPred = normalizeMatchExpression(std::move(residualPred));
        }
    }

    return {.metaOnlyExpr = std::move(metaOnlyPred),
            .bucketMetricExpr = std::move(bucketMetricPred),
            .residualExpr = std::move(residualPred)};
}

BucketSpec::BucketSpec(std::string timeField,
                       boost::optional<std::string> metaField,
                       std::set<std::string> fields,
                       Behavior behavior,
                       std::set<std::string> computedProjections,
                       bool usesExtendedRange)
    : _fieldSet(std::move(fields)),
      _behavior(behavior),
      _computedMetaProjFields(std::move(computedProjections)),
      _timeField(std::move(timeField)),
      _timeFieldHashed(FieldNameHasher().hashedFieldName(_timeField)),
      _metaField(std::move(metaField)),
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
    : BucketSpec(std::string{tsOptions.getTimeField()},
                 tsOptions.getMetaField()
                     ? boost::optional<string>(std::string{*tsOptions.getMetaField()})
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

BucketSpec& BucketSpec::operator=(BucketSpec&& other) {
    _fieldSet = std::move(other._fieldSet);
    _behavior = other._behavior;
    _computedMetaProjFields = std::move(other._computedMetaProjFields);
    _timeField = std::move(other._timeField);
    _timeFieldHashed = HashedFieldName{_timeField, other._timeFieldHashed->hash()};
    _metaField = std::move(other._metaField);
    if (_metaField) {
        _metaFieldHashed = HashedFieldName{*_metaField, other._metaFieldHashed->hash()};
    }
    _usesExtendedRange = other._usesExtendedRange;
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
    invariant(_timeFieldHashed->key().data() == _timeField.data());
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
}  // namespace mongo::timeseries
