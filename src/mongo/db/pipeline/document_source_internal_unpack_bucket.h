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

private:
    const BucketSpec _spec;
    const Behavior _unpackerBehavior;

    // Iterates the timestamp section of the bucket to drive the unpacking iteration.
    boost::optional<BSONObjIterator> _timeFieldIter;

    // A flag used to mark that the timestamp value should be materialized in measurements.
    const bool _includeTimeField;

    // A flag used to mark that a bucket's metadata element should be materialized in measurements.
    const bool _includeMetaField;

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

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSourceInternalUnpackBucket(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       BucketUnpacker bucketUnpacker);

    const char* getSourceName() const override {
        return kStageName.rawData();
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

private:
    GetNextResult doGetNext() final;

    BucketUnpacker _bucketUnpacker;
};
}  // namespace mongo
