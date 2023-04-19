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
#include <vector>

#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_match.h"

namespace mongo {
class DocumentSourceInternalUnpackBucket : public DocumentSource {
public:
    static constexpr StringData kStageNameInternal = "$_internalUnpackBucket"_sd;
    static constexpr StringData kStageNameExternal = "$_unpackBucket"_sd;
    static constexpr StringData kInclude = "include"_sd;
    static constexpr StringData kExclude = "exclude"_sd;
    static constexpr StringData kAssumeNoMixedSchemaData = "assumeNoMixedSchemaData"_sd;
    static constexpr StringData kUsesExtendedRange = "usesExtendedRange"_sd;
    static constexpr StringData kBucketMaxSpanSeconds = "bucketMaxSpanSeconds"_sd;
    static constexpr StringData kIncludeMinTimeAsMetadata = "includeMinTimeAsMetadata"_sd;
    static constexpr StringData kIncludeMaxTimeAsMetadata = "includeMaxTimeAsMetadata"_sd;
    static constexpr StringData kWholeBucketFilter = "wholeBucketFilter"_sd;
    static constexpr StringData kEventFilter = "eventFilter"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBsonInternal(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);
    static boost::intrusive_ptr<DocumentSource> createFromBsonExternal(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSourceInternalUnpackBucket(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       BucketUnpacker bucketUnpacker,
                                       int bucketMaxSpanSeconds,
                                       bool assumeNoMixedSchemaData = false);

    DocumentSourceInternalUnpackBucket(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       BucketUnpacker bucketUnpacker,
                                       int bucketMaxSpanSeconds,
                                       const boost::optional<BSONObj>& eventFilterBson,
                                       const boost::optional<BSONObj>& wholeBucketFilterBson,
                                       bool assumeNoMixedSchemaData = false);

    const char* getSourceName() const override {
        return kStageNameInternal.rawData();
    }

    void serializeToArray(std::vector<Value>& array,
                          SerializationOptions opts = SerializationOptions()) const final override;

    /**
     * Use 'serializeToArray' above.
     */
    Value serialize(SerializationOptions opts = SerializationOptions()) const final override {
        MONGO_UNREACHABLE_TASSERT(7484305);
    }

    bool includeMetaField() const {
        return _bucketUnpacker.includeMetaField();
    }

    bool includeTimeField() const {
        return _bucketUnpacker.includeTimeField();
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed,
                                     ChangeStreamRequirement::kDenylist};
        constraints.canSwapWithMatch = true;
        // The user cannot specify multiple $unpackBucket stages in the pipeline.
        constraints.canAppearOnlyOnceInPipeline = true;
        return constraints;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        if (_sampleSize) {
            deps->needRandomGenerator = true;
        }
        deps->needWholeDocument = true;
        return DepsTracker::State::EXHAUSTIVE_ALL;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    int getBucketMaxSpanSeconds() const {
        return _bucketMaxSpanSeconds;
    }

    std::string getMinTimeField() const {
        return _bucketUnpacker.getMinField(_bucketUnpacker.getTimeField());
    }

    std::string getMaxTimeField() const {
        return _bucketUnpacker.getMaxField(_bucketUnpacker.getTimeField());
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    };

    BucketUnpacker bucketUnpacker() const {
        return _bucketUnpacker.copy();
    }

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
     * Convenience wrapper around BucketSpec::createPredicatesOnBucketLevelField().
     */
    BucketSpec::BucketPredicate createPredicatesOnBucketLevelField(
        const MatchExpression* matchExpr) const;

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
     * Sets the sample size to 'n' and the maximum number of measurements in a bucket to be
     * 'bucketMaxCount'. Calling this method implicitly changes the behavior from having the stage
     * unpack every bucket in a collection to sampling buckets to generate a uniform random sample
     * of size 'n'.
     */
    void setSampleParameters(long long n, int bucketMaxCount) {
        _sampleSize = n;
        _bucketMaxCount = bucketMaxCount;
    }

    void setIncludeMinTimeAsMetadata() {
        _bucketUnpacker.setIncludeMinTimeAsMetadata();
    }

    void setIncludeMaxTimeAsMetadata() {
        _bucketUnpacker.setIncludeMaxTimeAsMetadata();
    }

    boost::optional<long long> sampleSize() const {
        return _sampleSize;
    }

    /**
     * If the stage after $_internalUnpackBucket is $project, $addFields, or $set, try to extract
     * from it computed meta projections and push them pass the current stage. Return true if the
     * next stage was removed as a result of the optimization.
     */
    bool pushDownComputedMetaProjection(Pipeline::SourceContainer::iterator itr,
                                        Pipeline::SourceContainer* container);

    /**
     * If 'src' represents an exclusion $project, attempts to extract the parts of 'src' that are
     * only on the metaField. Returns a BSONObj representing the extracted project and a bool
     * indicating whether all of 'src' was extracted. In the extracted $project, the metaField is
     * renamed from the user defined name to 'kBucketMetaFieldName'.
     */
    std::pair<BSONObj, bool> extractProjectForPushDown(DocumentSource* src) const;

    /**
     * Helper method which checks if we can avoid unpacking if we have a group stage with min/max
     * aggregates. If a rewrite is possible, 'container' is modified, and we returns result value
     * for 'doOptimizeAt'.
     */
    std::pair<bool, Pipeline::SourceContainer::iterator> rewriteGroupByMinMax(
        Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container);

    /**
     * Helper method which checks if we can replace DocumentSourceGroup with
     * DocumentSourceStreamingGroup. Returns true if the optimization is performed.
     */
    bool enableStreamingGroupIfPossible(Pipeline::SourceContainer::iterator itr,
                                        Pipeline::SourceContainer* container);

    /**
     * If the current aggregation is a lastpoint-type query (ie. with a $sort on meta and time
     * fields, and a $group with a meta _id and only $first or $last accumulators) we can rewrite
     * it to avoid unpacking all buckets.
     *
     * Ex: user aggregation of
     * [{_internalUnpackBucket: {...}},
     *  {$sort: {myMeta.a: 1, myTime: -1}},
     *  {$group: {_id: "$myMeta.a", otherFields: {$first: {$otherFields}}}}]
     *
     * will be rewritten into:
     * [{$sort: {meta.a: 1, 'control.max.myTime': -1, 'control.min.myTime': -1}},
     *  {$group: {_id: "$meta.a": 1, control: {$first: "$control"}, meta: {$first: "$meta"},
     *    data: {$first: "$data"}}},
     *  {$_internalUnpackBucket: {...}},
     *  {$sort: {myMeta.a: 1, myTime: -1}},
     *  {$group: {_id: "$myMeta.a", otherFields: {$first: {$otherFields}}}}]
     *
     * Note that the first $group includes all fields so we can avoid fetching the bucket twice.
     */
    bool optimizeLastpoint(Pipeline::SourceContainer::iterator itr,
                           Pipeline::SourceContainer* container);

    GetModPathsReturn getModifiedPaths() const final override;

    DepsTracker getRestPipelineDependencies(Pipeline::SourceContainer::iterator itr,
                                            Pipeline::SourceContainer* container,
                                            bool includeEventFilter) const;

private:
    GetNextResult doGetNext() final;

    boost::optional<Document> getNextMatchingMeasure();

    bool haveComputedMetaField() const;

    // If buckets contained a mixed type schema along some path, we have to push down special
    // predicates in order to ensure correctness.
    bool _assumeNoMixedSchemaData = false;

    // If any bucket contains dates outside the range of 1970-2038, we are unable to rely on
    // the _id index, as _id is truncates to 32 bits
    bool _usesExtendedRange = false;

    BucketUnpacker _bucketUnpacker;
    int _bucketMaxSpanSeconds;

    int _bucketMaxCount = 0;
    boost::optional<long long> _sampleSize;

    // Filters pushed from the later $match stages
    std::unique_ptr<MatchExpression> _eventFilter;
    BSONObj _eventFilterBson;
    DepsTracker _eventFilterDeps;
    std::unique_ptr<MatchExpression> _wholeBucketFilter;
    BSONObj _wholeBucketFilterBson;

    bool _unpackToBson = false;

    bool _optimizedEndOfPipeline = false;
    bool _triedInternalizeProject = false;
    bool _triedLastpointRewrite = false;
};
}  // namespace mongo
