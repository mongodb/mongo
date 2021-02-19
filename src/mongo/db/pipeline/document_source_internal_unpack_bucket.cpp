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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"

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
 * Determine which fields can be moved out of 'src', if it is a $project, and into
 * $_internalUnpackBucket. Return the set of those field names, the remaining $project, and a bool
 * indicating its type.
 *
 * For example, given {$project: {a: 1, b.c: 1, _id: 0}}, return the set ['a', 'b'], the project
 * {a: 1, b.c: 1}, and 'true'. In this case, '_id' does not need to be included in either the set or
 * the project, since the unpack will exclude any field not explicitly included in its field set.
 */
auto extractInternalizableFieldsRemainingProjectAndType(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, DocumentSource* src) {
    auto eraseIdIf = [](std::set<std::string>&& set, auto&& cond) {
        if (cond)
            set.erase("_id");
        return std::move(set);
    };

    if (auto [remainingProj, isInclusion] = getIncludeExcludeProjectAndType(src);
        remainingProj.isEmpty()) {
        // There is nothing to internalize.
        return std::tuple{std::set<std::string>{}, remainingProj, isInclusion};
    } else if (canInternalizeProjectObj(remainingProj)) {
        // We can internalize the whole object, so 'remainingProject' should be empty.
        return std::tuple{eraseIdIf(remainingProj.getFieldNames<std::set<std::string>>(),
                                    remainingProj.getBoolField("_id") != isInclusion),
                          BSONObj{},
                          isInclusion};
    } else if (isInclusion) {
        // We can't internalize the whole inclusion, so we must leave it unmodified in the pipeline
        // for correctness. We do dependency analysis to get an internalizable $project to ensure
        // we're handling dotted fields or fields referenced inside 'src'.
        Pipeline::SourceContainer projectStage{src};
        auto dependencyProj =
            Pipeline::getDependenciesForContainer(expCtx, projectStage, boost::none)
                .toProjectionWithoutMetadata(DepsTracker::TruncateToRootLevel::yes);
        return std::tuple{eraseIdIf(dependencyProj.getFieldNames<std::set<std::string>>(),
                                    dependencyProj.getIntField("_id") != 1),
                          remainingProj,
                          isInclusion};
    } else {
        // We can internalize any fields that are not dotted, and leave the rest in 'remainingProj'.
        std::set<std::string> topLevelFields;
        std::for_each(remainingProj.begin(), remainingProj.end(), [&topLevelFields](auto&& elem) {
            // '_id' may be included in this exclusion. If so, don't add it to 'topLevelFields'.
            if (elem.isBoolean() && !elem.Bool()) {
                topLevelFields.emplace(elem.fieldName());
            }
        });
        return std::tuple{topLevelFields, remainingProj.removeFields(topLevelFields), isInclusion};
    }
}

// Optimize the given pipeline after the $_internalUnpackBucket stage.
void optimizeEndOfPipeline(Pipeline::SourceContainer::iterator itr,
                           Pipeline::SourceContainer* container) {
    // We must create a new SourceContainer representing the subsection of the pipeline we wish to
    // optimize, since otherwise calls to optimizeAt() will overrun these limits.
    auto endOfPipeline = Pipeline::SourceContainer(std::next(itr), container->end());
    Pipeline::optimizeContainer(&endOfPipeline);
    container->erase(std::next(itr), container->end());
    container->splice(std::next(itr), endOfPipeline);
}
}  // namespace

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
        // The spec indicates that there should be a metadata region. Missing metadata in this case
        // is expressed with null, so the field is expected to be present. We also disallow
        // undefined since the undefined BSON type is deprecated.
        uassert(5369600,
                "The $_internalUnpackBucket stage requires metadata to be present in a bucket if "
                "metaField parameter is provided",
                (_metaValue.type() != BSONType::Undefined) && _metaValue);
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
        auto found = _spec.fieldSet.find(colName.toString()) != _spec.fieldSet.end();
        if ((_unpackerBehavior == Behavior::kInclude) == found) {
            _fieldIters.push_back({colName.toString(), BSONObjIterator{elem.Obj()}});
        }
    }
}

void BucketUnpacker::setBucketSpecAndBehavior(BucketSpec&& bucketSpec, Behavior behavior) {
    _includeMetaField = eraseMetaFromFieldSetAndDetermineIncludeMeta(behavior, &bucketSpec);
    _includeTimeField = determineIncludeTimeField(behavior, &bucketSpec);
    _unpackerBehavior = behavior;
    _spec = std::move(bucketSpec);
}

