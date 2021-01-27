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
                         DocumentSourceInternalUnpackBucket::createFromBson);

void BucketUnpacker::reset(Document&& bucket) {
    _fieldIters.clear();
    _timeFieldIter = boost::none;

    _bucket = std::move(bucket);
    uassert(5346510, "An empty bucket cannot be unpacked", !_bucket.empty());

    if (_bucket[kBucketDataFieldName].getDocument().empty()) {
        // If the data field of a bucket is present but it holds an empty object, there's nothing to
        // unpack.
        return;
    }

    _metaValue = _bucket[kBucketMetaFieldName];
    if (_spec.metaField) {
        // The spec indicates that there should be a metadata region. Missing metadata in this case
        // is expressed with null, so the field is expected to be present. We also disallow
        // undefined since the undefined BSON type is deprecated.
        uassert(5369600,
                "The $_internalUnpackBucket stage requires metadata to be present in a bucket if "
                "metaField parameter is provided",
                (_metaValue.getType() != BSONType::Undefined) && !_metaValue.missing());
    } else {
        // If the spec indicates that the time series collection has no metadata field, then we
        // should not find a metadata region in the underlying bucket documents.
        uassert(5369601,
                "The $_internalUnpackBucket stage expects buckets to have missing metadata regions "
                "if the metaField parameter is not provided",
                _metaValue.missing());
    }

    _timeFieldIter = _bucket[kBucketDataFieldName][_spec.timeField].getDocument().fieldIterator();

    // Walk the data region of the bucket, and decide if an iterator should be set up based on the
    // include or exclude case.
    auto colIter = _bucket[kBucketDataFieldName].getDocument().fieldIterator();
    while (colIter.more()) {
        auto&& [colName, colVal] = colIter.next();
        if (colName == _spec.timeField) {
            // Skip adding a FieldIterator for the timeField since the timestamp value from
            // _timeFieldIter can be placed accordingly in the materialized measurement.
            continue;
        }
        auto found = _spec.fieldSet.find(colName.toString()) != _spec.fieldSet.end();
        if ((_unpackerBehavior == Behavior::kInclude) == found) {
            _fieldIters.push_back({colName.toString(), colVal.getDocument().fieldIterator()});
        }
    }
}

Document BucketUnpacker::getNext() {
    invariant(hasNext());

    auto measurement = MutableDocument{};

    auto&& [currentIdx, timeVal] = _timeFieldIter->next();
    if (_includeTimeField) {
        measurement.addField(_spec.timeField, timeVal);
    }

    if (_includeMetaField && !_metaValue.nullish()) {
        measurement.addField(*_spec.metaField, _metaValue);
    }

    for (auto&& [colName, colIter] : _fieldIters) {
        if (colIter.more() && colIter.fieldName() == currentIdx) {
            auto&& [_, val] = colIter.next();
            measurement.addField(colName, val);
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

    // Determine if timestamp values should be included in the materialized measurements.
    auto includeTimeField = (unpackerBehavior == BucketUnpacker::Behavior::kInclude) ==
        (bucketSpec.fieldSet.find(bucketSpec.timeField) != bucketSpec.fieldSet.end());

    // Check the include/exclude set to determine if measurements should be materialized with
    // metadata.
    auto includeMetaField = false;
    if (bucketSpec.metaField) {
        const auto metaFieldIt = bucketSpec.fieldSet.find(*bucketSpec.metaField);
        auto found = metaFieldIt != bucketSpec.fieldSet.end();
        if (found) {
            bucketSpec.fieldSet.erase(metaFieldIt);
        }
        includeMetaField = (unpackerBehavior == BucketUnpacker::Behavior::kInclude) == found;
    }

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
        auto bucket = nextResult.getDocument();
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

namespace {
/**
 * A projection can be internalized if it does not include any dotted field names and if every field
 * name corresponds to a boolean value.
 */
bool canInternalizeProjectObj(const BSONObj& projObj) {
    const auto names = projObj.getFieldNames<std::set<std::string>>();
    return std::all_of(names.begin(), names.end(), [&projObj](auto&& name) {
        return name.find('.') == std::string::npos && projObj.getField(name).isBoolean();
    });
}

/**
 * If 'src' represents an inclusion or exclusion $project, return a BSONObj representing it, else
 * return an empty BSONObj. If 'inclusionOnly' is true, 'src' must be an inclusion $project.
 */
auto getProjectObj(DocumentSource* src, bool inclusionOnly) {
    if (const auto projStage = dynamic_cast<DocumentSourceSingleDocumentTransformation*>(src);
        projStage &&
        (projStage->getType() == TransformerInterface::TransformerType::kInclusionProjection ||
         (!inclusionOnly &&
          projStage->getType() == TransformerInterface::TransformerType::kExclusionProjection))) {
        return projStage->getTransformer().serializeTransformation(boost::none).toBson();
    }

    return BSONObj{};
}

/**
 * Given a source container and an iterator pointing to the $unpackBucket stage, builds a projection
 * BSONObj that can be entirely moved into the $unpackBucket stage, following these rules:
 *    1. If there is an inclusion projection immediately after the $unpackBucket which can be
 *       internalized, an empty BSONObj will be returned.
 *    2. Otherwise, if there is a finite dependency set for the rest of the pipeline, an inclusion
 *       $project representing it and containing only root-level fields will be returned. An
 *       inclusion $project will be returned here even if there is a viable exclusion $project
 *       next in the pipeline.
 *    3. Otherwise, an empty BSONObj will be returned.
 */
auto buildProjectToInternalize(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               Pipeline::SourceContainer::iterator itr,
                               Pipeline::SourceContainer* container) {
    // Check for a viable inclusion $project after the $unpackBucket. This handles case 1.
    if (auto projObj = getProjectObj(std::next(itr)->get(), true);
        !projObj.isEmpty() && canInternalizeProjectObj(projObj)) {
        return BSONObj{};
    }

    // If there is a finite dependency set for the pipeline after the $unpackBucket, obtain an
    // inclusion $project representing its root-level fields. Otherwise, we get an empty BSONObj.
    Pipeline::SourceContainer restOfPipeline(std::next(itr), container->end());
    auto deps = Pipeline::getDependenciesForContainer(expCtx, restOfPipeline, boost::none);
    auto dependencyProj = deps.toProjectionWithoutMetadata(DepsTracker::TruncateToRootLevel::yes);

    // If 'dependencyProj' is not empty, we're in case 2. If it is empty, we're in case 3. There may
    // be a viable exclusion $project in the pipeline, but we don't need to check for it here.
    return dependencyProj;
}
}  // namespace

Pipeline::SourceContainer::iterator DocumentSourceInternalUnpackBucket::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // Attempt to build an internalizable $project based on dependency analysis.
    if (auto projObj = buildProjectToInternalize(getContext(), itr, container);
        !projObj.isEmpty()) {
        // Give the new $project a chance to be optimized before internalizing.
        container->insert(std::next(itr),
                          DocumentSourceProject::createFromBson(
                              BSON("$project" << projObj).firstElement(), pExpCtx));
        return std::next(itr);
    }

    return std::next(itr);
}
}  // namespace mongo
