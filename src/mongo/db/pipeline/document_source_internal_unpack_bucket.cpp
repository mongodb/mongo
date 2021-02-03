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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/logv2/log.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE(_internalUnpackBucket,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalUnpackBucket::createFromBson,
                         LiteParsedDocumentSource::AllowedWithApiStrict::kInternal);

namespace {
/**
 * Removes metaField from the field set and returns a boolean indicating whether metaField should be
 * included in the materialized measurements. Always returns false if metaField does not exist.
 */
auto eraseMetaFromFieldSetAndDetermineIncludeMeta(BucketUnpacker::Behavior unpackerBehavior,
                                                  BucketSpec* bucketSpec) {
    if (!bucketSpec->metaField) {
        return false;
    } else if (auto itr = bucketSpec->fieldSet.find(*bucketSpec->metaField);
               itr != bucketSpec->fieldSet.end()) {
        bucketSpec->fieldSet.erase(itr);
        return unpackerBehavior == BucketUnpacker::Behavior::kInclude;
    } else {
        return unpackerBehavior == BucketUnpacker::Behavior::kExclude;
    }
}

/**
 * Determine if timestamp values should be included in the materialized measurements.
 */
auto determineIncludeTimeField(BucketUnpacker::Behavior unpackerBehavior, BucketSpec* bucketSpec) {
    return (unpackerBehavior == BucketUnpacker::Behavior::kInclude) ==
        (bucketSpec->fieldSet.find(bucketSpec->timeField) != bucketSpec->fieldSet.end());
}

/**
 * Determine if an arbitrary field should be included in the materialized measurements.
 */
auto determineIncludeField(StringData fieldName,
                           BucketUnpacker::Behavior unpackerBehavior,
                           const BucketSpec& bucketSpec) {
    return (unpackerBehavior == BucketUnpacker::Behavior::kInclude) ==
        (bucketSpec.fieldSet.find(fieldName.toString()) != bucketSpec.fieldSet.end());
}

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

// Optimize the given pipeline after the $_internalUnpackBucket stage pointed to by 'itr'.
void optimizeEndOfPipeline(Pipeline::SourceContainer::iterator itr,
                           Pipeline::SourceContainer* container) {
    // We must create a new SourceContainer representing the subsection of the pipeline we wish to
    // optimize, since otherwise calls to optimizeAt() will overrun these limits.
    auto endOfPipeline = Pipeline::SourceContainer(std::next(itr), container->end());
    Pipeline::optimizeContainer(&endOfPipeline);
    container->erase(std::next(itr), container->end());
    container->splice(std::next(itr), endOfPipeline);
}

/**
 * Creates an ObjectId initialized with an appropriate timestamp corresponding to 'matchExpr' and
 * returns it as a Value.
 */
Value constructObjectIdValue(const ComparisonMatchExpression* matchExpr) {
    // An ObjectId consists of a 4-byte timestamp, as well as a unique value and a counter, thus
    // two ObjectIds initialized with the same date will have different values. To ensure that we
    // do not incorrectly include or exclude any buckets, depending on the operator we will
    // construct either the largest or the smallest ObjectId possible with the corresponding date.
    OID oid;
    if (matchExpr->getData().type() == BSONType::Date) {
        switch (matchExpr->matchType()) {
            case MatchExpression::LT: {
                oid.init(matchExpr->getData().date(), false /* min */);
                break;
            }
            case MatchExpression::LTE:
            case MatchExpression::EQ: {
                oid.init(matchExpr->getData().date(), true /* max */);
                break;
            }
            default:
                // We will only perform this optimization with query operators $lt, $lte and $eq.
                MONGO_UNREACHABLE_TASSERT(5375801);
        }
    }
    // If the query operand is not of type Date, the original query will not match on any documents
    // because documents in a time-series collection must have a timeField of type Date. We will
    // make this case faster by keeping the ObjectId as the lowest possible value so as to
    // eliminate all buckets.
    return Value(oid);
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
        auto updatedFieldPath = FieldPath(BucketUnpacker::kBucketMetaFieldName);
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
}  // namespace

// Calculates the number of measurements in a bucket given the 'targetTimestampObjSize' using the
// 'BucketUnpacker::kTimestampObjSizeTable' table. If the 'targetTimestampObjSize' hits a record in
// the table, this helper returns the measurement count corresponding to the table record.
// Otherwise, the 'targetTimestampObjSize' is used to probe the table for the smallest {b_i, S_i}
// pair such that 'targetTimestampObjSize' < S_i. Once the interval is found, the upper bound of the
// pair for the interval is computed and then linear interpolation is used to compute the
// measurement count corresponding to the 'targetTimestampObjSize' provided.
int BucketUnpacker::computeMeasurementCount(int targetTimestampObjSize) {
    auto currentInterval =
        std::find_if(std::begin(BucketUnpacker::kTimestampObjSizeTable),
                     std::end(BucketUnpacker::kTimestampObjSizeTable),
                     [&](const auto& entry) { return targetTimestampObjSize <= entry.second; });

    if (currentInterval->second == targetTimestampObjSize) {
        return currentInterval->first;
    }
    // This points to the first interval larger than the target 'targetTimestampObjSize', the actual
    // interval that will cover the object size is the interval before the current one.
    tassert(5422104,
            "currentInterval should not point to the first table entry",
            currentInterval > BucketUnpacker::kTimestampObjSizeTable.begin());
    --currentInterval;

    auto nDigitsInRowKey = 1 + (currentInterval - BucketUnpacker::kTimestampObjSizeTable.begin());

    return currentInterval->first +
        ((targetTimestampObjSize - currentInterval->second) / (10 + nDigitsInRowKey));
}

void BucketUnpacker::reset(BSONObj&& bucket) {
    _fieldIters.clear();
    _timeFieldIter = boost::none;

    _bucket = std::move(bucket);
    uassert(5346510, "An empty bucket cannot be unpacked", !_bucket.isEmpty());
    tassert(5346701,
            "The $_internalUnpackBucket stage requires the bucket to be owned",
            _bucket.isOwned());

    auto&& dataRegion = _bucket.getField(kBucketDataFieldName).Obj();
    if (dataRegion.isEmpty()) {
        // If the data field of a bucket is present but it holds an empty object, there's nothing to
        // unpack.
        return;
    }

    auto&& timeFieldElem = dataRegion.getField(_spec.timeField);
    uassert(5346700,
            "The $_internalUnpackBucket stage requires the data region to have a timeField object",
            timeFieldElem);

    _timeFieldIter = BSONObjIterator{timeFieldElem.Obj()};

    _metaValue = _bucket[kBucketMetaFieldName];
    if (_spec.metaField) {
        // The spec indicates that there might be a metadata region. Missing metadata in
        // measurements is expressed with missing metadata in a bucket. But we disallow undefined
        // since the undefined BSON type is deprecated.
        uassert(5369600,
                "The $_internalUnpackBucket stage allows metadata to be absent or otherwise, it "
                "must not be the deprecated undefined bson type",
                !_metaValue || _metaValue.type() != BSONType::Undefined);
    } else {
        // If the spec indicates that the time series collection has no metadata field, then we
        // should not find a metadata region in the underlying bucket documents.
        uassert(5369601,
                "The $_internalUnpackBucket stage expects buckets to have missing metadata regions "
                "if the metaField parameter is not provided",
                !_metaValue);
    }

    // Walk the data region of the bucket, and decide if an iterator should be set up based on the
    // include or exclude case.
    for (auto&& elem : dataRegion) {
        auto& colName = elem.fieldNameStringData();
        if (colName == _spec.timeField) {
            // Skip adding a FieldIterator for the timeField since the timestamp value from
            // _timeFieldIter can be placed accordingly in the materialized measurement.
            continue;
        }

        // Includes a field when '_unpackerBehavior' is 'kInclude' and it's found in 'fieldSet' or
        // _unpackerBehavior is 'kExclude' and it's not found in 'fieldSet'.
        if (determineIncludeField(colName, _unpackerBehavior, _spec)) {
            _fieldIters.emplace_back(colName.toString(), BSONObjIterator{elem.Obj()});
        }
    }

    // Save the measurement count for the owned bucket.
    _numberOfMeasurements = computeMeasurementCount(timeFieldElem.objsize());
}

void BucketUnpacker::setBucketSpecAndBehavior(BucketSpec&& bucketSpec, Behavior behavior) {
    _includeMetaField = eraseMetaFromFieldSetAndDetermineIncludeMeta(behavior, &bucketSpec);
    _includeTimeField = determineIncludeTimeField(behavior, &bucketSpec);
    _unpackerBehavior = behavior;
    _spec = std::move(bucketSpec);
}

Document BucketUnpacker::getNext() {
    tassert(5422100, "'getNext()' was called after the bucket has been exhausted", hasNext());

    auto measurement = MutableDocument{};
    auto&& timeElem = _timeFieldIter->next();
    if (_includeTimeField) {
        measurement.addField(_spec.timeField, Value{timeElem});
    }

    // Includes metaField when we're instructed to do so and metaField value exists.
    if (_includeMetaField && _metaValue) {
        measurement.addField(*_spec.metaField, Value{_metaValue});
    }

    auto& currentIdx = timeElem.fieldNameStringData();
    for (auto&& [colName, colIter] : _fieldIters) {
        if (auto&& elem = *colIter; colIter.more() && elem.fieldNameStringData() == currentIdx) {
            measurement.addField(colName, Value{elem});
            colIter.advance(elem);
        }
    }

    return measurement.freeze();
}

Document BucketUnpacker::extractSingleMeasurement(int j) {
    tassert(5422101,
            "'extractSingleMeasurment' expects j to be greater than or equal to zero and less than "
            "or equal to the number of measurements in a bucket",
            j >= 0 && j < _numberOfMeasurements);

    auto measurement = MutableDocument{};

    auto rowKey = std::to_string(j);
    auto targetIdx = StringData{rowKey};
    auto&& dataRegion = _bucket.getField(kBucketDataFieldName).Obj();

    if (_includeMetaField && !_metaValue.isNull()) {
        measurement.addField(*_spec.metaField, Value{_metaValue});
    }

    for (auto&& dataElem : dataRegion) {
        auto colName = dataElem.fieldNameStringData();
        if (!determineIncludeField(colName, _unpackerBehavior, _spec)) {
            continue;
        }
        auto value = dataElem[targetIdx];
        if (value) {
            measurement.addField(dataElem.fieldNameStringData(), Value{value});
        }
    }

    return measurement.freeze();
}

DocumentSourceInternalUnpackBucket::DocumentSourceInternalUnpackBucket(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, BucketUnpacker bucketUnpacker)
    : DocumentSource(kStageName, expCtx), _bucketUnpacker(std::move(bucketUnpacker)) {}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalUnpackBucket::createFromBson(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5346500,
            "$_internalUnpackBucket specification must be an object",
            specElem.type() == Object);