Document BucketUnpacker::getNext() {
    invariant(hasNext());

    auto measurement = MutableDocument{};
    auto&& timeElem = _timeFieldIter->next();
    if (_includeTimeField) {
        measurement.addField(_spec.timeField, Value{timeElem});
    }

    if (_includeMetaField && !_metaValue.isNull()) {
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

DocumentSourceInternalUnpackBucket::DocumentSourceInternalUnpackBucket(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, BucketUnpacker bucketUnpacker)
    : DocumentSource(kStageName, expCtx), _bucketUnpacker(std::move(bucketUnpacker)) {}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalUnpackBucket::createFromBson(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5346500,
            "$_internalUnpackBucket specification must be an object",
            specElem.type() == Object);

    BucketUnpacker::Behavior unpackerBehavior;
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

    // Check that none of the required arguments are missing.
    uassert(5346507,
            "The $_internalUnpackBucket stage requries an include/exclude parameter",
            hasIncludeExclude);

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

Value DocumentSourceInternalUnpackBucket::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
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
    return Value(DOC(getSourceName() << out.freeze()));
}

DocumentSource::GetNextResult DocumentSourceInternalUnpackBucket::doGetNext() {
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

void DocumentSourceInternalUnpackBucket::internalizeProject(Pipeline::SourceContainer::iterator itr,
                                                            Pipeline::SourceContainer* container) {
    if (std::next(itr) == container->end() || !_bucketUnpacker.bucketSpec().fieldSet.empty()) {
        // There is no project to internalize or there are already fields being included/excluded.
        return;
    }
    auto [fields, remainingProject, isInclusion] =
        extractInternalizableFieldsRemainingProjectAndType(getContext(), std::next(itr)->get());
    if (fields.empty()) {
        return;
    }

    // Update 'bucketUnpacker' state with the new fields and behavior. Update 'container' state by
    // removing the old $project and potentially replacing it with 'remainingProject'.
    auto spec = _bucketUnpacker.bucketSpec();
    spec.fieldSet = std::move(fields);
    _bucketUnpacker.setBucketSpecAndBehavior(std::move(spec),
                                             isInclusion ? BucketUnpacker::Behavior::kInclude
                                                         : BucketUnpacker::Behavior::kExclude);
    container->erase(std::next(itr));
    if (!remainingProject.isEmpty()) {
        container->insert(std::next(itr),
                          DocumentSourceProject::createFromBson(
                              BSON("$project" << remainingProject).firstElement(), getContext()));
    }
}

/**
 * Given a source container and an iterator pointing to the $_internalUnpackBucket, builds a
 * projection that can be entirely moved into the $_internalUnpackBucket, following these rules:
 *    1. If there is an inclusion projection immediately after which can be internalized, an empty
         BSONObj will be returned.
 *    2. Otherwise, if there is a finite dependency set for the rest of the pipeline, an inclusion
 *       $project representing it and containing only root-level fields will be returned. An
 *       inclusion $project will be returned here even if there is a viable exclusion $project
 *       next in the pipeline.
 *    3. Otherwise, an empty BSONObj will be returned.
 */
BSONObj DocumentSourceInternalUnpackBucket::buildProjectToInternalize(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) const {
    if (std::next(itr) == container->end()) {
        return BSONObj{};
    }

    // Check for a viable inclusion $project after the $_internalUnpackBucket. This handles case 1.
    if (auto [project, isInclusion] = getIncludeExcludeProjectAndType(std::next(itr)->get());
        isInclusion && !project.isEmpty() && canInternalizeProjectObj(project)) {
        return BSONObj{};
    }

    // Attempt to get an inclusion $project representing the root-level dependencies of the pipeline
    // after the $_internalUnpackBucket. If this $project is not empty, then the dependency set was
    // finite, and we are in case 2. If it is empty, we're in case 3. There may be a viable
    // exclusion $project in the pipeline, but we don't need to check for it here.
    Pipeline::SourceContainer restOfPipeline(std::next(itr), container->end());
    auto deps = Pipeline::getDependenciesForContainer(getContext(), restOfPipeline, boost::none);
    return deps.toProjectionWithoutMetadata(DepsTracker::TruncateToRootLevel::yes);
}

Pipeline::SourceContainer::iterator DocumentSourceInternalUnpackBucket::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // Attempt to build an internalizable $project based on dependency analysis.
    if (auto projObj = buildProjectToInternalize(itr, container); !projObj.isEmpty()) {
        // Give the new $project a chance to be optimized before internalizing.
        container->insert(std::next(itr),
                          DocumentSourceProject::createFromBson(
                              BSON("$project" << projObj).firstElement(), pExpCtx));
    }

    // Optimize the pipeline after the $unpackBucket.
    optimizeEndOfPipeline(std::next(itr), container);

    // If there is a $project following the $_internalUnpackBucket, internalize as much of it as
    // possible, and update the state of 'container' and '_bucketUnpacker' to reflect this.
    internalizeProject(itr, container);

    return container->end();
}
}  // namespace mongo
