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

#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"

#include <cstddef>
#include <cstdint>

#include <s2cellid.h>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_streaming_group.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/monotonic_expression.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/timeseries/bucket_spec.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <ostream>
#include <string>
#include <tuple>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

/*
 * $_internalUnpackBucket is an internal stage for materializing time-series measurements from
 * time-series collections. It should never be used anywhere outside the MongoDB server.
 */
REGISTER_DOCUMENT_SOURCE(_internalUnpackBucket,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalUnpackBucket::createFromBsonInternal,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalUnpackBucket, DocumentSourceInternalUnpackBucket::id)

/*
 * $_unpackBucket is an alias of $_internalUnpackBucket. It only exposes the "timeField" and the
 * "metaField" parameters and is only used for special known use cases by other MongoDB products
 * rather than user applications.
 */
REGISTER_DOCUMENT_SOURCE(_unpackBucket,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalUnpackBucket::createFromBsonExternal,
                         AllowedWithApiStrict::kAlways);

namespace {
using timeseries::BucketSpec;
using timeseries::BucketUnpacker;

/**
 * A projection can be internalized if every field corresponds to a boolean value. Note that this
 * correctly rejects dotted fieldnames, which are mapped to objects internally.
 */
bool canInternalizeProjectObj(const BSONObj& projObj) {
    return std::all_of(projObj.begin(), projObj.end(), [](auto&& e) { return e.isBoolean(); });
}

/**
 * If 'src' represents an inclusion or exclusion $project, return a BSONObj representing it and a
 * bool indicating its type (true for inclusion, false for exclusion). Else return an empty BSONObj.
 */
auto getIncludeExcludeProjectAndType(DocumentSource* src) {
    if (const auto proj = dynamic_cast<DocumentSourceSingleDocumentTransformation*>(src); proj &&
        (proj->getTransformerType() ==
             TransformerInterface::TransformerType::kInclusionProjection ||
         proj->getTransformerType() ==
             TransformerInterface::TransformerType::kExclusionProjection)) {
        return std::pair{proj->getTransformer().serializeTransformation().toBson(),
                         proj->getTransformerType() ==
                             TransformerInterface::TransformerType::kInclusionProjection};
    }
    return std::pair{BSONObj{}, false};
}

/**
 * Creates a new DocumentSourceSort by pulling out the logic for getting maxMemoryUsageBytes.
 */
boost::intrusive_ptr<DocumentSourceSort> createNewSortWithMemoryUsage(
    const DocumentSourceSort& sort, const SortPattern& pattern, uint64_t limit) {
    return DocumentSourceSort::create(sort.getExpCtx(), pattern, {.limit = limit});
}

/**
 * Checks if a sort stage's pattern following our internal unpack bucket is suitable to be reordered
 * before us. The sort stage must refer exclusively to the meta field or any subfields.
 *
 * If this check is being used for lastpoint, the sort stage can also refer to the time field,
 * which should be the last field in the pattern.
 */
bool checkMetadataSortReorder(
    const SortPattern& sortPattern,
    StringData metaFieldStr,
    const boost::optional<std::string&> lastpointTimeField = boost::none) {
    auto timeFound = false;
    for (const auto& sortKey : sortPattern) {
        if (!sortKey.fieldPath.has_value()) {
            return false;
        }
        if (sortKey.fieldPath->getPathLength() < 1) {
            return false;
        }
        if (sortKey.fieldPath->getFieldName(0) != metaFieldStr) {
            if (lastpointTimeField && sortKey.fieldPath->fullPath() == lastpointTimeField.value()) {
                // If we are checking the sort pattern for the lastpoint case, 'time' is allowed.
                timeFound = true;
                continue;
            }
            return false;
        } else {
            if (lastpointTimeField && timeFound) {
                // The time field was not the last field in the sort pattern.
                return false;
            }
        }
    }
    // If we are checking for lastpoint, make sure we encountered the time field.
    return !lastpointTimeField || timeFound;
}

/**
 * Returns a new DocumentSort to reorder before current unpack bucket document.
 */
boost::intrusive_ptr<DocumentSourceSort> createMetadataSortForReorder(
    const DocumentSourceSort& sort,
    const boost::optional<std::string&> lastpointTimeField = boost::none,
    const boost::optional<std::string> groupIdField = boost::none,
    bool flipSort = false) {
    auto sortPattern = flipSort
        ? SortPattern(
              QueryPlannerCommon::reverseSortObj(
                  sort.getSortKeyPattern()
                      .serialize(SortPattern::SortKeySerialization::kForPipelineSerialization)
                      .toBson()),
              sort.getExpCtx())
        : sort.getSortKeyPattern();
    std::vector<SortPattern::SortPatternPart> updatedPattern;

    for (const auto& entry : sortPattern) {
        updatedPattern.push_back(entry);
        if (lastpointTimeField && entry.fieldPath->fullPath() == lastpointTimeField.value()) {
            updatedPattern.back().fieldPath =
                FieldPath((entry.isAscending ? timeseries::kControlMinFieldNamePrefix
                                             : timeseries::kControlMaxFieldNamePrefix) +
                          lastpointTimeField.value());
            updatedPattern.push_back(SortPattern::SortPatternPart{
                entry.isAscending,
                FieldPath((entry.isAscending ? timeseries::kControlMaxFieldNamePrefix
                                             : timeseries::kControlMinFieldNamePrefix) +
                          lastpointTimeField.value()),
                nullptr});
        } else {
            auto updated = FieldPath(timeseries::kBucketMetaFieldName);
            if (entry.fieldPath->getPathLength() > 1) {
                updated = updated.concat(entry.fieldPath->tail());
            }
            updatedPattern.back().fieldPath = updated;
        }
    }
    // After the modifications of the sortPattern are completed, for the lastPoint
    // optimizations, the group field needs to be added to the beginning of the sort pattern.
    // Do note that the modified sort pattern is for sorting within a group (within the bucket)
    // and the plan is to do the grouping and sort within on go.
    // If the group field is already in the sortPattern then it needs to moved to the first
    // position. A flip in the later case is not necessary anymore as the sort order was
    // already flipped.
    // Example 1: $group: {a:1}, $sort{b: 1, a: -1} --> modifiedPattern: {a: -1, b: 1}
    // Example 2: $group: {c:1}, $sort{d: -1, e: 1} --> modifiedPattern: {c: 1, d: -1, e: 1}
    if (groupIdField) {
        const auto groupId = FieldPath(groupIdField.value());
        SortPattern::SortPatternPart patternPart;
        patternPart.fieldPath = groupId;
        const auto pattern =
            std::find_if(updatedPattern.begin(),
                         updatedPattern.end(),
                         [&groupId](const SortPattern::SortPatternPart& s) -> bool {
                             return s.fieldPath->fullPath().compare(groupId.fullPath()) == 0;
                         });
        if (pattern != updatedPattern.end()) {
            patternPart.isAscending = pattern->isAscending;
            updatedPattern.erase(pattern);
        } else {
            patternPart.isAscending = !flipSort;
        }
        updatedPattern.insert(updatedPattern.begin(), patternPart);
    }

    return createNewSortWithMemoryUsage(sort, SortPattern{std::move(updatedPattern)}, 0);
}

/**
 * Returns a new DocumentSourceGroup to add before current unpack bucket document.
 */
boost::intrusive_ptr<DocumentSourceGroup> createBucketGroupForReorder(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::vector<std::string>& fieldsToInclude,
    const std::string& fieldPath) {
    auto groupByExpr = ExpressionFieldPath::createPathFromString(
        expCtx.get(), fieldPath, expCtx->variablesParseState);

    std::vector<AccumulationStatement> accumulators;
    for (const auto& fieldName : fieldsToInclude) {
        auto field = BSON(fieldName << BSON(AccumulatorFirst::kName << "$" + fieldName));
        accumulators.emplace_back(AccumulationStatement::parseAccumulationStatement(
            expCtx.get(), field.firstElement(), expCtx->variablesParseState));
    };

    auto newGroup =
        DocumentSourceGroup::create(expCtx, groupByExpr, std::move(accumulators), false);
    // The $first accumulator is compatible with SBE.
    newGroup->setSbeCompatibility(SbeCompatibility::noRequirements);

    return newGroup;
}

// Optimize the section of the pipeline before the $_internalUnpackBucket stage.
void optimizePrefix(DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    auto prefix = DocumentSourceContainer(container->begin(), itr);
    Pipeline::optimizeContainer(&prefix);
    Pipeline::optimizeEachStage(&prefix);
    container->erase(container->begin(), itr);
    container->splice(itr, prefix);
}

boost::intrusive_ptr<Expression> handleDateTruncRewrite(
    boost::intrusive_ptr<ExpressionContext> pExpCtx,
    boost::intrusive_ptr<Expression> expr,
    const std::string& timeField,
    int bucketMaxSpanSeconds) {
    if (bucketMaxSpanSeconds > 86400 /* seconds in 1 day */) {
        return {};
    }
    ExpressionDateTrunc* dateExpr = dynamic_cast<ExpressionDateTrunc*>(expr.get());
    if (!dateExpr) {
        return {};
    }
    const auto dateExprChildren = dateExpr->getChildren();
    auto datePathExpr = dynamic_cast<ExpressionFieldPath*>(dateExprChildren[0].get());
    if (!datePathExpr) {
        return {};
    }

    // The path must be at the timeField for this re-write to be correct (and the zero
    // component is always CURRENT).
    const auto& datePath = datePathExpr->getFieldPath();
    if (datePath.getPathLength() != 2 || datePath.getFieldName(1) != timeField) {
        return {};
    }

    // Validate the bucket boundaries align with the date produced by $dateTrunc. We don't
    // have access to any documents, so 'unit' and  'binSize' must be constants or can be
    // optimized to a constant. The smallest possible value for 'bucketMaxSpanSeconds' is 1 second,
    // and therefore if the 'unit' is a millisecond, the rewrite does not apply.
    boost::optional<TimeUnit> unit = dateExpr->getOptimizedUnit();
    if (!unit || unit == TimeUnit::millisecond) {
        return {};
    }
    long long dateTruncUnitInSeconds = timeUnitValueInSeconds(*unit);
    if (dateExpr->isBinSizeSpecified()) {
        auto optBin = dateExpr->getOptimizedBinSize();
        if (!optBin) {
            return {};
        }
        dateTruncUnitInSeconds = dateTruncUnitInSeconds * optBin.value();
    }

    // Confirm that the 'binSize' and 'unit' provided is a multiple of 'bucketMaxSpanSeconds',
    // to ensure the predicate aligns with the bucket boundaries.
    if (dateTruncUnitInSeconds % bucketMaxSpanSeconds) {
        return {};
    }

    if (dateExpr->isTimezoneSpecified()) {
        boost::optional<TimeZone> timezone = dateExpr->getOptimizedTimeZone();
        if (!timezone) {
            return {};
        }

        // The rewrite will result in incorrect results, if the timezone doesn't align with
        // the bucket boundaries. If the bucket's boundary is greater than one hour, measurements in
        // the same bucket might be considered in different groups for different timezones. For
        // example, two measurements might be on the same day for one timezone but on different days
        // in another timezones. So, this rewrite is restricted to buckets spanning an hour or less
        // if the timezone is specified.
        if (bucketMaxSpanSeconds > 3600 /* seconds in 1 hour */ ||
            timezone->utcOffset(Date_t::now()).count() % bucketMaxSpanSeconds) {
            return {};
        }
    }

    // If the bucket boundaries align, we should rewrite $dateTrunc to use the minimum
    // timeField stored in the control field.
    auto date = ExpressionFieldPath::createPathFromString(pExpCtx.get(),
                                                          timeseries::kControlMinFieldNamePrefix +
                                                              timeField,
                                                          pExpCtx->variablesParseState);
    return make_intrusive<ExpressionDateTrunc>(pExpCtx.get(),
                                               date,
                                               dateExprChildren[1 /*_kUnit */].get(),
                                               dateExprChildren[2 /* _kBinSize */].get(),
                                               dateExprChildren[3 /* _kTimeZone */].get(),
                                               dateExprChildren[4 /* _kStartOfWeek */].get());
}

// Returns true if all the field paths contained in the expression only reference the metaField or
// some subfield(s) of the metaField.
bool fieldPathsAccessOnlyMetaField(boost::intrusive_ptr<Expression> expr,
                                   const boost::optional<std::string>& metaField) {
    auto deps = expression::getDependencies(expr.get());
    if (deps.needWholeDocument) {
        return false;
    }

    if (!deps.fields.empty() && !metaField) {
        return false;
    }

    for (auto&& path : deps.fields) {
        FieldPath idPath(path);
        if (idPath.getPathLength() < 1 || idPath.getFieldName(0) != metaField.value()) {
            return false;
        }
    }
    return true;
}

// Rewrites all the fields paths in the given expression that contain the metafield to use
// kBucketMetaFieldName instead.
boost::intrusive_ptr<Expression> rewriteMetaFieldPaths(
    boost::intrusive_ptr<ExpressionContext> pExpCtx,
    const boost::optional<std::string>& metaField,
    boost::intrusive_ptr<Expression> expr) {
    if (!metaField) {
        return expr;
    }

    // We need to clone here to avoid corrupting the original expression if the optimization cannot
    // be made. E.g., if a subsequent group by element contains a field path that references a time
    // series field.
    auto clonedExpr = expr->clone();
    auto renameMap =
        StringMap<std::string>{{metaField.value(), std::string{timeseries::kBucketMetaFieldName}}};
    SubstituteFieldPathWalker walker{renameMap};
    auto mutatedExpr = expression_walker::walk<Expression>(clonedExpr.get(), &walker);
    if (!mutatedExpr) {
        // mutatedExpr may be null if the root of the expression tree was not updated, but
        // clonedExpr will still have been updated in-place.
        return clonedExpr;
    }

    return mutatedExpr.release();
}

boost::intrusive_ptr<Expression> rewriteGroupByElement(
    boost::intrusive_ptr<ExpressionContext> pExpCtx,
    boost::intrusive_ptr<Expression> expr,
    const timeseries::BucketUnpacker& bucketUnpacker,
    int bucketMaxSpanSeconds,
    bool fixedBuckets,
    bool usesExtendedRange) {
    // We allow the $group stage to be rewritten if the _id field only consists of these 3 options:
    // 1. If the _id field is constant.
    // 2. If the _id field is an expression whose fieldPaths are at or under the metaField.
    // 3. For a collection with fixed buckets, if the _id field is a $dateTrunc expressions on the
    // timeField.
    if (ExpressionConstant::isConstant(expr)) {
        return expr;
    }

    // Option 2: The only field path supported is at or under the metaField.
    auto metaField = bucketUnpacker.getMetaField();
    if (fieldPathsAccessOnlyMetaField(expr, metaField) && metaField &&
        bucketUnpacker.providesField(*metaField)) {
        return rewriteMetaFieldPaths(pExpCtx, metaField, expr);
    }

    // Option 3: Currently the only allowed field path not on the metaField is $dateTrunc on the
    // timeField if the buckets are fixed and do not use an extended range.
    if (fixedBuckets && !usesExtendedRange &&
        bucketUnpacker.providesField(bucketUnpacker.getTimeField())) {
        return handleDateTruncRewrite(
            pExpCtx, expr, bucketUnpacker.getTimeField(), bucketMaxSpanSeconds);
    }
    return {};
}

std::unique_ptr<AccumulationExpression> rewriteCountGroupAccm(
    ExpressionContext* pExpCtx,
    const mongo::AccumulationStatement& stmt,
    const timeseries::BucketUnpacker& bucketUnpacker) {
    // We rewrite {$sum: 1} to use the bucket level field names and account for
    // compressed/uncompressed buckets. The resulting MQL will look like:
    //$sum : {
    //  $cond: [
    //    {$gte : [ "$control.version", 2 ]},
    //    "$control.count",
    //    {$size : {$objectToArray : "$data.t"}}]}}
    // We expect most buckets to be compressed, and should just reference $control.count.
    auto exprArg = dynamic_cast<ExpressionConstant*>(stmt.expr.argument.get());
    if (!exprArg || ValueComparator::kInstance.evaluate(exprArg->getValue() != Value(1))) {
        return {};
    }

    // BucketSpec should have access to timeField to be able to do this rewrite, because version 1
    // buckets do not store an explicit count field.
    if (!bucketUnpacker.providesFieldWithoutModification(bucketUnpacker.getTimeField())) {
        return {};
    }

    std::string controlCountField = std::string{timeseries::kControlFieldNamePrefix} +
        std::string{timeseries::kBucketControlCountFieldName};

    auto ifExpr = ExpressionCompare::create(
        pExpCtx,
        ExpressionCompare::CmpOp::GTE,
        ExpressionFieldPath::createPathFromString(
            pExpCtx,
            std::string{timeseries::kControlFieldNamePrefix} +
                std::string{timeseries::kBucketControlVersionFieldName},
            pExpCtx->variablesParseState),
        ExpressionConstant::create(pExpCtx,
                                   Value(timeseries::kTimeseriesControlCompressedSortedVersion)));

    auto thenExpr = ExpressionFieldPath::createPathFromString(
        pExpCtx, controlCountField, pExpCtx->variablesParseState);

    auto elseExpr =
        BSON("$size" << BSON("$objectToArray" << "$" + timeseries::kDataFieldNamePrefix +
                                 bucketUnpacker.getTimeField()));
    auto argument = ExpressionCond::create(
        pExpCtx,
        std::move(ifExpr),
        std::move(thenExpr),
        ExpressionSize::parse(pExpCtx, elseExpr.firstElement(), pExpCtx->variablesParseState));

    auto initializer = ExpressionConstant::create(pExpCtx, Value(BSONNULL));
    return std::make_unique<AccumulationExpression>(
        initializer,
        argument,
        [pExpCtx]() { return make_intrusive<AccumulatorSum>(pExpCtx); },
        AccumulatorSum::kName);
}

std::unique_ptr<AccumulationExpression> rewriteMinMaxGroupAccm(
    boost::intrusive_ptr<ExpressionContext> pExpCtx,
    const mongo::AccumulationStatement& stmt,
    const timeseries::BucketUnpacker& bucketUnpacker) {
    const auto* exprArgPath = dynamic_cast<const ExpressionFieldPath*>(stmt.expr.argument.get());

    // This is either a const or a compound expression. While some such expressions (e.g: {$min:
    // {$add: ['$a', 2]}}) could be re-written in terms of the min/max on the control fields, in
    // general we cannot do it (e.g. {$min: {$add: ['$a', '$b']}}), so we block the re-write.
    if (!exprArgPath) {
        return {};
    }

    // Path can have a single component if it's using $$CURRENT or a similar variable. We don't
    // support these.
    const auto& path = exprArgPath->getFieldPath();
    if (path.getPathLength() <= 1) {
        return {};
    }
    const auto& accFieldName = std::string{path.getFieldName(1)};
    if (!bucketUnpacker.providesFieldWithoutModification(accFieldName)) {
        return {};
    }

    // Build the paths for the bucket-level fields.
    std::ostringstream os;
    if (bucketUnpacker.getMetaField() && accFieldName == *bucketUnpacker.getMetaField()) {
        // Update aggregates to reference the meta field.
        os << timeseries::kBucketMetaFieldName;

        for (size_t index = 2; index < path.getPathLength(); index++) {
            os << "." << path.getFieldName(index);
        }
    } else {
        // Update aggregates to reference the control field.
        const auto op = stmt.expr.name;
        if (op == "$min") {
            // Rewrite not valid for the timeField because control.min.time contains a rounded-down
            // time and not the actual min time of events in the bucket.
            if (accFieldName == bucketUnpacker.getTimeField()) {
                return {};
            }
            os << timeseries::kControlMinFieldNamePrefix;
        } else if (op == "$max") {
            // The control.max.time field is not a true max if there's extended range data. So we
            // can only perform this rewrite conditionally.
            // If we are on mongos, the extended range flag may not be accurate. If we are on
            // mongod, the flag is accurate and we can perform the rewrite if there's no extended
            // range data.
            if (pExpCtx->getInRouter() || bucketUnpacker.getUsesExtendedRange()) {
                return {};
            }
            os << timeseries::kControlMaxFieldNamePrefix;
        } else {
            MONGO_UNREACHABLE;
        }

        for (size_t index = 1; index < path.getPathLength(); index++) {
            if (index > 1) {
                os << ".";
            }
            os << path.getFieldName(index);
        }
    }

    // Re-create the accumulator using the bucket-level paths.
    const auto& newExpr = ExpressionFieldPath::createPathFromString(
        pExpCtx.get(), os.str(), pExpCtx->variablesParseState);
    AccumulationExpression accExpr = stmt.expr;
    accExpr.argument = newExpr;
    return std::make_unique<AccumulationExpression>(std::move(accExpr));
}

boost::intrusive_ptr<Expression> rewriteGroupByField(
    boost::intrusive_ptr<ExpressionContext> pExpCtx,
    const std::vector<boost::intrusive_ptr<Expression>>& idFieldExpressions,
    const std::vector<std::string>& idFieldNames,
    const timeseries::BucketUnpacker& bucketUnpacker,
    int bucketMaxSpanSeconds,
    bool fixedBuckets,
    bool usesExtendedRange) {
    tassert(7823400,
            "idFieldNames must be empty or the same size as idFieldExpressions",
            (idFieldNames.empty() && idFieldExpressions.size() == 1) ||
                idFieldNames.size() == idFieldExpressions.size());

    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> fieldsAndExprs;
    const bool isIdFieldAnExpr = idFieldNames.empty();
    for (std::size_t i = 0; i < idFieldExpressions.size(); ++i) {
        auto expr = rewriteGroupByElement(pExpCtx,
                                          idFieldExpressions[i],
                                          bucketUnpacker,
                                          bucketMaxSpanSeconds,
                                          fixedBuckets,
                                          usesExtendedRange);
        if (!expr) {
            return {};
        }
        if (isIdFieldAnExpr) {
            return expr;
        }
        fieldsAndExprs.emplace_back(idFieldNames[i], expr);
    }
    return ExpressionObject::create(pExpCtx.get(), std::move(fieldsAndExprs));
}

}  // namespace

