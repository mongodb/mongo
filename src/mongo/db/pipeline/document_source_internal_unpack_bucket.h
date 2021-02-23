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

#pragma once

#include <set>

#include "mongo/db/pipeline/document_source.h"

namespace mongo {

/**
 * Carries parameters for unpacking a bucket.
 */
struct BucketSpec {
    // The user-supplied timestamp field name specified during time-series collection creation.
    std::string timeField;

    // An optional user-supplied metadata field name specified during time-series collection
    // creation. This field name is used during materialization of metadata fields of a measurement
    // after unpacking.
    boost::optional<std::string> metaField;

    // The set of field names in the data region that should be included or excluded.
    std::set<std::string> fieldSet;
};

/**
 * BucketUnpacker will unpack bucket fields for metadata and the provided fields.
 */
class BucketUnpacker {
public:
    // These are hard-coded constants in the bucket schema.
    static constexpr StringData kBucketIdFieldName = "_id"_sd;
    static constexpr StringData kBucketDataFieldName = "data"_sd;
    static constexpr StringData kBucketMetaFieldName = "meta"_sd;

    // When BucketUnpacker is created with kInclude it must produce measurements that contain the
    // set of fields. Otherwise, if the kExclude option is used, the measurements will include the
    // set difference between all fields in the bucket and the provided fields.
    enum class Behavior { kInclude, kExclude };

    BucketUnpacker(BucketSpec spec,
                   Behavior unpackerBehavior,
                   bool includeTimeField,
                   bool includeMetaField)
        : _spec(std::move(spec)),
          _unpackerBehavior(unpackerBehavior),
          _includeTimeField(includeTimeField),
          _includeMetaField(includeMetaField) {}

    Document getNext();
    bool hasNext() const {
        return _timeFieldIter && _timeFieldIter->more();
    }

    /**
     * This resets the unpacker to prepare to unpack a new bucket described by the given document.
     */
    void reset(BSONObj&& bucket);

    Behavior behavior() const {
        return _unpackerBehavior;
    }

    const BucketSpec& bucketSpec() const {
        return _spec;
    }

    const BSONObj& bucket() const {
        return _bucket;
    }

    bool includeMetaField() const {
        return _includeMetaField;
    }

    bool includeTimeField() const {
        return _includeTimeField;
    }

    void setBucketSpecAndBehavior(BucketSpec&& bucketSpec, Behavior behavior);

private:
    BucketSpec _spec;
    Behavior _unpackerBehavior;

    // Iterates the timestamp section of the bucket to drive the unpacking iteration.
    boost::optional<BSONObjIterator> _timeFieldIter;

    // A flag used to mark that the timestamp value should be materialized in measurements.
    bool _includeTimeField;

    // A flag used to mark that a bucket's metadata value should be materialized in measurements.
    bool _includeMetaField;

    // The bucket being unpacked.
    BSONObj _bucket;

    // Since the metadata value is the same across all materialized measurements we can cache the
    // metadata BSONElement in the reset phase and use it to materialize the metadata in each
    // measurement.
    BSONElement _metaValue;

    // Iterators used to unpack the columns of the above bucket that are populated during the reset
    // phase according to the provided 'Behavior' and 'BucketSpec'.
    std::vector<std::pair<std::string, BSONObjIterator>> _fieldIters;
};

class DocumentSourceInternalUnpackBucket : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalUnpackBucket"_sd;
    static constexpr StringData kInclude = "include"_sd;
    static constexpr StringData kExclude = "exclude"_sd;
    static constexpr StringData kTimeFieldName = "timeField"_sd;
    static constexpr StringData kMetaFieldName = "metaField"_sd;
    static constexpr StringData kControlMaxFieldName = "control.max."_sd;
    static constexpr StringData kControlMinFieldName = "control.min."_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSourceInternalUnpackBucket(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       BucketUnpacker bucketUnpacker);

    const char* getSourceName() const override {
        return kStageName.rawData();
    }

    bool includeMetaField() const {
        return _bucketUnpacker.includeMetaField();
    }

    bool includeTimeField() const {
        return _bucketUnpacker.includeTimeField();
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kAllowed,
                UnionRequirement::kAllowed,
                ChangeStreamRequirement::kBlacklist};
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    };

    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

    /*
     * Given a $project produced by 'extractOrBuildProjectToInternalize()', attempt to internalize
     * its top-level fields by updating the state of '_bucketUnpacker'.
     */
    void internalizeProject(const BSONObj& project, bool isInclusion);

    /**
     * Given a SourceContainer and an iterator pointing to $_internalUnpackBucket, extracts or
     * builds a $project that can be entirely internalized according to the below rules. Returns the
     * $project and a bool indicating its type (true for inclusion, false for exclusion).
     *    1. If there is an inclusion projection immediately after $_internalUnpackBucket which can
     *       be internalized, it will be removed from the pipeline and returned.
     *    2. Otherwise, if there is a finite dependency set for the rest of the pipeline, an
     *       inclusion $project representing it and containing only root-level fields will be
     *       returned. An inclusion $project will be returned here even if there is a viable
     *       exclusion $project next in the pipeline.
     *    3. Otherwise, if there is an exclusion projection immediately after $_internalUnpackBucket
     *       which can be internalized, it will be removed from the pipeline and returned.
     *    3. Otherwise, an empty BSONObj will be returned.
     */
    std::pair<BSONObj, bool> extractOrBuildProjectToInternalize(
        Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) const;

    /**
     * Takes a predicate after $_internalUnpackBucket on a bucketed field as an argument and
     * attempts to map it to a new predicate on the 'control' field. For example, the predicate
     * {a: {$gt: 5}} will generate the predicate {control.max.a: {$_internalExprGt: 5}}, which will
     * be added before the $_internalUnpackBucket stage.
     *
     * If the original predicate is on the bucket's timeField we may also create a new predicate
     * on the '_id' field to assist in index utilization. For example, the predicate
     * {time: {$lt: new Date(...)}} will generate the following predicate:
     * {$and: [
     *      {_id: {$lt: ObjectId(...)}},
     *      {control.min.time: {$_internalExprLt: new Date(...)}}
     * ]}
     *
     * If the provided predicate is ineligible for this mapping, the function will return a nullptr.
     */
    std::unique_ptr<MatchExpression> createPredicatesOnBucketLevelField(
        const MatchExpression* matchExpr) const;

private:
    GetNextResult doGetNext() final;

    BucketUnpacker _bucketUnpacker;
};
}  // namespace mongo