    // If neither "include" nor "exclude" is specified, the default is "exclude": [] and
    // if that's the case, no field will be added to 'bucketSpec.fieldSet' in the for-loop below.
    BucketUnpacker::Behavior unpackerBehavior = BucketUnpacker::Behavior::kExclude;
    BucketSpec bucketSpec;
    auto hasIncludeExclude = false;
    std::vector<std::string> fields;
    for (auto&& elem : specElem.embeddedObject()) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == "include" || fieldName == "exclude") {
            uassert(5408000,
                    "The $_internalUnpackBucket stage expects at most one of include/exclude "
                    "parameters to be specified",
                    !hasIncludeExclude);

            uassert(5346501,
                    "include or exclude field must be an array",
                    elem.type() == BSONType::Array);

            for (auto&& elt : elem.embeddedObject()) {
                uassert(5346502,
                        "include or exclude field element must be a string",
                        elt.type() == BSONType::String);
                auto field = elt.valueStringData();
                uassert(5346503,
                        "include or exclude field element must be a single-element field path",
                        field.find('.') == std::string::npos);
                bucketSpec.fieldSet.emplace(field);
            }
            unpackerBehavior = fieldName == "include" ? BucketUnpacker::Behavior::kInclude
                                                      : BucketUnpacker::Behavior::kExclude;
            hasIncludeExclude = true;
        } else if (fieldName == kTimeFieldName) {
            uassert(5346504, "timeField field must be a string", elem.type() == BSONType::String);
            bucketSpec.timeField = elem.str();
        } else if (fieldName == kMetaFieldName) {
            uassert(5346505,
                    str::stream() << "metaField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::String);
            bucketSpec.metaField = elem.str();
        } else {
            uasserted(5346506,
                      str::stream()
                          << "unrecognized parameter to $_internalUnpackBucket: " << fieldName);
        }
    }

    uassert(5346508,
            "The $_internalUnpackBucket stage requires a timeField parameter",
            specElem[kTimeFieldName].ok());

    auto includeTimeField = determineIncludeTimeField(unpackerBehavior, &bucketSpec);

    auto includeMetaField =
        eraseMetaFromFieldSetAndDetermineIncludeMeta(unpackerBehavior, &bucketSpec);

    return make_intrusive<DocumentSourceInternalUnpackBucket>(
        expCtx,
        BucketUnpacker{
            std::move(bucketSpec), unpackerBehavior, includeTimeField, includeMetaField});
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
    out.addField(behavior, Value{fields});
    out.addField(kTimeFieldName, Value{spec.timeField});
    if (spec.metaField) {
        out.addField(kMetaFieldName, Value{*spec.metaField});
    }

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