DocumentSourceInternalUnpackBucket::DocumentSourceInternalUnpackBucket(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BucketUnpacker bucketUnpacker,
    int bucketMaxSpanSeconds,
    bool assumeNoMixedSchemaData,
    bool fixedBuckets,
    boost::optional<bool> sbeCompatible)
    : DocumentSource(kStageNameInternal, expCtx),
      _assumeNoMixedSchemaData(assumeNoMixedSchemaData),
      _fixedBuckets(fixedBuckets),
      _bucketMaxSpanSeconds{bucketMaxSpanSeconds},
      _sharedState(std::make_shared<InternalUnpackBucketSharedState>(
          InternalUnpackBucketSharedState{._bucketUnpacker = std::move(bucketUnpacker)})),
      _isSbeCompatible(sbeCompatible) {}

DocumentSourceInternalUnpackBucket::DocumentSourceInternalUnpackBucket(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BucketUnpacker bucketUnpacker,
    int bucketMaxSpanSeconds,
    const boost::optional<BSONObj>& eventFilterBson,
    const boost::optional<BSONObj>& wholeBucketFilterBson,
    bool assumeNoMixedSchemaData,
    bool fixedBuckets,
    boost::optional<bool> sbeCompatible)
    : DocumentSourceInternalUnpackBucket(expCtx,
                                         std::move(bucketUnpacker),
                                         bucketMaxSpanSeconds,
                                         assumeNoMixedSchemaData,
                                         fixedBuckets,
                                         sbeCompatible) {
    if (eventFilterBson) {
        // Optimizing '_eventFilter' here duplicates predicates in $expr expressions.
        // TODO SERVER-79692 remove the 'shouldOptimize' boolean.
        setEventFilter(eventFilterBson.get(), false /* shouldOptimize */);
    }
    if (wholeBucketFilterBson) {
        _wholeBucketFilterBson = wholeBucketFilterBson->getOwned();
        _sharedState->_wholeBucketFilter =
            uassertStatusOK(MatchExpressionParser::parse(_wholeBucketFilterBson,
                                                         getExpCtx(),
                                                         ExtensionsCallbackNoop(),
                                                         Pipeline::kAllowedMatcherFeatures));
    }
}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalUnpackBucket::createFromBsonInternal(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5346500,
            str::stream() << "$_internalUnpackBucket specification must be an object, got: "
                          << specElem.type(),
            specElem.type() == BSONType::object);

    // If neither "include" nor "exclude" is specified, the default is "exclude": [] and
    // if that's the case, no field will be added to 'bucketSpec.fieldSet' in the for-loop below.
    BucketSpec bucketSpec;
    // Use extended-range support if any individual collection requires it, even if 'specElem'
    // doesn't mention this flag.
    if (expCtx->getRequiresTimeseriesExtendedRangeSupport()) {
        bucketSpec.setUsesExtendedRange(true);
    }
    auto hasIncludeExclude = false;
    auto hasTimeField = false;
    auto hasBucketMaxSpanSeconds = false;
    auto bucketMaxSpanSeconds = 0;
    auto assumeClean = false;
    bool fixedBuckets = false;
    boost::optional<bool> sbeCompatible = boost::none;
    std::vector<std::string> computedMetaProjFields;
    boost::optional<BSONObj> eventFilterBson;
    boost::optional<BSONObj> wholeBucketFilterBson;
    for (auto&& elem : specElem.embeddedObject()) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == kInclude || fieldName == kExclude) {
            uassert(5408000,
                    "The $_internalUnpackBucket stage expects at most one of include/exclude "
                    "parameters to be specified",
                    !hasIncludeExclude);

            uassert(5346501,
                    str::stream() << "include or exclude field must be an array, got: "
                                  << elem.type(),
                    elem.type() == BSONType::array);

            for (auto&& elt : elem.embeddedObject()) {
                uassert(5346502,
                        str::stream() << "include or exclude field element must be a string, got: "
                                      << elt.type(),
                        elt.type() == BSONType::string);
                auto field = elt.valueStringData();
                uassert(5346503,
                        "include or exclude field element must be a single-element field path",
                        field.find('.') == std::string::npos);
                uassert(9568705,
                        "include or exclude field element must not contain an embedded null byte",
                        field.find('\0') == std::string::npos);
                bucketSpec.addIncludeExcludeField(field);
            }
            bucketSpec.setBehavior(fieldName == kInclude ? BucketSpec::Behavior::kInclude
                                                         : BucketSpec::Behavior::kExclude);
            hasIncludeExclude = true;
        } else if (fieldName == kAssumeNoMixedSchemaData) {
            uassert(6067202,
                    str::stream() << "assumeClean field must be a bool, got: " << elem.type(),
                    elem.type() == BSONType::boolean);
            assumeClean = elem.boolean();
        } else if (fieldName == timeseries::kTimeFieldName) {
            uassert(5346504,
                    str::stream() << "timeField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::string);
            auto timeField = elem.str();
            uassert(9568701,
                    str::stream() << "timeField must not contain an embedded null byte",
                    timeField.find('\0') == std::string::npos);
            bucketSpec.setTimeField(std::move(timeField));
            hasTimeField = true;
        } else if (fieldName == timeseries::kMetaFieldName) {
            uassert(5346505,
                    str::stream() << "metaField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::string);
            auto metaField = elem.str();
            uassert(5545700,
                    str::stream() << "metaField field must be a single-element field path",
                    metaField.find('.') == std::string::npos);
            uassert(9568702,
                    str::stream() << "metaField field must not contain an embedded null byte",
                    metaField.find('\0') == std::string::npos);
            bucketSpec.setMetaField(std::move(metaField));
        } else if (fieldName == kBucketMaxSpanSeconds) {
            uassert(5510600,
                    str::stream() << "bucketMaxSpanSeconds field must be an integer, got: "
                                  << elem.type(),
                    elem.type() == BSONType::numberInt);
            uassert(5510601,
                    "bucketMaxSpanSeconds field must be greater than zero",
                    elem._numberInt() > 0);
            bucketMaxSpanSeconds = elem._numberInt();
            hasBucketMaxSpanSeconds = true;
        } else if (fieldName == "computedMetaProjFields") {
            uassert(5509900,
                    str::stream() << "computedMetaProjFields field must be an array, got: "
                                  << elem.type(),
                    elem.type() == BSONType::array);

            for (auto&& elt : elem.embeddedObject()) {
                uassert(5509901,
                        str::stream()
                            << "computedMetaProjFields field element must be a string, got: "
                            << elt.type(),
                        elt.type() == BSONType::string);
                auto field = elt.valueStringData();
                uassert(5509902,
                        "computedMetaProjFields field element must be a single-element field path",
                        field.find('.') == std::string::npos);
                uassert(9568706,
                        "computedMetaProjFields field must not contain an embedded null byte",
                        field.find('\0') == std::string::npos);
                bucketSpec.addComputedMetaProjFields(field);
            }
        } else if (fieldName == kIncludeMinTimeAsMetadata) {
            uassert(6460208,
                    str::stream() << kIncludeMinTimeAsMetadata
                                  << " field must be a bool, got: " << elem.type(),
                    elem.type() == BSONType::boolean);
            bucketSpec.includeMinTimeAsMetadata = elem.boolean();
        } else if (fieldName == kIncludeMaxTimeAsMetadata) {
            uassert(6460209,
                    str::stream() << kIncludeMaxTimeAsMetadata
                                  << " field must be a bool, got: " << elem.type(),
                    elem.type() == BSONType::boolean);
            bucketSpec.includeMaxTimeAsMetadata = elem.boolean();
        } else if (fieldName == kUsesExtendedRange) {
            uassert(6646901,
                    str::stream() << kUsesExtendedRange
                                  << " field must be a bool, got: " << elem.type(),
                    elem.type() == BSONType::boolean);
            bucketSpec.setUsesExtendedRange(elem.boolean());
        } else if (fieldName == kEventFilter) {
            uassert(7026902,
                    str::stream() << kEventFilter
                                  << " field must be an object, got: " << elem.type(),
                    elem.type() == BSONType::object);
            eventFilterBson = elem.Obj();
        } else if (fieldName == kWholeBucketFilter) {
            uassert(7026903,
                    str::stream() << kWholeBucketFilter
                                  << " field must be an object, got: " << elem.type(),
                    elem.type() == BSONType::object);
            wholeBucketFilterBson = elem.Obj();
        } else if (fieldName == kFixedBuckets) {
            uassert(7823300,
                    str::stream() << kFixedBuckets << " field must be a bool, got: " << elem.type(),
                    elem.type() == BSONType::boolean);
            fixedBuckets = elem.boolean();
        } else if (fieldName == kSbeCompatible) {
            uassert(8796100,
                    str::stream() << kSbeCompatible
                                  << " field must be a bool, got: " << elem.type(),
                    elem.type() == BSONType::boolean);
            if (elem.boolean() == false) {
                expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
                sbeCompatible = false;
            }
        } else {
            uasserted(5346506,
                      str::stream()
                          << "unrecognized parameter to $_internalUnpackBucket: " << fieldName);
        }
    }

    uassert(
        5346508, "The $_internalUnpackBucket stage requires a timeField parameter", hasTimeField);

    uassert(5510602,
            "The $_internalUnpackBucket stage requires a bucketMaxSpanSeconds parameter",
            hasBucketMaxSpanSeconds);

    return make_intrusive<DocumentSourceInternalUnpackBucket>(expCtx,
                                                              BucketUnpacker{std::move(bucketSpec)},
                                                              bucketMaxSpanSeconds,
                                                              eventFilterBson,
                                                              wholeBucketFilterBson,
                                                              assumeClean,
                                                              fixedBuckets,
                                                              sbeCompatible);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalUnpackBucket::createFromBsonExternal(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5612400,
            str::stream() << "$_unpackBucket specification must be an object, got: "
                          << specElem.type(),
            specElem.type() == BSONType::object);

    BucketSpec bucketSpec;
    auto hasTimeField = false;
    auto assumeClean = false;
    for (auto&& elem : specElem.embeddedObject()) {
        auto fieldName = elem.fieldNameStringData();
        // We only expose "timeField" and "metaField" as parameters in $_unpackBucket.
        if (fieldName == timeseries::kTimeFieldName) {
            auto timeField = elem.str();
            uassert(5612401,
                    str::stream() << "timeField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::string);
            uassert(9568703,
                    str::stream() << "timeField must not contain an embedded null byte",
                    timeField.find('\0') == std::string::npos);
            bucketSpec.setTimeField(std::move(timeField));
            hasTimeField = true;
        } else if (fieldName == timeseries::kMetaFieldName) {
            uassert(5612402,
                    str::stream() << "metaField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::string);
            auto metaField = elem.str();
            uassert(5612403,
                    str::stream() << "metaField field must be a single-element field path",
                    metaField.find('.') == std::string::npos);
            uassert(9568704,
                    str::stream() << "metaField field must not contain an embedded null byte",
                    metaField.find('\0') == std::string::npos);
            bucketSpec.setMetaField(std::move(metaField));
        } else if (fieldName == kAssumeNoMixedSchemaData) {
            uassert(6067203,
                    str::stream() << "assumeClean field must be a bool, got: " << elem.type(),
                    elem.type() == BSONType::boolean);
            assumeClean = elem.boolean();
        } else {
            uasserted(5612404,
                      str::stream() << "unrecognized parameter to $_unpackBucket: " << fieldName);
        }
    }
    uassert(5612405,
            str::stream() << "The $_unpackBucket stage requires a timeField parameter",
            hasTimeField);

    return make_intrusive<DocumentSourceInternalUnpackBucket>(
        expCtx,
        BucketUnpacker{std::move(bucketSpec)},
        // Using the bucket span associated with the default granularity seconds.
        timeseries::getMaxSpanSecondsFromGranularity(BucketGranularityEnum::Seconds),
        assumeClean,
        false /* fixedBuckets */);
}

void DocumentSourceInternalUnpackBucket::serializeToArray(std::vector<Value>& array,
                                                          const SerializationOptions& opts) const {
    auto explain = opts.verbosity;

    MutableDocument out;
    auto behavior = _sharedState->_bucketUnpacker.behavior() == BucketSpec::Behavior::kInclude
        ? kInclude
        : kExclude;
    const auto& spec = _sharedState->_bucketUnpacker.bucketSpec();
    std::vector<Value> fields;
    for (auto&& field : spec.fieldSet()) {
        fields.emplace_back(opts.serializeFieldPathFromString(field));
    }
    if (_sharedState->_bucketUnpacker.removedMetaFieldFromFieldSet() &&
        std::find(spec.computedMetaProjFields().cbegin(),
                  spec.computedMetaProjFields().cend(),
                  *_sharedState->_bucketUnpacker.getMetaField()) ==
            spec.computedMetaProjFields().cend())
        fields.emplace_back(
            opts.serializeFieldPathFromString(*_sharedState->_bucketUnpacker.getMetaField()));

    out.addField(behavior, Value{std::move(fields)});
    out.addField(
        timeseries::kTimeFieldName,
        Value{opts.serializeFieldPathFromString(_sharedState->_bucketUnpacker.getTimeField())});
    if (_sharedState->_bucketUnpacker.getMetaField()) {
        out.addField(timeseries::kMetaFieldName,
                     Value{opts.serializeFieldPathFromString(
                         *_sharedState->_bucketUnpacker.getMetaField())});
    }
    out.addField(kBucketMaxSpanSeconds, opts.serializeLiteral(Value{_bucketMaxSpanSeconds}));
    if (_assumeNoMixedSchemaData)
        out.addField(kAssumeNoMixedSchemaData,
                     opts.serializeLiteral(Value(_assumeNoMixedSchemaData)));

    if (_sharedState->_bucketUnpacker.getUsesExtendedRange()) {
        // Include this flag so that 'explain' is more helpful.
        // But this is not so useful for communicating from one process to another,
        // because mongos and/or the primary shard don't know whether any other shard
        // has extended-range data.
        out.addField(kUsesExtendedRange, opts.serializeLiteral(Value{true}));
    }

    if (!spec.computedMetaProjFields().empty())
        out.addField("computedMetaProjFields", Value{[&] {
                         std::vector<Value> compFields;
                         std::transform(spec.computedMetaProjFields().cbegin(),
                                        spec.computedMetaProjFields().cend(),
                                        std::back_inserter(compFields),
                                        [opts](auto&& projString) {
                                            return Value{
                                                opts.serializeFieldPathFromString(projString)};
                                        });
                         return compFields;
                     }()});

    if (_sharedState->_bucketUnpacker.includeMinTimeAsMetadata()) {
        out.addField(
            kIncludeMinTimeAsMetadata,
            opts.serializeLiteral(Value{_sharedState->_bucketUnpacker.includeMinTimeAsMetadata()}));
    }
    if (_sharedState->_bucketUnpacker.includeMaxTimeAsMetadata()) {
        out.addField(
            kIncludeMaxTimeAsMetadata,
            opts.serializeLiteral(Value{_sharedState->_bucketUnpacker.includeMaxTimeAsMetadata()}));
    }

    if (_sharedState->_wholeBucketFilter) {
        out.addField(kWholeBucketFilter, Value{_sharedState->_wholeBucketFilter->serialize(opts)});
    }
    if (_sharedState->_eventFilter) {
        out.addField(kEventFilter, Value{_sharedState->_eventFilter->serialize(opts)});
    }

    if (_fixedBuckets) {
        out.addField(kFixedBuckets, opts.serializeLiteral(Value(_fixedBuckets)));
    }

    if (_isSbeCompatible && *_isSbeCompatible == false) {
        out.addField(kSbeCompatible, Value(false));
    }

    if (!explain) {
        array.push_back(Value(DOC(getSourceName() << out.freeze())));
        if (_sampleSize) {
            auto sampleSrc = DocumentSourceSample::create(getExpCtx(), *_sampleSize);
            sampleSrc->serializeToArray(array, opts);
        }
    } else {
        if (_sampleSize) {
            out.addField("sample",
                         opts.serializeLiteral(Value{static_cast<long long>(*_sampleSize)}));
            out.addField("bucketMaxCount", opts.serializeLiteral(Value{_bucketMaxCount}));
        }
        array.push_back(Value(DOC(getSourceName() << out.freeze())));
    }
}

boost::optional<DocumentSourceContainer::iterator>
DocumentSourceInternalUnpackBucket::pushDownComputedMetaProjection(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    if (_sharedState->_eventFilter || std::next(itr) == container->end()) {
        return boost::none;
    }
    if (!_sharedState->_bucketUnpacker.getMetaField() ||
        !_sharedState->_bucketUnpacker.includeMetaField() ||
        !_sharedState->_bucketUnpacker.bucketSpec().computedMetaProjFields().empty()) {
        return boost::none;
    }

    auto nextTransform =
        dynamic_cast<DocumentSourceSingleDocumentTransformation*>(std::next(itr)->get());
    if (!nextTransform) {
        return boost::none;
    }

    if (auto transformType = nextTransform->getTransformerType();
        (transformType != TransformerInterface::TransformerType::kInclusionProjection &&
         transformType != TransformerInterface::TransformerType::kComputedProjection)) {
        return boost::none;
    }

    // We should remember which $project / $addFields we have applied this optimization to and don't
    // reapply this optimization as this method makes the optimization process restart at the newly
    // pushed down stage which is inserted before $_internalUnpackBucket. Otherwise, an infinite
    // optimization loop will be formed.
    if (std::find(_triedComputedMetaPushDownFor.begin(),
                  _triedComputedMetaPushDownFor.end(),
                  nextTransform) != _triedComputedMetaPushDownFor.end()) {
        return boost::none;
    }
    _triedComputedMetaPushDownFor.push_back(nextTransform);

    auto& metaName = _sharedState->_bucketUnpacker.getMetaField().value();
    auto [addFieldsSpec, deleteStage] =
        nextTransform->extractComputedProjections(metaName,
                                                  std::string{timeseries::kBucketMetaFieldName},
                                                  BucketUnpacker::reservedBucketFieldNames);
    if (addFieldsSpec.isEmpty()) {
        return boost::none;
    }

    // Extend bucket specification of this stage to include the computed meta projections that are
    // passed through.
    std::vector<StringData> computedMetaProjFields;
    for (auto&& elem : addFieldsSpec) {
        // If the added field name is same as 'meta', it should be treated as if it's the new 'meta'
        // since it shadows the original 'meta' and so it should not be considered as "computed".
        // Otherwise, it would disable several optimizations that needs the condition of no computed
        // meta.
        if (elem.fieldName() != timeseries::kBucketMetaFieldName) {
            computedMetaProjFields.emplace_back(elem.fieldName());
        }
    }

    _sharedState->_bucketUnpacker.addComputedMetaProjFields(computedMetaProjFields);
    // Insert extracted computed projections before the $_internalUnpackBucket.
    container->insert(itr,
                      DocumentSourceAddFields::createFromBson(
                          BSON("$addFields" << addFieldsSpec).firstElement(), getExpCtx()));
    // Remove the next stage if it became empty after the field extraction.
    if (deleteStage) {
        container->erase(std::next(itr));
        return std::prev(itr) == container->begin() ? std::prev(itr) : std::prev(std::prev(itr));
    }
    return std::prev(itr);
}

void DocumentSourceInternalUnpackBucket::setEventFilter(BSONObj eventFilterBson,
                                                        bool shouldOptimize) {
    _eventFilterBson = eventFilterBson.getOwned();

    // Having an '_eventFilter' might make the unpack stage incompatible with SBE. Rather
    // than tracking the specific exprs, we temporarily reset the context to be fully SBE
    // compatible and check after parsing if the '_eventFilter' made the unpack stage
    // incompatible.
    auto originalSbeCompatibility = getExpCtx()->getSbeCompatibility();
    getExpCtx()->setSbeCompatibility(SbeCompatibility::noRequirements);

    _sharedState->_eventFilter =
        uassertStatusOK(MatchExpressionParser::parse(_eventFilterBson,
                                                     getExpCtx(),
                                                     ExtensionsCallbackNoop(),
                                                     Pipeline::kAllowedMatcherFeatures));
    if (shouldOptimize) {
        _sharedState->_eventFilter = optimizeMatchExpression(std::move(_sharedState->_eventFilter),
                                                             /* enableSimplification */ false);
    }
    _isEventFilterSbeCompatible.emplace(getExpCtx()->getSbeCompatibility());

    // Reset the sbeCompatibility taking _eventFilter into account.
    getExpCtx()->setSbeCompatibility(
        std::min(originalSbeCompatibility, _isEventFilterSbeCompatible.get()));

    _eventFilterDeps = DepsTracker();
    dependency_analysis::addDependencies(_sharedState->_eventFilter.get(), &_eventFilterDeps);
}

void DocumentSourceInternalUnpackBucket::internalizeProject(const BSONObj& project,
                                                            bool isInclusion) {
    // 'fields' are the top-level fields to be included/excluded by the unpacker. We handle the
    // special case of _id, which may be excluded in an inclusion $project (or vice versa), here.
    auto fields = project.getFieldNames<std::set<std::string>>();
    if (auto elt = project.getField("_id"); (elt.isBoolean() && elt.Bool() != isInclusion) ||
        (elt.isNumber() && (elt.Int() == 1) != isInclusion)) {
        fields.erase("_id");
    }

    // Update '_bucketUnpacker' state with the new fields and behavior.
    auto spec = _sharedState->_bucketUnpacker.bucketSpec();
    spec.setFieldSet(std::move(fields));
    spec.setBehavior(isInclusion ? BucketSpec::Behavior::kInclude : BucketSpec::Behavior::kExclude);
    _sharedState->_bucketUnpacker.setBucketSpec(std::move(spec));
}

std::pair<BSONObj, bool> DocumentSourceInternalUnpackBucket::extractOrBuildProjectToInternalize(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) const {
    if (std::next(itr) == container->end() ||
        _sharedState->_bucketUnpacker.hasIncludeExcludeFields()) {
        // There is no project to internalize or there are already fields being included/excluded,
        // which means we've already internalized a project.
        return {BSONObj{}, false};
    }

    // Check for a viable inclusion $project after the $_internalUnpackBucket.
    // Note: an $_internalUnpackBucket stage with a set of 'kInclude' fields and an event filter is
    // equivalent to [{$_internalUnpackBucket}{$project}{$match}] -- in _this_ order. That is, if
    // the $match is on a field that is not included by $project, the result must be an empty set.
    // But if the stage already has an '_eventFilter', it means we started with pipeline like
    // [{$_internalUnpackBucket}{$match}{$project}], which might return non-empty set of results, so
    // we cannot internalize the $project.
    auto [existingProj, isInclusion] = getIncludeExcludeProjectAndType(std::next(itr)->get());
    if (!_sharedState->_eventFilter && isInclusion && !existingProj.isEmpty() &&
        canInternalizeProjectObj(existingProj)) {
        container->erase(std::next(itr));
        return {existingProj, isInclusion};
    }

    // Attempt to get an inclusion $project representing the root-level dependencies of the pipeline
    // after the $_internalUnpackBucket. If this $project is not empty, then the dependency set was
    // finite.
    auto deps = getRestPipelineDependencies(itr, container, true /* includeEventFilter */);
    if (auto dependencyProj =
            deps.toProjectionWithoutMetadata(DepsTracker::TruncateToRootLevel::yes);
        !dependencyProj.isEmpty()) {
        return {dependencyProj, true};
    }

    // Check for a viable exclusion $project after the $_internalUnpackBucket.
    if (!_sharedState->_eventFilter && !existingProj.isEmpty() &&
        canInternalizeProjectObj(existingProj)) {
        container->erase(std::next(itr));
        return {existingProj, isInclusion};
    }

    return {BSONObj{}, false};
}

BucketSpec::BucketPredicate DocumentSourceInternalUnpackBucket::createPredicatesOnBucketLevelField(
    const MatchExpression* matchExpr) const {
    return BucketSpec::createPredicatesOnBucketLevelField(
        matchExpr,
        _sharedState->_bucketUnpacker.bucketSpec(),
        _bucketMaxSpanSeconds,
        getExpCtx(),
        haveComputedMetaField(),
        _sharedState->_bucketUnpacker.includeMetaField(),
        _assumeNoMixedSchemaData,
        BucketSpec::IneligiblePredicatePolicy::kIgnore,
        _fixedBuckets);
}

bool DocumentSourceInternalUnpackBucket::generateBucketLevelIdPredicates(
    MatchExpression* matchExpr) const {
    return BucketSpec::generateBucketLevelIdPredicates(
        *getExpCtx(), _sharedState->_bucketUnpacker.bucketSpec(), _bucketMaxSpanSeconds, matchExpr);
}

std::pair<BSONObj, bool> DocumentSourceInternalUnpackBucket::extractProjectForPushDown(
    DocumentSource* src) const {
    if (auto nextProject = dynamic_cast<DocumentSourceSingleDocumentTransformation*>(src);
        _sharedState->_bucketUnpacker.getMetaField() && nextProject &&
        nextProject->getTransformerType() ==
            TransformerInterface::TransformerType::kExclusionProjection) {
        return nextProject->extractProjectOnFieldAndRename(
            _sharedState->_bucketUnpacker.getMetaField().value(), timeseries::kBucketMetaFieldName);
    }

    return {BSONObj{}, false};
}

std::pair<bool, DocumentSourceContainer::iterator>
DocumentSourceInternalUnpackBucket::rewriteGroupStage(DocumentSourceContainer::iterator itr,
                                                      DocumentSourceContainer* container) {
    // Rewriting a group might make it incompatible with SBE (e.g. if the rewrite is using
    // accumulator exprs that are not supported in SBE yet). Rather than tracking the specific
    // exprs, we temporarily reset the context to be fully SBE compatible and check later if any of
    // the exprs created by the rewrite mark it as incompatible, so that we can transfer the flag
    // onto the group.
    const SbeCompatibility origSbeCompat = getExpCtx()->getSbeCompatibility();
    getExpCtx()->setSbeCompatibility(SbeCompatibility::noRequirements);

    // We destruct 'this' object when we replace it with the new group, so the guard has to capture
    // the context's intrusive pointer by value.
    const ScopeGuard guard([=, ctx = getExpCtx()] {
        ctx->setSbeCompatibility(std::min(origSbeCompat, ctx->getSbeCompatibility()));
    });

    // The computed min/max for each bucket uses the default collation. If the collation of the
    // query doesn't match the default we cannot rely on the computed values as they might differ
    // (e.g. numeric and lexicographic collations compare "5" and "10" in opposite order).
    // NB: Unfortuntealy, this means we have to forgo the optimization even if the source field is
    // numeric and not affected by the collation as we cannot know the data type until runtime.
    if (getExpCtx()->getCollationMatchesDefault() ==
        ExpressionContextCollationMatchesDefault::kNo) {
        return {};
    }

    const auto* groupPtr = dynamic_cast<DocumentSourceGroup*>(std::next(itr)->get());
    if (groupPtr == nullptr) {
        return {};
    }

    const auto& idFieldExpressions = groupPtr->getIdExpressions();
    const auto& idFieldNames = groupPtr->getIdFieldNames();
    auto rewrittenIdExpression = rewriteGroupByField(getExpCtx(),
                                                     idFieldExpressions,
                                                     idFieldNames,
                                                     _sharedState->_bucketUnpacker,
                                                     _bucketMaxSpanSeconds,
                                                     _fixedBuckets,
                                                     _usesExtendedRange);
    if (!rewrittenIdExpression) {
        return {};
    }

    std::vector<AccumulationStatement> accumulationStatementsBucket;
    for (const AccumulationStatement& stmt : groupPtr->getAccumulationStatements()) {
        const auto& op = stmt.expr.name;
        // If _any_ of the accumulators aren't $min/$max/$count we won't perform the re-write (some
        // other accs might be re-writable in terms of the bucket controls, we just haven't invested
        // into implementing them). $count is desugared to {$sum: 1}.
        std::unique_ptr<AccumulationExpression> accExpr;
        if (op == "$min" || op == "$max") {
            accExpr = rewriteMinMaxGroupAccm(getExpCtx(), stmt, _sharedState->_bucketUnpacker);
            if (!accExpr) {
                return {};
            }
        } else if (op == "$sum") {
            accExpr = rewriteCountGroupAccm(getExpCtx().get(), stmt, _sharedState->_bucketUnpacker);
            if (!accExpr) {
                return {};
            }
        } else {
            return {};
        }
        accumulationStatementsBucket.emplace_back(stmt.fieldName, std::move(*accExpr));
    }

    boost::intrusive_ptr<mongo::DocumentSourceGroup> newGroup =
        DocumentSourceGroup::create(getExpCtx(),
                                    rewrittenIdExpression,
                                    std::move(accumulationStatementsBucket),
                                    groupPtr->willBeMerged(),
                                    groupPtr->getMaxMemoryUsageBytes());

    // The exprs used in the rewritten group might or might not be supported by SBE, so we have to
    // transfer the state from the expr context onto the group.
    newGroup->setSbeCompatibility(getExpCtx()->getSbeCompatibility());

    // Replace the current stage (DocumentSourceInternalUnpackBucket) and the following group stage
    // with the new group.
    container->erase(std::next(itr));
    *itr = std::move(newGroup);
    // NB: Below this point any access to members of 'this' is invalid!

    if (itr == container->begin()) {
        // Optimize the new group stage.
        return {true, itr};
    } else {
        // Give chance to the previous stage to optimize against the new group stage.
        return {true, std::prev(itr)};
    }
}

bool DocumentSourceInternalUnpackBucket::haveComputedMetaField() const {
    return _sharedState->_bucketUnpacker.getMetaField() &&
        _sharedState->_bucketUnpacker.bucketSpec().fieldIsComputed(
            _sharedState->_bucketUnpacker.getMetaField().value());
}

bool DocumentSourceInternalUnpackBucket::enableStreamingGroupIfPossible(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    // skip unpack stage
    itr = std::next(itr);

    FieldPath timeField = _sharedState->_bucketUnpacker.getTimeField();
    DocumentSourceGroup* groupStage = nullptr;
    bool isSortedOnTime = false;
    for (; itr != container->end(); ++itr) {
        if (auto groupStagePtr = dynamic_cast<DocumentSourceGroup*>(itr->get())) {
            groupStage = groupStagePtr;
            break;
        }
        if (auto sortStagePtr = dynamic_cast<DocumentSourceSort*>(itr->get())) {
            isSortedOnTime = sortStagePtr->getSortKeyPattern().front().fieldPath == timeField;
        } else if (!itr->get()->constraints().preservesOrderAndMetadata) {
            // If this is after the sort, the sort is invalidated. If it's before the sort, there's
            // no harm in keeping the boolean false.
            isSortedOnTime = false;
        }
        // We modify time field, so we can't proceed with optimization. It may be possible to
        // proceed in some cases if the modification happens before the sort, but we won't worry
        // about or bother with those - in large part because it is risky that it will change the
        // type away from a date into something with more difficult/subtle semantics.
        if (itr->get()->getModifiedPaths().canModify(timeField)) {
            return false;
        }
    }

    if (groupStage == nullptr || !isSortedOnTime) {
        return false;
    }

    auto& idFields = groupStage->getMutableIdFields();
    std::vector<size_t> monotonicIdFields;
    for (size_t i = 0; i < idFields.size(); ++i) {
        // To enable streaming, we need id field expression to be clustered, so that all documents
        // with the same value of this id field are in a single continious cluster. However this
        // property is hard to check for, so we check for monotonicity instead, which is stronger.
        idFields[i] = idFields[i]->optimize();  // We optimize here to make use of constant folding.
        auto monotonicState = idFields[i]->getMonotonicState(timeField);

        // We don't add monotonic::State::Constant id fields, because they are useless when
        // determining if a group batch is finished.
        if (monotonicState == monotonic::State::Increasing ||
            monotonicState == monotonic::State::Decreasing) {
            monotonicIdFields.push_back(i);
        }
    }
    if (monotonicIdFields.empty()) {
        return false;
    }

    *itr = DocumentSourceStreamingGroup::create(
        getExpCtx(),
        groupStage->getIdExpression(),
        std::move(monotonicIdFields),
        std::move(groupStage->getMutableAccumulationStatements()),
        groupStage->getMaxMemoryUsageBytes());

    // Streaming group isn't supported in SBE yet and we don't want to run the pipeline in hybrid
    // mode due to potential perf impact.
    getExpCtx()->setSbePipelineCompatibility(SbeCompatibility::notCompatible);
    _isSbeCompatible = false;

    return true;
}

namespace {
template <TopBottomSense sense, bool single>
bool extractFromAcc(const AccumulatorN* acc,
                    const boost::intrusive_ptr<Expression>& init,
                    boost::optional<BSONObj>& outputAccumulator,
                    boost::optional<BSONObj>& outputSortPattern) {
    // If this accumulator will not return a single document then we cannot rewrite this query to
    // use a $sort + a $group with $first or $last.
    if constexpr (!single) {
        // We may have a $topN or a $bottomN with n = 1; in this case, we may still be able to
        // perform the lastpoint rewrite.
        if (auto constInit = dynamic_cast<ExpressionConstant*>(init.get()); constInit) {
            // Since this is a $const expression, the input to evaluate() should not matter.
            auto constVal = constInit->evaluate(Document(), nullptr);
            if (!constVal.numeric() || (constVal.coerceToLong() != 1)) {
                return false;
            }
        } else {
            return false;
        }
    }

    // Retrieve sort pattern for an equivalent $sort.
    const auto multiAc = dynamic_cast<const AccumulatorTopBottomN<sense, single>*>(acc);
    invariant(multiAc);
    outputSortPattern = multiAc->getSortPattern()
                            .serialize(SortPattern::SortKeySerialization::kForPipelineSerialization)
                            .toBson();

    // Retrieve equivalent accumulator statement using $first/$last for retrieving the entire
    // document.
    constexpr auto accumulator =
        (sense == TopBottomSense::kTop) ? AccumulatorFirst::kName : AccumulatorLast::kName;
    // Note: we don't need to preserve what the $top/$bottom accumulator outputs here. We only need
    // a $group stage with the appropriate accumulator that retrieves the bucket in some way. For
    // the rewrite we preserve the original group and insert an $group that returns all the data in
    // the $first bucket selected for each _id.
    outputAccumulator = BSON("bucket" << BSON(accumulator << "$$ROOT"));

    return true;
}

bool extractFromAccIfTopBottomN(const AccumulatorN* multiAcc,
                                const boost::intrusive_ptr<Expression>& init,
                                boost::optional<BSONObj>& outputAccumulator,
                                boost::optional<BSONObj>& outputSortPattern) {
    const auto accType = multiAcc->getAccumulatorType();
    if (accType == AccumulatorN::kTopN) {
        return extractFromAcc<TopBottomSense::kTop, false /* single */>(
            multiAcc, init, outputAccumulator, outputSortPattern);
    } else if (accType == AccumulatorN::kTop) {
        return extractFromAcc<TopBottomSense::kTop, true /* single */>(
            multiAcc, init, outputAccumulator, outputSortPattern);
    } else if (accType == AccumulatorN::kBottomN) {
        return extractFromAcc<TopBottomSense::kBottom, false /* single */>(
            multiAcc, init, outputAccumulator, outputSortPattern);
    } else if (accType == AccumulatorN::kBottom) {
        return extractFromAcc<TopBottomSense::kBottom, true /* single */>(
            multiAcc, init, outputAccumulator, outputSortPattern);
    }
    // This isn't a topN/bottomN/top/bottom accumulator.
    return false;
}

// The lastpoint optimization ultimately inserts bucket-level $sort + $group to limit the amount of
// data to be unpacked. Here we use the original $group to create a matching $sort followed by a
// $group with $first/$last accumulator over $$ROOT as if it ran _after_ unpacking. We'll modify
// these stages to run at the bucket-level later.
std::pair<boost::intrusive_ptr<DocumentSourceSort>, boost::intrusive_ptr<DocumentSourceGroup>>
tryCreateBucketLevelSortGroup(boost::intrusive_ptr<ExpressionContext> expCtx,
                              DocumentSourceContainer::iterator itr,
                              DocumentSourceContainer* container,
                              DocumentSourceGroup* groupStage) {
    const auto accumulators = groupStage->getAccumulationStatements();
    if (accumulators.size() != 1) {
        // If we have multiple accumulators, we fail to optimize for a lastpoint query. Notice, that
        // this is too strict. The optimization can still be done for the same type of acc with
        // the same ordering. However, for this case the query can be re-written to combine the two
        // accumulators into one $top/$bottom with a concatenated 'output' array.
        return {nullptr, nullptr};
    }

    const auto init = accumulators[0].expr.initializer;
    const auto accState = accumulators[0].makeAccumulator();
    const AccumulatorN* multiAcc = dynamic_cast<const AccumulatorN*>(accState.get());
    if (!multiAcc) {
        return {nullptr, nullptr};
    }

    boost::optional<BSONObj> maybeAcc;
    boost::optional<BSONObj> maybeSortPattern;
    if (!extractFromAccIfTopBottomN(multiAcc, init, maybeAcc, maybeSortPattern)) {
        // This isn't a topN/bottomN/top/bottom accumulator or N != 1.
        return {nullptr, nullptr};
    }

    tassert(6165600,
            "sort pattern and accumulator must be initialized if cast of $top or $bottom succeeds",
            maybeSortPattern && maybeAcc);

    auto newSortStage = DocumentSourceSort::create(expCtx, SortPattern(*maybeSortPattern, expCtx));
    auto newAccState = AccumulationStatement::parseAccumulationStatement(
        expCtx.get(), maybeAcc->firstElement(), expCtx->variablesParseState);
    auto newGroupStage = DocumentSourceGroup::create(
        expCtx, groupStage->getIdExpression(), {newAccState}, groupStage->willBeMerged());

    // The bucket-level group uses $first/$last accumulators that are supported by SBE.
    newGroupStage->setSbeCompatibility(SbeCompatibility::noRequirements);
    return {newSortStage, newGroupStage};
}
}  // namespace

bool DocumentSourceInternalUnpackBucket::optimizeLastpoint(DocumentSourceContainer::iterator itr,
                                                           DocumentSourceContainer* container) {
    // A lastpoint-type aggregation must contain both a $sort and a $group stage, in that order, or
    // only a $group stage with a $top, $topN, $bottom, or $bottomN accumulator. This means we need
    // at least one stage after $_internalUnpackBucket.
    if (std::next(itr) == container->end()) {
        return false;
    }

    // If we only have one stage after $_internalUnpackBucket, it must be a $group for the
    // lastpoint rewrite to happen.
    DocumentSourceSort* sortStage = nullptr;
    auto groupStage = dynamic_cast<DocumentSourceGroup*>(std::next(itr)->get());

    // If we don't have a $sort + $group lastpoint query, we will need to replace the $group with
    // equivalent $sort + $group stages for the rewrite.
    boost::intrusive_ptr<DocumentSourceSort> sortStagePtr;
    boost::intrusive_ptr<DocumentSourceGroup> groupStagePtr;
    if (!groupStage && (std::next(itr, 2) != container->end())) {
        // If the first stage is not a $group, we may have a $sort + $group lastpoint query.
        sortStage = dynamic_cast<DocumentSourceSort*>(std::next(itr)->get());
        groupStage = dynamic_cast<DocumentSourceGroup*>(std::next(itr, 2)->get());
    } else if (groupStage) {
        // Try to rewrite the $group to a $sort+$group-style lastpoint query before proceeding with
        // the optimization.
        std::tie(sortStagePtr, groupStagePtr) =
            tryCreateBucketLevelSortGroup(getExpCtx(), itr, container, groupStage);

        // Both these stages should be discarded once we exit this function; either because the
        // rewrite failed validation checks, or because we created updated versions of these stages
        // in 'tryInsertBucketLevelSortAndGroup' below (which will be inserted into the pipeline).
        // The intrusive_ptrs above handle this deletion gracefully.
        sortStage = sortStagePtr.get();
        groupStage = groupStagePtr.get();
    }

    if (!sortStage || !groupStage) {
        return false;
    }

    if (sortStage->hasLimit()) {
        // This $sort stage was previously followed by a $limit stage.
        return false;
    }

    auto maybeMetaField = _sharedState->_bucketUnpacker.getMetaField();
    auto timeField = _sharedState->_bucketUnpacker.getTimeField();
    if (!maybeMetaField || haveComputedMetaField()) {
        return false;
    }

    auto metaField = maybeMetaField.value();
    if (!checkMetadataSortReorder(sortStage->getSortKeyPattern(), metaField, timeField)) {
        return false;
    }

    auto groupIdFields = groupStage->getIdFields();
    if (groupIdFields.size() != 1) {
        return false;
    }

    auto groupId = dynamic_cast<ExpressionFieldPath*>(groupIdFields.cbegin()->second.get());
    if (!groupId || groupId->isVariableReference()) {
        return false;
    }

    const auto fieldPath = groupId->getFieldPath();
    if (fieldPath.getPathLength() <= 1 || fieldPath.tail().getFieldName(0) != metaField) {
        return false;
    }

    auto rewrittenFieldPath = FieldPath(timeseries::kBucketMetaFieldName);
    if (fieldPath.tail().getPathLength() > 1) {
        rewrittenFieldPath = rewrittenFieldPath.concat(fieldPath.tail().tail());
    }
    auto newFieldPath = rewrittenFieldPath.fullPath();

    // Check to see if $group uses only the specified accumulator.
    auto accumulators = groupStage->getAccumulationStatements();
    auto groupOnlyUsesTargetAccum = [&](AccumulatorDocumentsNeeded targetAccum) {
        return std::all_of(accumulators.begin(), accumulators.end(), [&](auto&& accum) {
            return targetAccum == accum.makeAccumulator()->documentsNeeded();
        });
    };

    // We disallow firstpoint queries from participating in this rewrite due to the fact that we
    // round down control.min.time for buckets, which means that if we are given two buckets with
    // the same min time, we won't know which bucket contains the earliest time until we unpack
    // them and sort their measurements. Hence, this rewrite could give us an incorrect firstpoint.
    auto isSortValidForGroup = [&](AccumulatorDocumentsNeeded targetAccum) {
        bool firstpointTimeIsAscending =
            (targetAccum == AccumulatorDocumentsNeeded::kFirstInputDocument);
        for (const auto& entry : sortStage->getSortKeyPattern()) {
            auto isTimeField = entry.fieldPath->fullPath() == timeField;
            if (isTimeField && (entry.isAscending == firstpointTimeIsAscending)) {
                // This is a first-point query, which is disallowed.
                return false;
            } else if (!isTimeField &&
                       (entry.fieldPath->fullPath() != fieldPath.tail().fullPath())) {
                // This sorts on a metaField which does not match the $group's _id field.
                return false;
            }
        }
        return true;
    };

    // Try to insert bucket-level $sort and $group stages before we unpack any buckets. We ensure
    // that the generated $group preserves all bucket fields, so that the $_internalUnpackBucket
    // stage and the original $group stage can read them.
    std::vector<std::string> fieldsToInclude{std::string{timeseries::kBucketMetaFieldName},
                                             std::string{timeseries::kBucketControlFieldName},
                                             std::string{timeseries::kBucketDataFieldName}};
    const auto& computedMetaProjFields =
        _sharedState->_bucketUnpacker.bucketSpec().computedMetaProjFields();
    std::copy(computedMetaProjFields.begin(),
              computedMetaProjFields.end(),
              std::back_inserter(fieldsToInclude));

    auto tryInsertBucketLevelSortAndGroup = [&](AccumulatorDocumentsNeeded accum) {
        if (!groupOnlyUsesTargetAccum(accum) || !isSortValidForGroup(accum)) {
            return false;
        }

        bool flipSort = (accum == AccumulatorDocumentsNeeded::kLastInputDocument);
        auto newSort = createMetadataSortForReorder(*sortStage, timeField, newFieldPath, flipSort);
        auto newGroup = createBucketGroupForReorder(getExpCtx(), fieldsToInclude, newFieldPath);

        // Note that we don't erase any of the original stages for this rewrite. This allows us to
        // preserve the particular semantics of the original group (e.g. $top behaves differently
        // than topN with n = 1, $group accumulators have a special way of dealing with nulls, etc.)
        // without constructing a specialized projection to exactly match what the original query
        // would have returned.
        container->insert(itr, newSort);
        container->insert(itr, newGroup);
        return true;
    };

    const bool optimized =
        tryInsertBucketLevelSortAndGroup(AccumulatorDocumentsNeeded::kFirstInputDocument) ||
        tryInsertBucketLevelSortAndGroup(AccumulatorDocumentsNeeded::kLastInputDocument);

    // If we lower the group to SBE to read raw buckets we won't be able to unpack in SBE due to the
    // current limitations of 'TsBucketToCellBlockStage', but we don't want to run this pipeline in
    // hybrid mode because of the potential perf impact.
    if (optimized) {
        getExpCtx()->setSbePipelineCompatibility(SbeCompatibility::notCompatible);
        _isSbeCompatible = false;
    }
    return optimized;
}


bool findSequentialDocumentCache(DocumentSourceContainer::iterator start,
                                 DocumentSourceContainer::iterator end) {
    while (start != end && !dynamic_cast<DocumentSourceSequentialDocumentCache*>(start->get())) {
        start = std::next(start);
    }
    return start != end;
}

DocumentSourceContainer::iterator DocumentSourceInternalUnpackBucket::optimizeAtRestOfPipeline(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    if (itr == container->end()) {
        return itr;
    }

    invariant(*itr == this);
    DocumentSourceContainer::iterator unpackBucket = itr;

    itr = std::next(itr);

    try {
        while (itr != container->end()) {
            if (itr == unpackBucket) {
                itr = std::next(itr);
                if (itr == container->end())
                    break;
            }
            itr = (*itr).get()->optimizeAt(itr, container);
        }
    } catch (DBException& ex) {
        ex.addContext("Failed to optimize pipeline");
        throw;
    }

    return itr;
}

DepsTracker DocumentSourceInternalUnpackBucket::getRestPipelineDependencies(
    DocumentSourceContainer::iterator itr,
    DocumentSourceContainer* container,
    bool includeEventFilter) const {
    auto deps = Pipeline::getDependenciesForContainer(
        getExpCtx(),
        DocumentSourceContainer{std::next(itr), container->end()},
        DepsTracker::NoMetadataValidation());
    if (_sharedState->_eventFilter && includeEventFilter) {
        dependency_analysis::addDependencies(_sharedState->_eventFilter.get(), &deps);
    }
    return deps;
}

void DocumentSourceInternalUnpackBucket::addVariableRefs(std::set<Variables::Id>* refs) const {
    if (_sharedState->_eventFilter) {
        dependency_analysis::addVariableRefs(eventFilter(), refs);
    }
    if (_sharedState->_wholeBucketFilter) {
        dependency_analysis::addVariableRefs(wholeBucketFilter(), refs);
    }
}

namespace {
// For now, we only support adjacent $sort + $group for the top-k sort optimization.
std::pair<DocumentSourceSort*, DocumentSourceGroup*> getSortAndGroup(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    if (std::next(itr) == container->end() || std::next(std::next(itr)) == container->end()) {
        return {nullptr, nullptr};
    }

    auto prospectiveSortItr = std::next(itr);
    auto prospectiveSort = dynamic_cast<DocumentSourceSort*>(prospectiveSortItr->get());
    if (!prospectiveSort) {
        return {nullptr, nullptr};
    }

    auto prospectiveGroupItr = std::next(prospectiveSortItr);
    auto prospectiveGroup = dynamic_cast<DocumentSourceGroup*>(prospectiveGroupItr->get());
    if (!prospectiveGroup) {
        return {prospectiveSort, nullptr};
    }

    return {prospectiveSort, prospectiveGroup};
}
}  // namespace

bool DocumentSourceInternalUnpackBucket::tryToAbsorbTopKSortIntoGroup(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    auto [prospectiveSort, prospectiveGroup] = getSortAndGroup(itr, container);
    if (!prospectiveSort || !prospectiveGroup) {
        return false;
    }

    return prospectiveGroup->tryToAbsorbTopKSort(
        prospectiveSort, /*prospectiveSortItr=*/std::next(itr), container);
}

DocumentSourceContainer::iterator DocumentSourceInternalUnpackBucket::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {

    invariant(*itr == this);

    // See ../query/timeseries/README.md for a description of all the rewrites implemented in this
    // function. The order of optimizations in this function is important, since some optimizations
    // blocks others. Additionally, if an optimization is internalizing logic that could be
    // correlated in a $lookup, it must be placed after finding the sequential cache.

    if (itr != container->begin() && _checkIfNeedsIdPredicates.value_or(true)) {
        // If we were able to push a $match stage before this unpack stage, see if the predicates
        // are on control.(min|max).<timeField>, we may be able to augment them with a predicate on
        // _id as well, if there are no extended range values.
        //
        // Note that this transformation cannot apply on router, because the presence of extended
        // range values is not known there. See SERVER-73641.
        //
        // Unlike other rewrites for this stage, this rewrite affects a $match stage that is
        // *before* the unpack stage. So we need to apply this rewrite first, before the others,
        // which might cause us to return early.
        if (auto prevMatch = dynamic_cast<DocumentSourceMatch*>(std::prev(itr)->get()); prevMatch &&
            !getExpCtx()->getInRouter() && !_sharedState->_bucketUnpacker.getUsesExtendedRange()) {
            MatchExpression* matchExpr = prevMatch->getMatchExpression();
            bool updated = generateBucketLevelIdPredicates(matchExpr);
            if (updated) {
                BSONObj predObj = matchExpr->serialize();
                prevMatch->rebuild(predObj);
            }
            _checkIfNeedsIdPredicates = false;
        }
    }

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // Some optimizations may not be safe to do if we have computed the metaField via an $addFields
    // or a computed $project. We won't do those optimizations if 'haveComputedMetaField' is true.
    bool haveComputedMetaField = this->haveComputedMetaField();

    // Before any other rewrites for the current stage, consider reordering with $sort.
    if (auto sortPtr = dynamic_cast<DocumentSourceSort*>(std::next(itr)->get());
        sortPtr && !_sharedState->_eventFilter) {
        if (auto metaField = _sharedState->_bucketUnpacker.getMetaField();
            metaField && !haveComputedMetaField) {
            if (checkMetadataSortReorder(sortPtr->getSortKeyPattern(), metaField.value())) {
                // We have a sort on metadata field following this stage. Reorder the two stages
                // and return a pointer to the preceding stage.
                auto sortForReorder = createMetadataSortForReorder(*sortPtr);

                // If the original sort had a limit, pushing the $sort stage before the unpack
                // stage will not preserve that limit. To truly limit the number of documents,
                // we need to add an additional limit after the unpack stage.
                if (auto limit = sortPtr->getLimit(); limit && *limit != 0) {
                    // Current iterator is at $_internalUnpackBucket which is followed by $sort. We
                    // are pushing $limit after $sort.
                    auto limitPos = itr;
                    std::advance(limitPos, 2);
                    _triedLimitPushDownLocally = true;
                    container->insert(limitPos, DocumentSourceLimit::create(getExpCtx(), *limit));
                }

                // Reorder sort and current doc.
                *std::next(itr) = std::move(*itr);
                *itr = std::move(sortForReorder);

                if (itr == container->begin()) {
                    // Try to optimize the current stage again.
                    return std::next(itr);
                } else {
                    // Try to optimize the previous stage against $sort.
                    return std::prev(itr);
                }
            }
        }
    }

    if (tryToAbsorbTopKSortIntoGroup(itr, container)) {
        return itr;
    }

    // Attempt to push geoNear on the metaField past $_internalUnpackBucket.
    if (auto nextNear = dynamic_cast<DocumentSourceGeoNear*>(std::next(itr)->get());
        nextNear && !_sharedState->_eventFilter) {
        // Currently we only support geo indexes on the meta field, and we enforce this by
        // requiring the key field to be set so we can check before we try to look up indexes.
        auto keyField = nextNear->getKeyField();
        uassert(5892921,
                "Must specify 'key' option for $geoNear on a time-series collection",
                keyField);

        // Currently we do not support query for $geoNear on a bucket
        uassert(
            1938439,
            "Must not specify 'query' for $geoNear on a time-series collection; use $match instead",
            nextNear->getQuery().binaryEqual(BSONObj()));

        auto metaField = _sharedState->_bucketUnpacker.getMetaField();
        if (metaField && *metaField == keyField->front()) {
            // Make sure we actually re-write the key field when reading raw timeseries buckets so
            // we can locate the index.
            static const FieldPath baseMetaFieldPath{timeseries::kBucketMetaFieldName};
            nextNear->setKeyField(keyField->getPathLength() > 1
                                      ? baseMetaFieldPath.concat(keyField->tail())
                                      : baseMetaFieldPath);

            // Save the source, remove it, and then push it down.
            auto source = *std::next(itr);
            container->erase(std::next(itr));
            container->insert(itr, source);
            return std::prev(itr) == container->begin() ? std::prev(itr)
                                                        : std::prev(std::prev(itr));
        } else {
            // Don't push down query on measurements.
        }
    }

    // OptimizeAt the pipeline after this stage to merge $match stages and push them forward.
    if (!_optimizedEndOfPipeline) {
        _optimizedEndOfPipeline = true;

        if (std::next(itr) == container->end()) {
            return container->end();
        }

        // TODO SERVER-84113: Remove specific caching logic and calls that result in optimize()
        // being called.
        auto cacheFound = findSequentialDocumentCache(itr, container->end());
        if (cacheFound) {
            // We want to call optimizeAt() on the rest of the pipeline first, and exit this
            // function since any calls to optimize() will interfere with the
            // sequentialDocumentCache's ability to properly place itself or abandon. This ensures
            // the unpack stage is always uncorrelated.
            tassert(10313200,
                    "Cannot internalize any $match stages with a cache",
                    !_sharedState->_eventFilter);
            return DocumentSourceInternalUnpackBucket::optimizeAtRestOfPipeline(itr, container);
        } else {
            if (auto nextStage = dynamic_cast<DocumentSourceGeoNear*>(std::next(itr)->get())) {
                // If the end of the pipeline starts with a $geoNear stage, make sure it gets
                // optimized in a context where it knows there are other stages before it. It will
                // split itself up into separate $match and $sort stages. But it doesn't split
                // itself up when it's the first stage, because it expects to use a special
                // DocumentSouceGeoNearCursor plan.
                nextStage->optimizeAt(std::next(itr), container);
            }
            // We want to optimize the rest of the pipeline to ensure the stages are in their
            // optimal position and expressions have been optimized to allow for certain rewrites.
            Pipeline::optimizeEndOfPipeline(itr, container);
        }

        if (std::next(itr) == container->end()) {
            return container->end();
        } else {
            // Kick back out to optimizing this stage again a level up, so any matches that were
            // moved to directly after this stage can be moved before it if possible.
            return itr;
        }
    }

    // If the next stage is a limit, then push the limit above to avoid fetching more buckets than
    // necessary.
    // If _eventFilter is true, a match was present which may impact the number of
    // documents we return from limit, hence we don't want to push limit.
    // If _triedLimitPushDownLocally is true, we have already done a limit push down and don't want
    // to push again to avoid an infinite loop.
    if (!_sharedState->_eventFilter && !_triedLimitPushDownLocally) {
        if (auto limitPtr = dynamic_cast<DocumentSourceLimit*>(std::next(itr)->get()); limitPtr) {
            _triedLimitPushDownLocally = true;
            container->insert(itr, DocumentSourceLimit::create(getExpCtx(), limitPtr->getLimit()));
            return container->begin();
        }
    }

    if (!_sharedState->_eventFilter) {
        // Check if we can avoid unpacking if we have a group stage with min/max/count aggregates.
        auto [success, result] = rewriteGroupStage(itr, container);
        if (success) {
            return result;
        }
    }

    //
    // If a field is overwritten through computed projection/addFields, or project out using a
    // $project, then the fields are no longer accessible after the projection stage. This would
    // also invalidate the control block summaries. The optimizations below this check may assume
    // that the bucket fields and the control block is still valid.
    //
    bool hasAlreadyAbsorbedProjectSpec = _sharedState->_bucketUnpacker.hasIncludeExcludeFields() ||
        !_sharedState->_bucketUnpacker.bucketSpec().computedMetaProjFields().empty();

    {
        // Check if the rest of the pipeline needs any fields. For example we might only be
        // interested in $count.
        auto deps = getRestPipelineDependencies(itr, container, true /* includeEventFilter */);
        if (deps.hasNoRequirements() && !hasAlreadyAbsorbedProjectSpec) {
            _sharedState->_bucketUnpacker.setBucketSpec(
                {_sharedState->_bucketUnpacker.getTimeField(),
                 _sharedState->_bucketUnpacker.getMetaField(),
                 {},
                 BucketSpec::Behavior::kInclude});

            // Keep going for next optimization.
        }

        if (deps.getNeedsMetadata(DocumentMetadataFields::MetaType::kTimeseriesBucketMinTime)) {
            _sharedState->_bucketUnpacker.setIncludeMinTimeAsMetadata();
        }

        if (deps.getNeedsMetadata(DocumentMetadataFields::MetaType::kTimeseriesBucketMaxTime)) {
            _sharedState->_bucketUnpacker.setIncludeMaxTimeAsMetadata();
        }
    }

    // Attempt to optimize last-point type queries.
    if (!hasAlreadyAbsorbedProjectSpec && !_triedLastpointRewriteLocally &&
        !_sharedState->_eventFilter && optimizeLastpoint(itr, container)) {
        _triedLastpointRewriteLocally = true;
        // If we are able to rewrite the aggregation, give the resulting pipeline a chance to
        // perform further optimizations.
        return container->begin();
    };

    // Attempt to map predicates on bucketed fields to the predicates on the control field.
    if (auto nextMatch = dynamic_cast<DocumentSourceMatch*>(std::next(itr)->get());
        !hasAlreadyAbsorbedProjectSpec && nextMatch) {
        // Merge multiple following $match stages.
        auto itrToMatch = std::next(itr);
        while (std::next(itrToMatch) != container->end() &&
               dynamic_cast<DocumentSourceMatch*>(std::next(itrToMatch)->get())) {
            nextMatch->doOptimizeAt(itrToMatch, container);
        }

        auto predicates = createPredicatesOnBucketLevelField(nextMatch->getMatchExpression());

        // Try to create a tight bucket predicate to perform bucket level matching.
        // If we removed the _eventFilter with fixed buckets, we do not need a wholeBucketFilter.
        if (predicates.tightPredicate && !predicates.rewriteProvidesExactMatchPredicate) {

            _wholeBucketFilterBson = predicates.tightPredicate->serialize();
            _sharedState->_wholeBucketFilter =
                uassertStatusOK(MatchExpressionParser::parse(_wholeBucketFilterBson,
                                                             getExpCtx(),
                                                             ExtensionsCallbackNoop(),
                                                             Pipeline::kAllowedMatcherFeatures));
            _sharedState->_wholeBucketFilter =
                optimizeMatchExpression(std::move(_sharedState->_wholeBucketFilter),
                                        /* enableSimplification */ false);
        }

        if (!predicates.rewriteProvidesExactMatchPredicate) {
            // Push the original event predicate into the unpacking stage.
            setEventFilter(nextMatch->getQuery(), true /* shouldOptimize */);
        }

        container->erase(std::next(itr));
        auto deps = getRestPipelineDependencies(itr, container, false /* includeEventFilter */);
        if (deps.fields.empty()) {
            _unpackToBson = true;
        }

        // Create a loose bucket predicate and push it before the unpacking stage.
        if (predicates.loosePredicate) {
            container->insert(
                itr,
                DocumentSourceMatch::create(predicates.loosePredicate->serialize(), getExpCtx()));

            // If we generated predicates on the control block's min or max of the time field we may
            // be able to also generate predicates on _id.
            _checkIfNeedsIdPredicates = true;

            // Give other stages a chance to optimize with the new $match.
            return std::prev(itr) == container->begin() ? std::prev(itr)
                                                        : std::prev(std::prev(itr));
        }

        // We have removed a $match after this stage, so we try to optimize this stage again.
        return itr;
    }

    // Attempt to push down an exclusion $project on the metaField past $_internalUnpackBucket. Only
    // patterns {$project: {metaField: 0, ...}} would be eligible for this optimization.
    if (!_sharedState->_eventFilter && !haveComputedMetaField) {
        if (auto [metaProject, deleteRemainder] = extractProjectForPushDown(std::next(itr)->get());
            !metaProject.isEmpty()) {
            container->insert(itr,
                              DocumentSourceProject::createFromBson(
                                  BSON("$project" << metaProject).firstElement(), getExpCtx()));

            if (deleteRemainder) {
                // We have pushed down the entire $project. Remove the old $project from the
                // pipeline, then attempt to optimize this stage again.
                container->erase(std::next(itr));
                return std::prev(itr) == container->begin() ? std::prev(itr)
                                                            : std::prev(std::prev(itr));
            }
        }
    }

    // Attempt to extract computed projections on metaField from subsequent $project, $addFields, or
    // $set and push them before the $_internalunpackBucket.
    if (auto nextItr = pushDownComputedMetaProjection(itr, container)) {
        // We've pushed down and removed a stage after this one. Try to optimize the new stage.
        return *nextItr;
    }

    if (!hasAlreadyAbsorbedProjectSpec) {
        enableStreamingGroupIfPossible(itr, container);
    }

    // Attempt to build a $project based on dependency analysis or extract one from the pipeline. We
    // can internalize the result so we can handle projections during unpacking.
    if (!_triedInternalizeProjectLocally) {
        if (auto [project, isInclusion] = extractOrBuildProjectToInternalize(itr, container);
            !project.isEmpty()) {
            _triedInternalizeProjectLocally = true;
            internalizeProject(project, isInclusion);

            // We may have removed a $project after this stage, so we try to optimize this stage
            // again.
            return itr;
        }
    }

    return container->end();
}

bool DocumentSourceInternalUnpackBucket::isSbeCompatible() {
    // Caches the SBE compatibility status when this function is called for the first time. This is
    // called before trying to push down stages to SBE.
    if (!_isSbeCompatible) {
        _isSbeCompatible.emplace([&] {
            // Just in case that the event filter or the whole bucket filter is incompatible with
            // SBE. While optimizing pipeline, we may end up with pushing SBE-incompatible filters
            // down to the '$_internalUnpackBucket' stage. We've stored the sbeCompatibility of
            // '_eventFilter' in '_isEventFilterSbeCompatible'.
            if (_sharedState->_eventFilter) {
                tassert(8062700,
                        "If _eventFilter is set, we must have determined if it is compatible with "
                        "SBE or not.",
                        _isEventFilterSbeCompatible);
                if (_isEventFilterSbeCompatible.get() < SbeCompatibility::noRequirements) {
                    return false;
                }
            }

            // Currently we only support in SBE unpacking with a statically known set of fields.
            if (_sharedState->_bucketUnpacker.bucketSpec().behavior() !=
                BucketSpec::Behavior::kInclude) {
                return false;
            }

            return true;
        }());
    }

    return *_isSbeCompatible;
}

DocumentSource::GetModPathsReturn DocumentSourceInternalUnpackBucket::getModifiedPaths() const {
    if (_sharedState->_bucketUnpacker.includeMetaField()) {
        StringMap<std::string> renames;
        renames.emplace(*_sharedState->_bucketUnpacker.getMetaField(),
                        timeseries::kBucketMetaFieldName);
        return {GetModPathsReturn::Type::kAllExcept, OrderedPathSet{}, std::move(renames)};
    }
    return {GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
}
}  // namespace mongo
