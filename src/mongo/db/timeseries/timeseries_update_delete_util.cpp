// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/timeseries_update_delete_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/timeseries/bucket_spec.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"

#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo::timeseries {
namespace {

/**
 * Expression used to filter out closed buckets for timeseries writes. The 'control.closed' field
 * may not exist or may be false. To be safe, filter on 'control.closed' != true.
 */
static const std::unique_ptr<MatchExpression> closedBucketFilter =
    std::make_unique<NotMatchExpression>(std::make_unique<EqualityMatchExpression>(
        std::string_view(std::string{timeseries::kBucketControlFieldName} + "." +
                         std::string{timeseries::kBucketControlClosedFieldName}),
        Value(true)));

/**
 * Returns whether the given metaField is the first element of the dotted path in the given
 * field.
 */
bool isFieldFirstElementOfDottedPathField(std::string_view field, std::string_view metaField) {
    return field.substr(0, field.find('.')) == metaField;
}

/**
 * Returns a string where the substring leading up to "." in the given field is replaced with
 * "meta". If there is no "." in the given field, returns "meta".
 */
std::string getRenamedField(std::string_view field) {
    size_t dotIndex = field.find('.');
    return dotIndex != std::string::npos
        ? "meta" + std::string{field.substr(dotIndex, field.size() - dotIndex)}
        : "meta";
}

void assertQueryFieldIsMetaField(bool isMetaField, std::string_view metaField) {
    uassert(ErrorCodes::InvalidOptions,
            fmt::format("Cannot perform an update or delete on a time-series collection "
                        "when querying on a field that is not the metaField '{}'",
                        metaField),
            isMetaField);
}

Status checkUpdateFieldIsMetaField(bool isMetaField, std::string_view metaField) {
    return isMetaField
        ? Status::OK()
        : Status(ErrorCodes::InvalidOptions,
                 fmt::format("Cannot perform an update on a time-series collection which updates a "
                             "field that is not the metaField '{}'",
                             metaField));
}

/**
 * Recurses through the mutablebson element query and replaces any occurrences of the metaField with
 * "meta" accounting for queries that may be in dot notation. isTopLevelField is set for elements
 * which contain a top-level field name, and parentIsArray is set for elements with an array as a
 * parent.
 */
void replaceQueryMetaFieldName(mutablebson::Element elem,
                               std::string_view metaField,
                               bool isTopLevelField = true,
                               bool parentIsArray = false) {
    auto fieldName = elem.getFieldName();

    uassert(ErrorCodes::InvalidOptions,
            "Cannot use $expr when performing an update or delete on a time-series collection",
            fieldName != "$expr");

    uassert(ErrorCodes::InvalidOptions,
            "Cannot use $where when performing an update or delete on a time-series collection",
            fieldName != "$where");

    // Replace any occurences of the metaField in the top-level required fields of the JSON Schema
    // object with "meta".
    if (fieldName == "$jsonSchema") {
        mutablebson::Element requiredElem = elem.findFirstChildNamed("required");
        if (requiredElem.ok()) {
            for (auto subElem = requiredElem.leftChild(); subElem.ok();
                 subElem = subElem.rightSibling()) {
                assertQueryFieldIsMetaField(subElem.isType(BSONType::string) &&
                                                subElem.getValueString() == metaField,
                                            metaField);
                invariantStatusOK(
                    subElem.setValueString(getRenamedField(subElem.getValueString())));
            }
        }

        mutablebson::Element propertiesElem = elem.findFirstChildNamed("properties");
        if (propertiesElem.ok()) {
            for (auto property = propertiesElem.leftChild(); property.ok();
                 property = property.rightSibling()) {
                assertQueryFieldIsMetaField(property.getFieldName() == metaField, metaField);
                invariantStatusOK(property.rename(getRenamedField(property.getFieldName())));
            }
        }

        return;
    }

    if (isTopLevelField && (fieldName.empty() || fieldName[0] != '$')) {
        assertQueryFieldIsMetaField(isFieldFirstElementOfDottedPathField(fieldName, metaField),
                                    metaField);
        invariantStatusOK(elem.rename(getRenamedField(fieldName)));
    }

    isTopLevelField = parentIsArray && elem.isType(BSONType::object);
    parentIsArray = elem.isType(BSONType::array);
    for (auto child = elem.leftChild(); child.ok(); child = child.rightSibling()) {
        replaceQueryMetaFieldName(child, metaField, isTopLevelField, parentIsArray);
    }
}
}  // namespace