DocumentSource::GetNextResult
DocumentSourceInternalUnpackBucket::sampleUniqueMeasurementFromBuckets() {
    const auto kMaxAttempts = 100;
    for (auto attempt = 0; attempt < kMaxAttempts; ++attempt) {
        auto randResult = pSource->getNext();
        switch (randResult.getStatus()) {
            case GetNextResult::ReturnStatus::kAdvanced: {
                auto bucket = randResult.getDocument().toBson();
                _bucketUnpacker.reset(std::move(bucket));

                auto& prng = pExpCtx->opCtx->getClient()->getPrng();
                auto j = prng.nextInt64(_bucketMaxCount);

                if (j < _bucketUnpacker.numberOfMeasurements()) {
                    auto sampledDocument = _bucketUnpacker.extractSingleMeasurement(j);

                    auto bucketId = _bucketUnpacker.bucket()[BucketUnpacker::kBucketIdFieldName];
                    auto bucketIdMeasurementIdxKey = SampledMeasurementKey{bucketId.OID(), j};

                    if (_seenSet.insert(std::move(bucketIdMeasurementIdxKey)).second) {
                        _nSampledSoFar++;
                        return sampledDocument;
                    } else {
                        LOGV2_DEBUG(
                            5422102,
                            1,
                            "$_internalUnpackBucket optimized for sample saw duplicate measurement",
                            "measurementIndex"_attr = j,
                            "bucketId"_attr = bucketId);
                    }
                }
                break;
            }
            case GetNextResult::ReturnStatus::kPauseExecution: {
                // This state should never be reached since the input stage is a random cursor.
                MONGO_UNREACHABLE;
            }
            case GetNextResult::ReturnStatus::kEOF: {
                return randResult;
            }
        }
    }
    uasserted(5422103,
              str::stream()
                  << "$_internalUnpackBucket stage could not find a non-duplicate document after "
                  << kMaxAttempts
                  << " attempts while using a random cursor. This is likely a "
                     "sporadic failure, please try again");
}

