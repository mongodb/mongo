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
#include "mongo/db/pipeline/document_source_match.h"

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

    /**
     * Given the size of a BSONObj timestamp column, formatted as it would be in a time-series
     * system.buckets.X collection, returns the number of measurements in the bucket in O(1) time.
     */
    static int computeMeasurementCount(int targetTimestampObjSize);

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

    /**
     * This method will continue to materialize Documents until the bucket is exhausted. A
     * precondition of this method is that 'hasNext()' must be true.
     */
    Document getNext();

    /**
     * This method will extract the j-th measurement from the bucket. A precondition of this method
     * is that j >= 0 && j <= the number of measurements within the underlying bucket.
     */
    Document extractSingleMeasurement(int j);

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

    int32_t numberOfMeasurements() const {
        return _numberOfMeasurements;
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

    // The number of measurements in the bucket.
    int32_t _numberOfMeasurements = 0;
};

class DocumentSourceInternalUnpackBucket : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalUnpackBucket"_sd;
    static constexpr StringData kInclude = "include"_sd;
    static constexpr StringData kExclude = "exclude"_sd;
    static constexpr StringData kTimeFieldName = "timeField"_sd;
    static constexpr StringData kMetaFieldName = "metaField"_sd;
    static constexpr StringData kControlMaxFieldNamePrefix = "control.max."_sd;
    static constexpr StringData kControlMinFieldNamePrefix = "control.min."_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSourceInternalUnpackBucket(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       BucketUnpacker bucketUnpacker);

    const char* getSourceName() const override {
        return kStageName.rawData();
    }

    void serializeToArray(
        std::vector<Value>& array,
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    /**
     * Use 'serializeToArray' above.
     */
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        MONGO_UNREACHABLE;
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

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        if (_sampleSize) {
            deps->needRandomGenerator = true;
        }
        deps->needWholeDocument = true;
        return DepsTracker::State::EXHAUSTIVE_ALL;
    }

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
     * Attempts to split 'match' into two stages, where the first is dependent only on the metaField
     * and the second is the remainder, so that applying them in sequence is equivalent to applying
     * 'match' once. Will return two intrusive_ptrs to new $match stages. Either pointer may be
     * null. If the first is non-null, it will have the metaField renamed from the user defined name
     * to 'kBucketMetaFieldName'.
     */
    std::pair<boost::intrusive_ptr<DocumentSourceMatch>, boost::intrusive_ptr<DocumentSourceMatch>>
    splitMatchOnMetaAndRename(boost::intrusive_ptr<DocumentSourceMatch> match);

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

    /**
     * Sets the sample size to 'n' and the maximum number of measurements in a bucket to be
     * 'bucketMaxCount'. Calling this method implicitly changes the behavior from having the stage
     * unpack every bucket in a collection to sampling buckets to generate a uniform random sample
     * of size 'n'.
     */
    void setSampleParameters(long long n, int bucketMaxCount) {
        _sampleSize = n;
        _bucketMaxCount = bucketMaxCount;
    }

    boost::optional<long long> sampleSize() const {
        return _sampleSize;
    }

private:
    /**
     * Carries the bucket _id and index for the measurement that was sampled by
     * 'sampleRandomBucketOptimized'.
     */
    struct SampledMeasurementKey {
        SampledMeasurementKey(OID bucketId, int64_t measurementIndex)
            : bucketId(bucketId), measurementIndex(measurementIndex) {}

        bool operator==(const SampledMeasurementKey& key) const {
            return this->bucketId == key.bucketId && this->measurementIndex == key.measurementIndex;
        }

        OID bucketId;
        int32_t measurementIndex;
    };

    /**
     * Computes a hash of 'SampledMeasurementKey' so measurements that have already been seen can
     * be kept track of for de-duplication after sampling.
     */
    struct SampledMeasurementKeyHasher {
        size_t operator()(const SampledMeasurementKey& s) const {
            return absl::Hash<uint64_t>{}(s.bucketId.view().read<uint64_t>()) ^
                absl::Hash<uint32_t>{}(s.bucketId.view().read<uint32_t>(8)) ^
                absl::Hash<int32_t>{}(s.measurementIndex);
        }
    };

    // Tracks which measurements have been seen so far. This is only used when sampling is enabled
    // for the purpose of de-duplicating measurements.
    using SeenSet = stdx::unordered_set<SampledMeasurementKey, SampledMeasurementKeyHasher>;

    GetNextResult doGetNext() final;

    /**
     * Keeps trying to sample a unique measurement by using the optimized ARHASH algorithm up to a
     * hardcoded maximum number of attempts. If a unique measurement isn't found before the maximum
     * number of tries is exhausted this method will throw.
     */
    GetNextResult sampleUniqueMeasurementFromBuckets();

    BucketUnpacker _bucketUnpacker;

    long long _nSampledSoFar = 0;
    int _bucketMaxCount = 0;
    boost::optional<long long> _sampleSize;

    SeenSet _seenSet;
};
}  // namespace mongo
