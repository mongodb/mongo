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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include <algorithm>
#include <iterator>

#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"

#include <string>
#include <type_traits>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

namespace mongo {

/*
 * $_internalUnpackBucket is an internal stage for materializing time-series measurements from
 * time-series collections. It should never be used anywhere outside the MongoDB server.
 */
REGISTER_DOCUMENT_SOURCE(_internalUnpackBucket,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalUnpackBucket::createFromBsonInternal,
                         AllowedWithApiStrict::kAlways);

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
        (proj->getType() == TransformerInterface::TransformerType::kInclusionProjection ||
         proj->getType() == TransformerInterface::TransformerType::kExclusionProjection)) {
        return std::pair{proj->getTransformer().serializeTransformation(boost::none).toBson(),
                         proj->getType() ==
                             TransformerInterface::TransformerType::kInclusionProjection};
    }
    return std::pair{BSONObj{}, false};
}

/**
 * Creates an ObjectId initialized with an appropriate timestamp corresponding to 'rhs' and
 * returns it as a Value.
 */
template <typename MatchType>
auto constructObjectIdValue(const BSONElement& rhs, int bucketMaxSpanSeconds) {
    // Indicates whether to initialize an ObjectId with a max or min value for the non-date bytes.
    enum class OIDInit : bool { max, min };
    // Make an ObjectId cooresponding to a date value.
    auto makeDateOID = [](auto&& date, auto&& maxOrMin) {
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
        return Value{makeDateOID(rhs.date(), OIDInit::min)};
    } else if constexpr (std::is_same_v<MatchType, LTEMatchExpression>) {
        return Value{makeDateOID(rhs.date(), OIDInit::max)};
    } else if constexpr (std::is_same_v<MatchType, GTMatchExpression>) {
        return Value{makeMaxAdjustedDateOID(rhs.date(), OIDInit::max)};
    } else if constexpr (std::is_same_v<MatchType, GTEMatchExpression>) {
        return Value{makeMaxAdjustedDateOID(rhs.date(), OIDInit::min)};
    }
    MONGO_UNREACHABLE_TASSERT(5756800);
}

/**
 * Checks if a sort stage's pattern following our internal unpack bucket is suitable to be reordered
 * before us. The sort stage must refer exclusively to the meta field or any subfields.
 */
bool checkMetadataSortReorder(const SortPattern& sortPattern, const StringData& metaFieldStr) {
    for (const auto& sortKey : sortPattern) {
        if (!sortKey.fieldPath.has_value()) {
            return false;
        }
        if (sortKey.fieldPath->getPathLength() < 1) {
            return false;
        }
        if (sortKey.fieldPath->getFieldName(0) != metaFieldStr) {
            return false;
        }
    }
    return true;
}

/**
 * Returns a new DocumentSort to reorder before current unpack bucket document.
 */
boost::intrusive_ptr<DocumentSourceSort> createMetadataSortForReorder(
    const DocumentSourceSort& sort) {
    std::vector<SortPattern::SortPatternPart> updatedPattern;
    for (const auto& entry : sort.getSortKeyPattern()) {
        // Repoint sort to use metadata field before renaming.
        auto updatedFieldPath = FieldPath(timeseries::kBucketMetaFieldName);
        if (entry.fieldPath->getPathLength() > 1) {
            updatedFieldPath = updatedFieldPath.concat(entry.fieldPath->tail());
        }

        updatedPattern.push_back(entry);
        updatedPattern.back().fieldPath = updatedFieldPath;
    }

    boost::optional<uint64_t> maxMemoryUsageBytes;
    if (auto sortStatsPtr = dynamic_cast<const SortStats*>(sort.getSpecificStats())) {
        maxMemoryUsageBytes = sortStatsPtr->maxMemoryUsageBytes;
    }

    return DocumentSourceSort::create(sort.getContext(),
                                      SortPattern{updatedPattern},
                                      sort.getLimit().get_value_or(0),
                                      maxMemoryUsageBytes);
}

// Optimize the section of the pipeline before the $_internalUnpackBucket stage.
void optimizePrefix(Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    auto prefix = Pipeline::SourceContainer(container->begin(), itr);
    Pipeline::optimizeContainer(&prefix);
    container->erase(container->begin(), itr);
    container->splice(itr, prefix);
}

// Returns whether 'field' depends on a pushed down $addFields or computed $project.
bool fieldIsComputed(BucketSpec spec, std::string field) {
    return std::any_of(
        spec.computedMetaProjFields.begin(), spec.computedMetaProjFields.end(), [&](auto& s) {
            return s == field || expression::isPathPrefixOf(field, s) ||
                expression::isPathPrefixOf(s, field);
        });
}

}  // namespace