BSONObj translateQuery(const BSONObj& query, std::string_view metaField) {
    invariant(!metaField.empty());

    mutablebson::Document queryDoc(query);
    for (auto queryElem = queryDoc.root().leftChild(); queryElem.ok();
         queryElem = queryElem.rightSibling()) {
        replaceQueryMetaFieldName(queryElem, metaField);
    }

    return queryDoc.getObject();
}

StatusWith<write_ops::UpdateModification> translateUpdate(
    const write_ops::UpdateModification& updateMod, boost::optional<std::string_view> metaField) {
    invariant(updateMod.type() != write_ops::UpdateModification::Type::kDelta);

    if (updateMod.type() == write_ops::UpdateModification::Type::kPipeline) {
        return Status(
            ErrorCodes::InvalidOptions,
            "Cannot perform an update on a time-series collection using a pipeline update");
    }

    if (updateMod.type() == write_ops::UpdateModification::Type::kReplacement) {
        return Status(
            ErrorCodes::InvalidOptions,
            "Cannot perform an update on a time-series collection using a replacement document");
    }

    // We can't translate an update without a meta field.
    if (!metaField) {
        return Status(ErrorCodes::InvalidOptions,
                      "Cannot perform an update on a time-series collection that does not have a "
                      "metaField");
    }

    const auto& document = updateMod.getUpdateModifier();

    // Make a mutable copy of the update document in order to replace all occurrences of the
    // metaField with "meta".
    mutablebson::Document updateDoc{document};
    // updateDoc = { <updateOperator> : { <field1>: <value1>, ... },
    //               <updateOperator> : { <field1>: <value1>, ... },
    //               ... }

    // updateDoc.root() = <updateOperator> : { <field1>: <value1>, ... },
    //                    <updateOperator> : { <field1>: <value1>, ... },
    //                    ...
    for (auto updatePair = updateDoc.root().leftChild(); updatePair.ok();
         updatePair = updatePair.rightSibling()) {
        // updatePair = <updateOperator> : { <field1>: <value1>, ... }
        // Check each field that is being modified by the update operator and replace it if it is
        // the metaField.
        for (auto fieldValuePair = updatePair.leftChild(); fieldValuePair.ok();
             fieldValuePair = fieldValuePair.rightSibling()) {
            auto fieldName = fieldValuePair.getFieldName();

            auto status = checkUpdateFieldIsMetaField(
                isFieldFirstElementOfDottedPathField(fieldName, *metaField), *metaField);
            if (!status.isOK()) {
                return status;
            }
            invariantStatusOK(fieldValuePair.rename(getRenamedField(fieldName)));

            // If this is a $rename, we also need to translate the value.
            if (updatePair.getFieldName() == "$rename") {
                auto status = checkUpdateFieldIsMetaField(
                    fieldValuePair.isType(BSONType::string) &&
                        isFieldFirstElementOfDottedPathField(fieldValuePair.getValueString(),
                                                             *metaField),
                    *metaField);
                if (!status.isOK()) {
                    return status;
                }
                invariantStatusOK(fieldValuePair.setValueString(
                    getRenamedField(fieldValuePair.getValueString())));
            }
        }
    }

    return write_ops::UpdateModification::parseFromClassicUpdate(updateDoc.getObject());
}

std::function<size_t(const BSONObj&)> numMeasurementsForBucketCounter(std::string_view timeField) {
    return [timeField = std::string{timeField}](const BSONObj& bucket) {
        return BucketUnpacker::computeMeasurementCount(bucket, timeField);
    };
}

namespace {
/**
 * Combines the given MatchExpressions into a single AndMatchExpression by $and-ing them. If there
 * is only one non-null expression, returns that expression. If there are no non-null expressions,
 * returns nullptr.
 *
 * Ts must be convertible to std::unique_ptr<MatchExpression>.
 */
template <typename... Ts>
std::unique_ptr<MatchExpression> andCombineMatchExpressions(Ts&&... matchExprs) {
    std::vector<std::unique_ptr<MatchExpression>> matchExprVector =
        makeVectorIfNotNull(std::forward<Ts>(matchExprs)...);
    if (matchExprVector.empty()) {
        return nullptr;
    }
    return matchExprVector.size() > 1
        ? std::make_unique<AndMatchExpression>(std::move(matchExprVector))
        : std::move(matchExprVector[0]);
}
}  // namespace