DocumentSource::GetNextResult DocumentSourceInternalUnpackBucket::doGetNext() {
    // If the '_sampleSize' member is present, then the stage will produce randomly sampled
    // documents from buckets.
    if (_sampleSize) {
        if (_nSampledSoFar >= _sampleSize) {
            return GetNextResult::makeEOF();
        }
        return sampleUniqueMeasurementFromBuckets();
    }

    // Otherwise, fallback to unpacking every measurement in all buckets until the child stage is
    // exhausted.
    if (_bucketUnpacker.hasNext()) {
        return _bucketUnpacker.getNext();
    }

    auto nextResult = pSource->getNext();
    if (nextResult.isAdvanced()) {
        auto bucket = nextResult.getDocument().toBson();
        _bucketUnpacker.reset(std::move(bucket));
        uassert(
            5346509,
            str::stream() << "A bucket with _id "
                          << _bucketUnpacker.bucket()[BucketUnpacker::kBucketIdFieldName].toString()
                          << " contains an empty data region",
            _bucketUnpacker.hasNext());
        return _bucketUnpacker.getNext();
    }

    return nextResult;
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

std::unique_ptr<MatchExpression> createComparisonPredicate(
    const ComparisonMatchExpression* matchExpr, const BucketSpec& bucketSpec) {
    auto path = matchExpr->path();
    auto rhs = matchExpr->getData();

    // The control field's min and max are chosen using a field-order insensitive comparator, while
    // MatchExpressions use a comparator that treats field-order as significant. Because of this we
    // will not perform this optimization on queries with operands of compound types.
    if (rhs.type() == BSONType::Object || rhs.type() == BSONType::Array) {
        return nullptr;
    }

    // MatchExpressions have special comparison semantics regarding null, in that {$eq: null} will
    // match all documents where the field is either null or missing. Because this is different
    // from both the comparison semantics that InternalExprComparison expressions and the control's
    // min and max fields use, we will not perform this optimization on queries with null operands.
    if (rhs.type() == BSONType::jstNULL) {
        return nullptr;
    }

    // We must avoid mapping predicates on the meta field onto the control field.
    if (bucketSpec.metaField &&
        (path == bucketSpec.metaField.get() ||
         expression::isPathPrefixOf(bucketSpec.metaField.get(), path))) {
        return nullptr;
    }

    switch (matchExpr->matchType()) {
        case MatchExpression::EQ: {
            auto andMatchExpr = std::make_unique<AndMatchExpression>();

            andMatchExpr->add(std::make_unique<InternalExprLTEMatchExpression>(
                str::stream() << DocumentSourceInternalUnpackBucket::kControlMinFieldNamePrefix
                              << path,
                rhs));
            andMatchExpr->add(std::make_unique<InternalExprGTEMatchExpression>(
                str::stream() << DocumentSourceInternalUnpackBucket::kControlMaxFieldNamePrefix
                              << path,
                rhs));

            if (path == bucketSpec.timeField) {
                andMatchExpr->add(std::make_unique<LTEMatchExpression>(
                    BucketUnpacker::kBucketIdFieldName, constructObjectIdValue(matchExpr)));
            }
            return andMatchExpr;
        }
        case MatchExpression::GT: {
            return std::make_unique<InternalExprGTMatchExpression>(
                str::stream() << DocumentSourceInternalUnpackBucket::kControlMaxFieldNamePrefix
                              << path,
                rhs);
        }
        case MatchExpression::GTE: {
            return std::make_unique<InternalExprGTEMatchExpression>(
                str::stream() << DocumentSourceInternalUnpackBucket::kControlMaxFieldNamePrefix
                              << path,
                rhs);
        }
        case MatchExpression::LT: {
            auto controlPred = std::make_unique<InternalExprLTMatchExpression>(
                str::stream() << DocumentSourceInternalUnpackBucket::kControlMinFieldNamePrefix
                              << path,
                rhs);
            if (path == bucketSpec.timeField) {
                auto andMatchExpr = std::make_unique<AndMatchExpression>();

                andMatchExpr->add(std::make_unique<LTMatchExpression>(
                    BucketUnpacker::kBucketIdFieldName, constructObjectIdValue(matchExpr)));
                andMatchExpr->add(controlPred.release());

                return andMatchExpr;
            }
            return controlPred;
        }
        case MatchExpression::LTE: {
            auto controlPred = std::make_unique<InternalExprLTEMatchExpression>(
                str::stream() << DocumentSourceInternalUnpackBucket::kControlMinFieldNamePrefix
                              << path,
                rhs);
            if (path == bucketSpec.timeField) {
                auto andMatchExpr = std::make_unique<AndMatchExpression>();

                andMatchExpr->add(std::make_unique<LTEMatchExpression>(
                    BucketUnpacker::kBucketIdFieldName, constructObjectIdValue(matchExpr)));
                andMatchExpr->add(controlPred.release());

                return andMatchExpr;
            }
            return controlPred;
        }
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
                                         _bucketUnpacker.bucketSpec());
    }

    return nullptr;
}