DocumentSourceInternalUnpackBucket::DocumentSourceInternalUnpackBucket(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BucketUnpacker bucketUnpacker,
    int bucketMaxSpanSeconds)
    : DocumentSource(kStageNameInternal, expCtx),
      _bucketUnpacker(std::move(bucketUnpacker)),
      _bucketMaxSpanSeconds{bucketMaxSpanSeconds} {}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalUnpackBucket::createFromBsonInternal(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5346500,
            str::stream() << "$_internalUnpackBucket specification must be an object, got: "
                          << specElem.type(),
            specElem.type() == BSONType::Object);

    // If neither "include" nor "exclude" is specified, the default is "exclude": [] and
    // if that's the case, no field will be added to 'bucketSpec.fieldSet' in the for-loop below.
    BucketUnpacker::Behavior unpackerBehavior = BucketUnpacker::Behavior::kExclude;
    BucketSpec bucketSpec;
    auto hasIncludeExclude = false;
    auto hasTimeField = false;
    auto hasBucketMaxSpanSeconds = false;
    auto bucketMaxSpanSeconds = 0;
    std::vector<std::string> computedMetaProjFields;
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
                    elem.type() == BSONType::Array);

            for (auto&& elt : elem.embeddedObject()) {
                uassert(5346502,
                        str::stream() << "include or exclude field element must be a string, got: "
                                      << elt.type(),
                        elt.type() == BSONType::String);
                auto field = elt.valueStringData();
                uassert(5346503,
                        "include or exclude field element must be a single-element field path",
                        field.find('.') == std::string::npos);
                bucketSpec.fieldSet.emplace(field);
            }
            unpackerBehavior = fieldName == kInclude ? BucketUnpacker::Behavior::kInclude
                                                     : BucketUnpacker::Behavior::kExclude;
            hasIncludeExclude = true;
        } else if (fieldName == timeseries::kTimeFieldName) {
            uassert(5346504,
                    str::stream() << "timeField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::String);
            bucketSpec.timeField = elem.str();
            hasTimeField = true;
        } else if (fieldName == timeseries::kMetaFieldName) {
            uassert(5346505,
                    str::stream() << "metaField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::String);
            auto metaField = elem.str();
            uassert(5545700,
                    str::stream() << "metaField field must be a single-element field path",
                    metaField.find('.') == std::string::npos);
            bucketSpec.metaField = std::move(metaField);
        } else if (fieldName == kBucketMaxSpanSeconds) {
            uassert(5510600,
                    str::stream() << "bucketMaxSpanSeconds field must be an integer, got: "
                                  << elem.type(),
                    elem.type() == BSONType::NumberInt);
            uassert(5510601,
                    "bucketMaxSpanSeconds field must be greater than zero",
                    elem._numberInt() > 0);
            bucketMaxSpanSeconds = elem._numberInt();
            hasBucketMaxSpanSeconds = true;
        } else if (fieldName == "computedMetaProjFields") {
            uassert(5509900,
                    str::stream() << "computedMetaProjFields field must be an array, got: "
                                  << elem.type(),
                    elem.type() == BSONType::Array);

            for (auto&& elt : elem.embeddedObject()) {
                uassert(5509901,
                        str::stream()
                            << "computedMetaProjFields field element must be a string, got: "
                            << elt.type(),
                        elt.type() == BSONType::String);
                auto field = elt.valueStringData();
                uassert(5509902,
                        "computedMetaProjFields field element must be a single-element field path",
                        field.find('.') == std::string::npos);
                bucketSpec.computedMetaProjFields.emplace_back(field);
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

    return make_intrusive<DocumentSourceInternalUnpackBucket>(
        expCtx, BucketUnpacker{std::move(bucketSpec), unpackerBehavior}, bucketMaxSpanSeconds);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalUnpackBucket::createFromBsonExternal(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5612400,
            str::stream() << "$_unpackBucket specification must be an object, got: "
                          << specElem.type(),
            specElem.type() == BSONType::Object);

    BucketSpec bucketSpec;
    auto hasTimeField = false;
    for (auto&& elem : specElem.embeddedObject()) {
        auto fieldName = elem.fieldNameStringData();
        // We only expose "timeField" and "metaField" as parameters in $_unpackBucket.
        if (fieldName == timeseries::kTimeFieldName) {
            uassert(5612401,
                    str::stream() << "timeField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::String);
            bucketSpec.timeField = elem.str();
            hasTimeField = true;
        } else if (fieldName == timeseries::kMetaFieldName) {
            uassert(5612402,
                    str::stream() << "metaField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::String);
            auto metaField = elem.str();
            uassert(5612403,
                    str::stream() << "metaField field must be a single-element field path",
                    metaField.find('.') == std::string::npos);
            bucketSpec.metaField = std::move(metaField);
        } else {
            uasserted(5612404,
                      str::stream() << "unrecognized parameter to $_unpackBucket: " << fieldName);
        }
    }
    uassert(5612405,
            str::stream() << "The $_unpackBucket stage requires a timeField parameter",
            hasTimeField);

    return make_intrusive<DocumentSourceInternalUnpackBucket>(
        expCtx, BucketUnpacker{std::move(bucketSpec), BucketUnpacker::Behavior::kExclude}, 3600);
}

void DocumentSourceInternalUnpackBucket::serializeToArray(
    std::vector<Value>& array, boost::optional<ExplainOptions::Verbosity> explain) const {
    MutableDocument out;
    auto behavior =
        _bucketUnpacker.behavior() == BucketUnpacker::Behavior::kInclude ? kInclude : kExclude;
    auto&& spec = _bucketUnpacker.bucketSpec();
    std::vector<Value> fields;
    for (auto&& field : spec.fieldSet) {
        fields.emplace_back(field);
    }
    if (((_bucketUnpacker.includeMetaField() &&
          _bucketUnpacker.behavior() == BucketUnpacker::Behavior::kInclude) ||
         (!_bucketUnpacker.includeMetaField() &&
          _bucketUnpacker.behavior() == BucketUnpacker::Behavior::kExclude && spec.metaField)) &&
        std::find(spec.computedMetaProjFields.cbegin(),
                  spec.computedMetaProjFields.cend(),
                  *spec.metaField) == spec.computedMetaProjFields.cend())
        fields.emplace_back(*spec.metaField);

    out.addField(behavior, Value{std::move(fields)});
    out.addField(timeseries::kTimeFieldName, Value{spec.timeField});
    if (spec.metaField) {
        out.addField(timeseries::kMetaFieldName, Value{*spec.metaField});
    }
    out.addField(kBucketMaxSpanSeconds, Value{_bucketMaxSpanSeconds});

    if (!spec.computedMetaProjFields.empty())
        out.addField("computedMetaProjFields", Value{[&] {
                         std::vector<Value> compFields;
                         std::transform(spec.computedMetaProjFields.cbegin(),
                                        spec.computedMetaProjFields.cend(),
                                        std::back_inserter(compFields),
                                        [](auto&& projString) { return Value{projString}; });
                         return compFields;
                     }()});

    if (!explain) {
        array.push_back(Value(DOC(getSourceName() << out.freeze())));
        if (_sampleSize) {
            auto sampleSrc = DocumentSourceSample::create(pExpCtx, *_sampleSize);
            sampleSrc->serializeToArray(array);
        }
    } else {
        if (_sampleSize) {
            out.addField("sample", Value{static_cast<long long>(*_sampleSize)});
            out.addField("bucketMaxCount", Value{_bucketMaxCount});
        }
        array.push_back(Value(DOC(getSourceName() << out.freeze())));
    }
}

DocumentSource::GetNextResult DocumentSourceInternalUnpackBucket::doGetNext() {
    tassert(5521502, "calling doGetNext() when '_sampleSize' is set is disallowed", !_sampleSize);

    // Otherwise, fallback to unpacking every measurement in all buckets until the child stage is
    // exhausted.
    if (_bucketUnpacker.hasNext()) {
        return _bucketUnpacker.getNext();
    }

    auto nextResult = pSource->getNext();
    if (nextResult.isAdvanced()) {
        auto bucket = nextResult.getDocument().toBson();
        _bucketUnpacker.reset(std::move(bucket));
        uassert(5346509,
                str::stream() << "A bucket with _id "
                              << _bucketUnpacker.bucket()[timeseries::kBucketIdFieldName].toString()
                              << " contains an empty data region",
                _bucketUnpacker.hasNext());
        return _bucketUnpacker.getNext();
    }

    return nextResult;
}

bool DocumentSourceInternalUnpackBucket::pushDownComputedMetaProjection(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    bool nextStageWasRemoved = false;
    if (std::next(itr) == container->end()) {
        return nextStageWasRemoved;
    }
    if (!_bucketUnpacker.bucketSpec().metaField) {
        return nextStageWasRemoved;
    }

    if (auto nextTransform =
            dynamic_cast<DocumentSourceSingleDocumentTransformation*>(std::next(itr)->get());
        nextTransform &&
        (nextTransform->getType() == TransformerInterface::TransformerType::kInclusionProjection ||
         nextTransform->getType() == TransformerInterface::TransformerType::kComputedProjection)) {

        auto& metaName = _bucketUnpacker.bucketSpec().metaField.get();
        auto [addFieldsSpec, deleteStage] =
            nextTransform->extractComputedProjections(metaName,
                                                      timeseries::kBucketMetaFieldName.toString(),
                                                      BucketUnpacker::reservedBucketFieldNames);
        nextStageWasRemoved = deleteStage;

        if (!addFieldsSpec.isEmpty()) {
            // Extend bucket specification of this stage to include the computed meta projections
            // that are passed through.
            std::vector<StringData> computedMetaProjFields;
            for (auto&& elem : addFieldsSpec) {
                computedMetaProjFields.emplace_back(elem.fieldName());
            }
            _bucketUnpacker.addComputedMetaProjFields(computedMetaProjFields);
            // Insert extracted computed projections before the $_internalUnpackBucket.
            container->insert(
                itr,
                DocumentSourceAddFields::createFromBson(
                    BSON("$addFields" << addFieldsSpec).firstElement(), getContext()));
            // Remove the next stage if it became empty after the field extraction.
            if (deleteStage) {
                container->erase(std::next(itr));
            }
        }
    }
    return nextStageWasRemoved;
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
    auto spec = _bucketUnpacker.bucketSpec();
    spec.fieldSet = std::move(fields);
    _bucketUnpacker.setBucketSpecAndBehavior(std::move(spec),
                                             isInclusion ? BucketUnpacker::Behavior::kInclude
                                                         : BucketUnpacker::Behavior::kExclude);
}

std::pair<BSONObj, bool> DocumentSourceInternalUnpackBucket::extractOrBuildProjectToInternalize(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) const {
    if (std::next(itr) == container->end() || !_bucketUnpacker.bucketSpec().fieldSet.empty()) {
        // There is no project to internalize or there are already fields being included/excluded.
        return {BSONObj{}, false};
    }

    // Check for a viable inclusion $project after the $_internalUnpackBucket.
    auto [existingProj, isInclusion] = getIncludeExcludeProjectAndType(std::next(itr)->get());
    if (isInclusion && !existingProj.isEmpty() && canInternalizeProjectObj(existingProj)) {
        container->erase(std::next(itr));
        return {existingProj, isInclusion};
    }

    // Attempt to get an inclusion $project representing the root-level dependencies of the pipeline
    // after the $_internalUnpackBucket. If this $project is not empty, then the dependency set was
    // finite.
    Pipeline::SourceContainer restOfPipeline(std::next(itr), container->end());
    auto deps = Pipeline::getDependenciesForContainer(pExpCtx, restOfPipeline, boost::none);
    if (auto dependencyProj =
            deps.toProjectionWithoutMetadata(DepsTracker::TruncateToRootLevel::yes);
        !dependencyProj.isEmpty()) {
        return {dependencyProj, true};
    }

    // Check for a viable exclusion $project after the $_internalUnpackBucket.
    if (!existingProj.isEmpty() && canInternalizeProjectObj(existingProj)) {
        container->erase(std::next(itr));
        return {existingProj, isInclusion};
    }

    return {BSONObj{}, false};
}

/*
 * Creates a predicate that ensures that if there exists a subpath of matchExprPath such that the
 * type of `control.min.subpath` is not the same as `control.max.subpath` then we will match that
 * document.
 */
std::unique_ptr<MatchExpression> createTypeEqualityPredicate(
    boost::intrusive_ptr<ExpressionContext> pExpCtx, const StringData& matchExprPath) {
    FieldPath matchExprField(matchExprPath);
    using namespace timeseries;
    std::vector<std::unique_ptr<MatchExpression>> typeEqualityPredicates;

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
    return std::make_unique<OrMatchExpression>(std::move(typeEqualityPredicates));
}

std::unique_ptr<MatchExpression> createComparisonPredicate(
    const ComparisonMatchExpression* matchExpr,
    const BucketSpec& bucketSpec,
    int bucketMaxSpanSeconds,
    ExpressionContext::CollationMatchesDefault collationMatchesDefault,
    boost::intrusive_ptr<ExpressionContext> pExpCtx) {
    using namespace timeseries;
    const auto matchExprPath = matchExpr->path();
    const auto matchExprData = matchExpr->getData();

    // The control field's min and max are chosen using a field-order insensitive comparator, while
    // MatchExpressions use a comparator that treats field-order as significant. Because of this we
    // will not perform this optimization on queries with operands of compound types.
    if (matchExprData.type() == BSONType::Object || matchExprData.type() == BSONType::Array)
        return nullptr;

    // MatchExpressions have special comparison semantics regarding null, in that {$eq: null} will
    // match all documents where the field is either null or missing. Because this is different
    // from both the comparison semantics that InternalExprComparison expressions and the control's
    // min and max fields use, we will not perform this optimization on queries with null operands.
    if (matchExprData.type() == BSONType::jstNULL)
        return nullptr;

    // The control field's min and max are chosen based on the collation of the collection. If the
    // query's collation does not match the collection's collation and the query operand is a
    // string or compound type (skipped above) we will not perform this optimization.
    if (collationMatchesDefault == ExpressionContext::CollationMatchesDefault::kNo &&
        matchExprData.type() == BSONType::String) {
        return nullptr;
    }

    // We must avoid mapping predicates on the meta field onto the control field.
    if (bucketSpec.metaField &&
        (matchExprPath == bucketSpec.metaField.get() ||
         expression::isPathPrefixOf(bucketSpec.metaField.get(), matchExprPath)))
        return nullptr;

    // We must avoid mapping predicates on fields computed via $addFields or a computed $project.
    if (fieldIsComputed(bucketSpec, matchExprPath.toString())) {
        return nullptr;
    }

    const auto isTimeField = (matchExprPath == bucketSpec.timeField);
    if (isTimeField && matchExprData.type() != BSONType::Date) {
        // Users are not allowed to insert non-date measurements into time field. So this query
        // would not match anything. We do not need to optimize for this case.
        return nullptr;
    }

    BSONObj minTime;
    if (isTimeField) {
        auto timeField = matchExprData.Date();
        minTime = BSON("" << timeField - Seconds(bucketMaxSpanSeconds));
    }

    auto minPath = std::string{kControlMinFieldNamePrefix} + matchExprPath;
    auto maxPath = std::string{kControlMaxFieldNamePrefix} + matchExprPath;

    switch (matchExpr->matchType()) {
        case MatchExpression::EQ:
            // For $eq, make both a $lte against 'control.min' and a $gte predicate against
            // 'control.max'.
            //
            // If the comparison is against the 'time' field, include a predicate against the _id
            // field which is converted to the maximum for the corresponding range of ObjectIds and
            // is adjusted by the max range for a bucket to approximate the max bucket value given
            // the min. Also include a predicate against the _id field which is converted to the
            // minimum for the range of ObjectIds corresponding to the given date. In
            // addition, we include a {'control.min' : {$gte: 'time - bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time' field
            // and routing on mongos.
            return isTimeField
                ? makePredicate(
                      MatchExprPredicate<InternalExprLTEMatchExpression>(minPath, matchExprData),
                      MatchExprPredicate<InternalExprGTEMatchExpression>(minPath,
                                                                         minTime.firstElement()),
                      MatchExprPredicate<InternalExprGTEMatchExpression>(maxPath, matchExprData),
                      MatchExprPredicate<LTEMatchExpression, Value>(
                          kBucketIdFieldName,
                          constructObjectIdValue<LTEMatchExpression>(matchExprData,
                                                                     bucketMaxSpanSeconds)),
                      MatchExprPredicate<GTEMatchExpression, Value>(
                          kBucketIdFieldName,
                          constructObjectIdValue<GTEMatchExpression>(matchExprData,
                                                                     bucketMaxSpanSeconds)))
                : std::make_unique<OrMatchExpression>(makeVector<std::unique_ptr<MatchExpression>>(
                      makePredicate(MatchExprPredicate<InternalExprLTEMatchExpression>(
                                        minPath, matchExprData),
                                    MatchExprPredicate<InternalExprGTEMatchExpression>(
                                        maxPath, matchExprData)),
                      createTypeEqualityPredicate(pExpCtx, matchExprPath)));

        case MatchExpression::GT:
            // For $gt, make a $gt predicate against 'control.max'. In addition, if the comparison
            // is against the 'time' field, include a predicate against the _id field which is
            // converted to the maximum for the corresponding range of ObjectIds and is adjusted
            // by the max range for a bucket to approximate the max bucket value given the min. In
            // addition, we include a {'control.min' : {$gt: 'time - bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time' field
            // and routing on mongos.
            return isTimeField
                ? makePredicate(

                      MatchExprPredicate<InternalExprGTMatchExpression>(maxPath, matchExprData),
                      MatchExprPredicate<InternalExprGTMatchExpression>(minPath,
                                                                        minTime.firstElement()),
                      MatchExprPredicate<GTMatchExpression, Value>(
                          kBucketIdFieldName,
                          constructObjectIdValue<GTMatchExpression>(matchExprData,
                                                                    bucketMaxSpanSeconds)))
                : std::make_unique<OrMatchExpression>(makeVector<std::unique_ptr<MatchExpression>>(
                      std::make_unique<InternalExprGTMatchExpression>(maxPath, matchExprData),
                      createTypeEqualityPredicate(pExpCtx, matchExprPath)));

        case MatchExpression::GTE:
            // For $gte, make a $gte predicate against 'control.max'. In addition, if the comparison
            // is against the 'time' field, include a predicate against the _id field which is
            // converted to the minimum for the corresponding range of ObjectIds and is adjusted
            // by the max range for a bucket to approximate the max bucket value given the min. In
            // addition, we include a {'control.min' : {$gte: 'time - bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time' field
            // and routing on mongos.
            return isTimeField
                ? makePredicate(

                      MatchExprPredicate<InternalExprGTEMatchExpression>(maxPath, matchExprData),
                      MatchExprPredicate<InternalExprGTEMatchExpression>(minPath,
                                                                         minTime.firstElement()),
                      MatchExprPredicate<GTEMatchExpression, Value>(
                          kBucketIdFieldName,
                          constructObjectIdValue<GTEMatchExpression>(matchExprData,
                                                                     bucketMaxSpanSeconds)))
                : std::make_unique<OrMatchExpression>(makeVector<std::unique_ptr<MatchExpression>>(
                      std::make_unique<InternalExprGTEMatchExpression>(maxPath, matchExprData),
                      createTypeEqualityPredicate(pExpCtx, matchExprPath)));

        case MatchExpression::LT:
            // For $lt, make a $lt predicate against 'control.min'. In addition, if the comparison
            // is against the 'time' field, include a predicate against the _id field which is
            // converted to the minimum for the corresponding range of ObjectIds. In
            // addition, we include a {'control.min' : {$lt: 'time - bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time' field
            // and routing on mongos.
            return isTimeField
                ? makePredicate(
                      MatchExprPredicate<InternalExprLTMatchExpression>(minPath, matchExprData),
                      MatchExprPredicate<LTMatchExpression, Value>(
                          kBucketIdFieldName,
                          constructObjectIdValue<LTMatchExpression>(matchExprData,
                                                                    bucketMaxSpanSeconds)))
                : std::make_unique<OrMatchExpression>(makeVector<std::unique_ptr<MatchExpression>>(
                      std::make_unique<InternalExprLTMatchExpression>(minPath, matchExprData),
                      createTypeEqualityPredicate(pExpCtx, matchExprPath)));

        case MatchExpression::LTE:
            // For $lte, make a $lte predicate against 'control.min'. In addition, if the comparison
            // is against the 'time' field, include a predicate against the _id field which is
            // converted to the maximum for the corresponding range of ObjectIds. In
            // addition, we include a {'control.min' : {$lte: 'time - bucketMaxSpanSeconds'}}
            // predicate which will be helpful in reducing bounds for index scans on 'time' field
            // and routing on mongos.
            return isTimeField
                ? makePredicate(
                      MatchExprPredicate<InternalExprLTEMatchExpression>(minPath, matchExprData),
                      MatchExprPredicate<LTEMatchExpression, Value>(
                          kBucketIdFieldName,
                          constructObjectIdValue<LTEMatchExpression>(matchExprData,
                                                                     bucketMaxSpanSeconds)))
                : std::make_unique<OrMatchExpression>(makeVector<std::unique_ptr<MatchExpression>>(
                      std::make_unique<InternalExprLTEMatchExpression>(minPath, matchExprData),
                      createTypeEqualityPredicate(pExpCtx, matchExprPath)));

        default:
            MONGO_UNREACHABLE_TASSERT(5348302);
    }

    MONGO_UNREACHABLE_TASSERT(5348303);
}

std::unique_ptr<MatchExpression>
DocumentSourceInternalUnpackBucket::createPredicatesOnBucketLevelField(
    const MatchExpression* matchExpr) const {
    if (matchExpr->matchType() == MatchExpression::AND) {
        auto nextAnd = static_cast<const AndMatchExpression*>(matchExpr);
        auto andMatchExpr = std::make_unique<AndMatchExpression>();

        for (size_t i = 0; i < nextAnd->numChildren(); i++) {
            if (auto child = createPredicatesOnBucketLevelField(nextAnd->getChild(i))) {
                andMatchExpr->add(std::move(child));
            }
        }
        if (andMatchExpr->numChildren() > 0) {
            return andMatchExpr;
        }
    } else if (ComparisonMatchExpression::isComparisonMatchExpression(matchExpr)) {
        return createComparisonPredicate(static_cast<const ComparisonMatchExpression*>(matchExpr),
                                         _bucketUnpacker.bucketSpec(),
                                         _bucketMaxSpanSeconds,
                                         pExpCtx->collationMatchesDefault,
                                         pExpCtx);
    } else if (matchExpr->matchType() == MatchExpression::GEO) {
        auto& geoExpr = static_cast<const GeoMatchExpression*>(matchExpr)->getGeoExpression();
        if (geoExpr.getPred() == GeoExpression::WITHIN) {
            return std::make_unique<InternalBucketGeoWithinMatchExpression>(
                geoExpr.getGeometryPtr(), geoExpr.getField());
        }
    }

    return nullptr;
}

std::pair<boost::intrusive_ptr<DocumentSourceMatch>, boost::intrusive_ptr<DocumentSourceMatch>>
DocumentSourceInternalUnpackBucket::splitMatchOnMetaAndRename(
    boost::intrusive_ptr<DocumentSourceMatch> match) {
    if (auto&& metaField = _bucketUnpacker.bucketSpec().metaField) {
        return std::move(*match).extractMatchOnFieldsAndRemainder(
            {*metaField}, {{*metaField, timeseries::kBucketMetaFieldName.toString()}});
    }
    return {nullptr, match};
}

std::pair<BSONObj, bool> DocumentSourceInternalUnpackBucket::extractProjectForPushDown(
    DocumentSource* src) const {
    if (auto nextProject = dynamic_cast<DocumentSourceSingleDocumentTransformation*>(src);
        _bucketUnpacker.bucketSpec().metaField && nextProject &&
        nextProject->getType() == TransformerInterface::TransformerType::kExclusionProjection) {
        return nextProject->extractProjectOnFieldAndRename(
            _bucketUnpacker.bucketSpec().metaField.get(), timeseries::kBucketMetaFieldName);
    }

    return {BSONObj{}, false};
}

std::pair<bool, Pipeline::SourceContainer::iterator>
DocumentSourceInternalUnpackBucket::rewriteGroupByMinMax(Pipeline::SourceContainer::iterator itr,
                                                         Pipeline::SourceContainer* container) {
    const auto* groupPtr = dynamic_cast<DocumentSourceGroup*>(std::next(itr)->get());
    if (groupPtr == nullptr) {
        return {};
    }

    const auto& idFields = groupPtr->getIdFields();
    if (idFields.size() != 1 || !_bucketUnpacker.bucketSpec().metaField.has_value()) {
        return {};
    }

    const auto& exprId = idFields.cbegin()->second;
    const auto* exprIdPath = dynamic_cast<const ExpressionFieldPath*>(exprId.get());
    if (exprIdPath == nullptr) {
        return {};
    }

    const auto& idPath = exprIdPath->getFieldPath();
    if (idPath.getPathLength() < 2 ||
        idPath.getFieldName(1) != _bucketUnpacker.bucketSpec().metaField.get()) {
        return {};
    }

    bool suitable = true;
    std::vector<AccumulationStatement> accumulationStatements;
    for (const AccumulationStatement& stmt : groupPtr->getAccumulatedFields()) {
        const auto op = stmt.expr.name;
        const bool isMin = op == "$min";
        const bool isMax = op == "$max";

        // Rewrite is valid only for min and max aggregates.
        if (!isMin && !isMax) {
            suitable = false;
            break;
        }

        const auto* exprArg = stmt.expr.argument.get();
        if (const auto* exprArgPath = dynamic_cast<const ExpressionFieldPath*>(exprArg)) {
            const auto& path = exprArgPath->getFieldPath();
            if (path.getPathLength() <= 1 ||
                path.getFieldName(1) == _bucketUnpacker.bucketSpec().timeField) {
                // Rewrite not valid for time field. We want to eliminate the bucket
                // unpack stage here.
                suitable = false;
                break;
            }

            // Update aggregates to reference the control field.
            std::ostringstream os;
            if (isMin) {
                os << timeseries::kControlMinFieldNamePrefix;
            } else {
                os << timeseries::kControlMaxFieldNamePrefix;
            }

            for (size_t index = 1; index < path.getPathLength(); index++) {
                if (index > 1) {
                    os << ".";
                }
                os << path.getFieldName(index);
            }

            const auto& newExpr = ExpressionFieldPath::createPathFromString(
                pExpCtx.get(), os.str(), pExpCtx->variablesParseState);

            AccumulationExpression accExpr = stmt.expr;
            accExpr.argument = newExpr;
            accumulationStatements.emplace_back(stmt.fieldName, std::move(accExpr));
        }
    }

    if (suitable) {
        std::ostringstream os;
        os << timeseries::kBucketMetaFieldName;
        for (size_t index = 2; index < idPath.getPathLength(); index++) {
            os << "." << idPath.getFieldName(index);
        }
        auto exprId1 = ExpressionFieldPath::createPathFromString(
            pExpCtx.get(), os.str(), pExpCtx->variablesParseState);

        auto newGroup = DocumentSourceGroup::create(pExpCtx,
                                                    std::move(exprId1),
                                                    std::move(accumulationStatements),
                                                    groupPtr->getMaxMemoryUsageBytes());

        // Erase current stage and following group stage, and replace with updated
        // group.
        container->erase(std::next(itr));
        *itr = std::move(newGroup);

        if (itr == container->begin()) {
            // Optimize group stage.
            return {true, itr};
        } else {
            // Give chance of the previous stage to optimize against group stage.
            return {true, std::prev(itr)};
        }
    }

    return {};
}

Pipeline::SourceContainer::iterator DocumentSourceInternalUnpackBucket::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // Some optimizations may not be safe to do if we have computed the metaField via an $addFields
    // or a computed $project. We won't do those optimizations if 'haveComputedMetaField' is true.
    bool haveComputedMetaField = _bucketUnpacker.bucketSpec().metaField &&
        fieldIsComputed(_bucketUnpacker.bucketSpec(), _bucketUnpacker.bucketSpec().metaField.get());

    // Before any other rewrites for the current stage, consider reordering with $sort.
    if (auto sortPtr = dynamic_cast<DocumentSourceSort*>(std::next(itr)->get())) {
        if (auto metaField = _bucketUnpacker.bucketSpec().metaField;
            metaField && !haveComputedMetaField) {
            if (checkMetadataSortReorder(sortPtr->getSortKeyPattern(), metaField.get())) {
                // We have a sort on metadata field following this stage. Reorder the two stages
                // and return a pointer to the preceding stage.
                auto sortForReorder = createMetadataSortForReorder(*sortPtr);

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

    // Optimize the pipeline after this stage to merge $match stages and push them forward.
    if (!_optimizedEndOfPipeline) {
        _optimizedEndOfPipeline = true;
        Pipeline::optimizeEndOfPipeline(itr, container);

        if (std::next(itr) == container->end()) {
            return container->end();
        }
    }
    {
        // Check if we can avoid unpacking if we have a group stage with min/max aggregates.
        auto [success, result] = rewriteGroupByMinMax(itr, container);
        if (success) {
            return result;
        }
    }

    {
        // Check if the rest of the pipeline needs any fields. For example we might only be
        // interested in $count.
        auto deps = Pipeline::getDependenciesForContainer(
            pExpCtx, Pipeline::SourceContainer{std::next(itr), container->end()}, boost::none);
        if (deps.hasNoRequirements()) {
            _bucketUnpacker.setBucketSpecAndBehavior({_bucketUnpacker.bucketSpec().timeField,
                                                      _bucketUnpacker.bucketSpec().metaField,
                                                      {}},
                                                     BucketUnpacker::Behavior::kInclude);

            // Keep going for next optimization.
        }
    }

    // Attempt to push predicates on the metaField past $_internalUnpackBucket.
    if (auto nextMatch = dynamic_cast<DocumentSourceMatch*>(std::next(itr)->get());
        nextMatch && !haveComputedMetaField) {
        auto [metaMatch, remainingMatch] = splitMatchOnMetaAndRename(nextMatch);

        // The old $match can be removed and potentially replaced with 'remainingMatch'.
        container->erase(std::next(itr));
        if (remainingMatch) {
            container->insert(std::next(itr), remainingMatch);
        }

        // 'metaMatch' can be pushed down and given a chance to optimize with other stages.
        if (metaMatch) {
            container->insert(itr, metaMatch);
            return std::prev(itr) == container->begin() ? std::prev(itr)
                                                        : std::prev(std::prev(itr));
        }
    }

    // Attempt to push geoNear on the metaField past $_internalUnpackBucket.
    if (auto nextNear = dynamic_cast<DocumentSourceGeoNear*>(std::next(itr)->get())) {
        // Currently we only support geo indexes on the meta field, and we enforce this by
        // requiring the key field to be set so we can check before we try to look up indexes.
        auto keyField = nextNear->getKeyField();
        uassert(5892921,
                "Must specify 'key' option for $geoNear on a time-series collection",
                keyField);

        auto metaField = _bucketUnpacker.bucketSpec().metaField;
        uassert(
            4581294,
            "Must specify part of metadata field as 'key' for $geoNear on a time-series collection",
            metaField && *metaField == keyField->front());

        // Currently we do not support query for $geoNear on a bucket
        uassert(
            1938439,
            "Must not specify 'query' for $geoNear on a time-series collection; use $match instead",
            nextNear->getQuery().binaryEqual(BSONObj()));

        // Make sure we actually re-write the key field for the buckets collection so we can
        // locate the index.
        static const FieldPath baseMetaFieldPath{timeseries::kBucketMetaFieldName};
        nextNear->setKeyField(keyField->getPathLength() > 1
                                  ? baseMetaFieldPath.concat(keyField->tail())
                                  : baseMetaFieldPath);

        // Save the source, remove it, and then push it down.
        auto source = *std::next(itr);
        container->erase(std::next(itr));
        container->insert(itr, source);
        return std::prev(itr) == container->begin() ? std::prev(itr) : std::prev(std::prev(itr));
    }

    // Attempt to map predicates on bucketed fields to predicates on the control field.
    if (auto nextMatch = dynamic_cast<DocumentSourceMatch*>(std::next(itr)->get());
        nextMatch && !_triedBucketLevelFieldsPredicatesPushdown) {
        _triedBucketLevelFieldsPredicatesPushdown = true;

        if (auto match = createPredicatesOnBucketLevelField(nextMatch->getMatchExpression())) {
            BSONObjBuilder bob;
            match->serialize(&bob);
            container->insert(itr, DocumentSourceMatch::create(bob.obj(), pExpCtx));

            // Give other stages a chance to optimize with the new $match.
            return std::prev(itr) == container->begin() ? std::prev(itr)
                                                        : std::prev(std::prev(itr));
        }
    }

    // Attempt to push down a $project on the metaField past $_internalUnpackBucket.
    if (!haveComputedMetaField) {
        if (auto [metaProject, deleteRemainder] = extractProjectForPushDown(std::next(itr)->get());
            !metaProject.isEmpty()) {
            container->insert(itr,
                              DocumentSourceProject::createFromBson(
                                  BSON("$project" << metaProject).firstElement(), getContext()));

            if (deleteRemainder) {
                // We have pushed down the entire $project. Remove the old $project from the
                // pipeline, then attempt to optimize this stage again.
                container->erase(std::next(itr));
                return std::prev(itr) == container->begin() ? std::prev(itr)
                                                            : std::prev(std::prev(itr));
            }
        }
    }

    // Attempt to extract computed meta projections from subsequent $project, $addFields, or $set
    // and push them before the $_internalunpackBucket.
    if (pushDownComputedMetaProjection(itr, container)) {
        // We've pushed down and removed a stage after this one. Try to optimize the new stage.
        return std::prev(itr) == container->begin() ? std::prev(itr) : std::prev(std::prev(itr));
    }

    // Attempt to build a $project based on dependency analysis or extract one from the pipeline. We
    // can internalize the result so we can handle projections during unpacking.
    if (!_triedInternalizeProject) {
        if (auto [project, isInclusion] = extractOrBuildProjectToInternalize(itr, container);
            !project.isEmpty()) {
            _triedInternalizeProject = true;
            internalizeProject(project, isInclusion);

            // We may have removed a $project after this stage, so we try to optimize this stage
            // again.
            return itr;
        }
    }

    return container->end();
}
}  // namespace mongo