BSONObj getBucketLevelPredicateForRouting(const BSONObj& originalQuery,
                                          const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          const TimeseriesOptions& tsOptions,
                                          bool allowArbitraryWrites) {
    if (originalQuery.isEmpty()) {
        return BSONObj();
    }

    auto&& metaField = tsOptions.getMetaField();
    if (!allowArbitraryWrites) {
        if (!metaField) {
            // In case the time-series collection does not have meta field defined, we broadcast
            // the request to all shards or use two phase protocol using empty predicate.
            return BSONObj();
        }

        // Translate the delete query into a query on the time-series collection's underlying
        // buckets collection.
        return timeseries::translateQuery(originalQuery, *metaField);
    }

    auto matchExpr = uassertStatusOK(
        MatchExpressionParser::parse(originalQuery,
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures));

    // If the meta field exists, split out the meta field predicate which can be potentially used
    // for bucket-level routing.
    auto [metaOnlyPred, residualPred] =
        BucketSpec::splitOutMetaOnlyPredicate(std::move(matchExpr), metaField);

    // Split out the time field predicate which can be potentially used for bucket-level routing.
    auto timeOnlyPred = residualPred
        ? expression::splitMatchExpressionBy(std::move(residualPred),
                                             {std::string{tsOptions.getTimeField()}} /*fields*/,
                                             {} /*renames*/,
                                             expression::isOnlyDependentOn,
                                             expression::isExprOnlyDependentOn)
              .first
        : std::unique_ptr<MatchExpression>{};

    // Translate the time field predicate into a predicate on the bucket-level time field.
    // The router has no visibility into shard-local bucket data, so we must conservatively
    // assume extended-range data may be present to avoid an unsafe optimization.
    BucketSpec routingBucketSpec{
        std::string{tsOptions.getTimeField()},
        metaField.map([](std::string_view s) { return std::string{s}; }),
    };
    routingBucketSpec.setUsesExtendedRange(true);
    std::unique_ptr<MatchExpression> timeBucketPred = timeOnlyPred
        ? BucketSpec::createPredicatesOnBucketLevelField(
              timeOnlyPred.get(),
              routingBucketSpec,
              *tsOptions.getBucketMaxSpanSeconds(),
              expCtx,
              false /*haveComputedMetaField*/,
              false /*includeMetaField*/,
              true /*assumeNoMixedSchemaData*/,
              BucketSpec::IneligiblePredicatePolicy::kIgnore /*policy*/,
              false /* fixedBuckets */)
              .loosePredicate
        : nullptr;

    // In case that the delete query does not contain any potential bucket-level routing predicate,
    // target the request to all shards using empty predicate.
    if (!metaOnlyPred && !timeBucketPred) {
        return BSONObj();
    }

    // Combine the meta field and time field predicates into a single predicate by $and-ing them
    // together.
    // Note: At least one of 'metaOnlyPred' or 'timeBucketPred' is not null. So, the result
    // expression is guaranteed to be not null.
    return andCombineMatchExpressions(std::move(metaOnlyPred), std::move(timeBucketPred))
        ->serialize();
}

TimeseriesWritesQueryExprs getMatchExprsForWrites(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const TimeseriesOptions& tsOptions,
    const BSONObj& writeQuery) {
    const bool fixedBuckets = canUseFixedBucketOptimizations(
        tsOptions, expCtx->getRequiresTimeseriesExtendedRangeSupport());
    auto [metaOnlyExpr, bucketMetricExpr, residualExpr] =
        BucketSpec::getPushdownPredicates(expCtx,
                                          tsOptions,
                                          writeQuery,
                                          /*haveComputedMetaField*/ false,
                                          tsOptions.getMetaField().has_value(),
                                          /*assumeNoMixedSchemaData*/ true,
                                          BucketSpec::IneligiblePredicatePolicy::kIgnore,
                                          fixedBuckets);

    // Combine the closed bucket filter and the bucket metric filter and the meta-only filter into a
    // single filter by $and-ing them together.
    return {._bucketExpr = andCombineMatchExpressions(
                closedBucketFilter->clone(), std::move(metaOnlyExpr), std::move(bucketMetricExpr)),
            ._residualExpr = std::move(residualExpr)};
}

std::unique_ptr<MatchExpression> addClosedBucketExclusionExpr(
    std::unique_ptr<MatchExpression> base) {
    return andCombineMatchExpressions(closedBucketFilter->clone(), std::move(base));
}
}  // namespace mongo::timeseries