std::pair<boost::intrusive_ptr<DocumentSourceMatch>, boost::intrusive_ptr<DocumentSourceMatch>>
DocumentSourceInternalUnpackBucket::splitMatchOnMetaAndRename(
    boost::intrusive_ptr<DocumentSourceMatch> match) {
    if (auto&& metaField = _bucketUnpacker.bucketSpec().metaField) {
        return std::move(*match).extractMatchOnFieldsAndRemainder(
            {*metaField}, {{*metaField, BucketUnpacker::kBucketMetaFieldName.toString()}});
    }
    return {nullptr, match};
}

Pipeline::SourceContainer::iterator DocumentSourceInternalUnpackBucket::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // Before any other rewrites for the current stage, consider reordering with $sort.
    if (auto sortPtr = dynamic_cast<DocumentSourceSort*>(std::next(itr)->get())) {
        if (auto metaField = _bucketUnpacker.bucketSpec().metaField) {
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

    // Optimize the pipeline after the $unpackBucket.
    optimizeEndOfPipeline(itr, container);

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

    if (auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get())) {
        // Attempt to push predicates on the metaField past $_internalUnpackBucket.
        auto [metaMatch, remainingMatch] = splitMatchOnMetaAndRename(nextMatch);

        // 'metaMatch' is safe to move before $_internalUnpackBucket.
        if (metaMatch) {
            container->insert(itr, metaMatch);
        }

        // The old $match can be removed and potentially replaced with 'remainingMatch'.
        container->erase(std::next(itr));
        if (remainingMatch) {
            container->insert(std::next(itr), remainingMatch);

            // Attempt to map predicates on bucketed fields to predicates on the control field.
            if (auto match =
                    createPredicatesOnBucketLevelField(remainingMatch->getMatchExpression())) {
                BSONObjBuilder bob;
                match->serialize(&bob);
                container->insert(itr, DocumentSourceMatch::create(bob.obj(), pExpCtx));
            }
        }
    }

    // Attempt to build a $project based on dependency analysis or extract one from the pipeline. We
    // can internalize the result so we can handle projections during unpacking.
    if (auto [project, isInclusion] = extractOrBuildProjectToInternalize(itr, container);
        !project.isEmpty()) {
        internalizeProject(project, isInclusion);
    }

    // Optimize the prefix of the pipeline, now that all optimizations have been completed.
    optimizePrefix(itr, container);

    return container->end();
}
}  // namespace mongo
