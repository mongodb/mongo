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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/timeseries/bucket_spec.h"
#include "mongo/db/timeseries/mixed_schema_buckets_state.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

struct InternalUnpackBucketSharedState {
    // It's beneficial to do as much filtering at the bucket level as possible to avoid unpacking
    // buckets that wouldn't contribute to the results anyway. There is a generic mechanism that
    // allows to swap $match stages with this one (see 'getModifiedPaths()'). It lets us split out
    // and push down a filter on the metaField "as is". The remaining filters might cause creation
    // of additional bucket-level filters (see 'createPredicatesOnBucketLevelField()') that are
    // inserted before this stage while the original filter is incorporated into this stage as
    // '_eventFilter' (to be applied to each unpacked document) and/or '_wholeBucketFilter' for the
    // cases when _all_ events in a bucket would match so that the filter is evaluated only once
    // rather than on all events from the bucket (currently, we only do this for the 'timeField').
    std::unique_ptr<MatchExpression> _eventFilter;
    std::unique_ptr<MatchExpression> _wholeBucketFilter;
    timeseries::BucketUnpacker _bucketUnpacker;
};

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
    static constexpr StringData kFixedBuckets = "fixedBuckets"_sd;
    static constexpr StringData kSbeCompatible = "sbeCompatible"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBsonInternal(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);
    static boost::intrusive_ptr<DocumentSource> createFromBsonExternal(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSourceInternalUnpackBucket(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       timeseries::BucketUnpacker bucketUnpacker,
                                       int bucketMaxSpanSeconds,
                                       bool assumeNoMixedSchemaData = false,
                                       bool fixedBuckets = false,
                                       boost::optional<bool> sbeCompatible = boost::none);

    DocumentSourceInternalUnpackBucket(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       timeseries::BucketUnpacker bucketUnpacker,
                                       int bucketMaxSpanSeconds,
                                       const boost::optional<BSONObj>& eventFilterBson,
                                       const boost::optional<BSONObj>& wholeBucketFilterBson,
                                       bool assumeNoMixedSchemaData = false,
                                       bool fixedBuckets = false,
                                       boost::optional<bool> sbeCompatible = boost::none);

    const char* getSourceName() const override {
        return kStageNameInternal.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    void serializeToArray(std::vector<Value>& array,
                          const SerializationOptions& opts = SerializationOptions{}) const final;

    /**
     * Use 'serializeToArray' above.
     */
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final {
        MONGO_UNREACHABLE_TASSERT(7484305);
    }

    bool includeMetaField() const {
        return _sharedState->_bucketUnpacker.includeMetaField();
    }

    bool includeTimeField() const {
        return _sharedState->_bucketUnpacker.includeTimeField();
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed,
                                     ChangeStreamRequirement::kDenylist};
        constraints.canSwapWithMatch = true;
        // The user cannot specify multiple $unpackBucket stages in the pipeline.
        constraints.canAppearOnlyOnceInPipeline = true;
        // This stage only reads raw timeseries bucket documents.
        constraints.consumesLogicalCollectionData = false;
        return constraints;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        if (_sampleSize) {
            deps->needRandomGenerator = true;
        }
        deps->needWholeDocument = true;
        return DepsTracker::State::EXHAUSTIVE_ALL;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    int getBucketMaxSpanSeconds() const {
        return _bucketMaxSpanSeconds;
    }

    std::string getMinTimeField() const {
        return _sharedState->_bucketUnpacker.getMinField(
            _sharedState->_bucketUnpacker.getTimeField());
    }

    std::string getMaxTimeField() const {
        return _sharedState->_bucketUnpacker.getMaxField(
            _sharedState->_bucketUnpacker.getTimeField());
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    };

    const timeseries::BucketUnpacker& bucketUnpacker() const {
        return _sharedState->_bucketUnpacker;
    }

    /**
     * See ../query/timeseries/README.md for a description of all the rewrites implemented in this
     * function. The README.md should be maintained in sync with this function. Please update the
     * README accordingly.
     */
    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

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
        DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) const;

    /**
     * Convenience wrapper around BucketSpec::createPredicatesOnBucketLevelField().
     */
    timeseries::BucketSpec::BucketPredicate createPredicatesOnBucketLevelField(
        const MatchExpression* matchExpr) const;

    /**
     * Convenience wrapper around BucketSpec::generateBucketLevelIdPredicates().
     */
    bool generateBucketLevelIdPredicates(MatchExpression* matchExpr) const;

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
        _sharedState->_bucketUnpacker.setIncludeMinTimeAsMetadata();
    }

    void setIncludeMaxTimeAsMetadata() {
        _sharedState->_bucketUnpacker.setIncludeMaxTimeAsMetadata();
    }

    boost::optional<long long> sampleSize() const {
        return _sampleSize;
    }

    /**
     * If the stage after $_internalUnpackBucket is $project, $addFields, or $set, try to extract
     * from it computed meta projections and push them pass the current stage. Returns the iterator
     * that needs to be optimized next.
     */
    boost::optional<DocumentSourceContainer::iterator> pushDownComputedMetaProjection(
        DocumentSourceContainer::iterator itr, DocumentSourceContainer* container);

    /**
     * If 'src' represents an exclusion $project, attempts to extract the parts of 'src' that are
     * only on the metaField. Returns a BSONObj representing the extracted project and a bool
     * indicating whether all of 'src' was extracted. In the extracted $project, the metaField is
     * renamed from the user defined name to 'kBucketMetaFieldName'.
     */
    std::pair<BSONObj, bool> extractProjectForPushDown(DocumentSource* src) const;

    /**
     * Helper method which checks if we can avoid unpacking if we have a group stage with
     * min/max/count aggregates. If the rewrite is possible, 'container' is modified, bool in the
     * return pair is set to 'true' and the iterator is set to point to the new group.
     */
    std::pair<bool, DocumentSourceContainer::iterator> rewriteGroupStage(
        DocumentSourceContainer::iterator itr, DocumentSourceContainer* container);

    /**
     * Helper method which checks if we can replace DocumentSourceGroup with
     * DocumentSourceStreamingGroup. Returns true if the optimization is performed.
     */
    bool enableStreamingGroupIfPossible(DocumentSourceContainer::iterator itr,
                                        DocumentSourceContainer* container);

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
    bool optimizeLastpoint(DocumentSourceContainer::iterator itr,
                           DocumentSourceContainer* container);

    GetModPathsReturn getModifiedPaths() const final;

    DepsTracker getRestPipelineDependencies(DocumentSourceContainer::iterator itr,
                                            DocumentSourceContainer* container,
                                            bool includeEventFilter) const;

    const MatchExpression* eventFilter() const {
        return _sharedState->_eventFilter.get();
    }

    const MatchExpression* wholeBucketFilter() const {
        return _sharedState->_wholeBucketFilter.get();
    }

    bool isSbeCompatible();

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalUnpackBucketToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    bool haveComputedMetaField() const;

    // Parses given 'eventFilterBson' to set '_eventFilter' and determines its dependencies
    // and SBE compatibility.
    void setEventFilter(BSONObj eventFilterBson, bool shouldOptimize);

    /**
     * Applies optimizeAt() to all stages in the given pipeline after the stage that 'itr' points
     * to, which is the bucket unpack stage.
     *
     * Due to the manipulation of 'itr' through the optimizations, it may be possible that
     * preceeding stages will be optimized. However, optimization of the bucket unpack stage will be
     * skipped.
     */
    DocumentSourceContainer::iterator optimizeAtRestOfPipeline(
        DocumentSourceContainer::iterator itr, DocumentSourceContainer* container);

    /**
     * The top-k sort optimization absorbs a $sort stage that is enough to produce a top-k sorted
     * input for a group key if the $sort is followed by $group with $first and/or $last.
     *
     * For example, the following pipeline can be rewritten into a $group with $top/$bottom:
     * [
     *   {$_internalUnpackBucket: {...}},
     *   {$sort: {b: 1}},
     *   {$group: {_id: "$a", f: {$first: "$b"}, l: {$last: "$b"}}
     * ]
     *
     * The rewritten pipeline would be:
     * [
     *   {$_internalUnpackBucket: {...}},
     *   {
     *     $group: {
     *       _id: "$a",
     *       f: {$top: {sortBy: {b: 1}, output: "$b"}},
     *       l: {$bottom: {sortBy: {b: 1}, output: "$b"}}
     *     }
     *   }
     * ]
     */
    bool tryToAbsorbTopKSortIntoGroup(DocumentSourceContainer::iterator itr,
                                      DocumentSourceContainer* container);

    // If buckets contained a mixed type schema along some path, we have to push down special
    // predicates in order to ensure correctness.
    bool _assumeNoMixedSchemaData = false;

    // This is true if 'bucketRoundingSeconds' and 'bucketMaxSpanSeconds' are set, equal, and
    // unchanged. Then we can push down certain $match and $group queries.
    bool _fixedBuckets = false;

    // If any bucket contains dates outside the range of 1970-2038, we are unable to rely on
    // the _id index, as _id is truncated to 32 bits. Note that this is a per-shard attribute (some
    // shards of a collection may have extended range data while others do not), so when mongos
    // sends a pipeline containing this stage to mongod, it will omit this value, as it may be
    // different from the DB primary shard.
    bool _usesExtendedRange = false;

    int _bucketMaxSpanSeconds;

    int _bucketMaxCount = 0;
    boost::optional<long long> _sampleSize;

    std::shared_ptr<InternalUnpackBucketSharedState> _sharedState;

    BSONObj _eventFilterBson;
    DepsTracker _eventFilterDeps;
    BSONObj _wholeBucketFilterBson;

    // This will be boost::none or true if should check to see if we can generate predicates on _id
    // in a match stage that has been pushed before this stage.
    boost::optional<bool> _checkIfNeedsIdPredicates = boost::none;

    // If after unpacking there are no stages referencing any fields (e.g. $count), unpack directly
    // to BSON so that data doesn't need to be materialized to Document.
    bool _unpackToBson = false;

    bool _optimizedEndOfPipeline = false;

    // These variables are only used to prevent infinite loops when we step backwards to optimize
    // $match. These values do not guarantee that optimizations have only run once. Even with these
    // values optimizations can happen twice (once on mongos and once on mongod) and all
    // optimizations must be correct when run multiple times.
    bool _triedInternalizeProjectLocally = false;
    bool _triedLastpointRewriteLocally = false;
    bool _triedLimitPushDownLocally = false;

    // The $project or $addFields stages which we have tried to apply the computed meta project push
    // down optimization to.
    std::vector<DocumentSource*> _triedComputedMetaPushDownFor;

    // Caches the SBE-compatibility status result of this stage.
    boost::optional<bool> _isSbeCompatible = boost::none;
    boost::optional<SbeCompatibility> _isEventFilterSbeCompatible = boost::none;
};
}  // namespace mongo
