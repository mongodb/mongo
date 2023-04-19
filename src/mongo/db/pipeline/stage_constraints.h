/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/intrusive_ptr.hpp>
#include <numeric>

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"

namespace mongo {
/**
 * A struct describing various constraints about where this stage can run, where it must be in
 * the pipeline, what resources it may require, etc.
 */
struct StageConstraints {
    /**
     * A StreamType defines whether this stage is streaming (can produce output based solely on
     * the current input document) or blocking (must examine subsequent documents before
     * producing an output document).
     */
    enum class StreamType { kStreaming, kBlocking };

    /**
     * A PositionRequirement stipulates what specific position the stage must occupy within the
     * pipeline, if any.
     */
    enum class PositionRequirement {
        kNone,
        kFirst,
        kLast,
        // Stages with 'kCustom' requirement must also implement the 'validatePipelinePosition()'
        // method which is called during pipeline validation.
        kCustom
    };

    /**
     * A HostTypeRequirement defines where this stage is permitted to be executed when the
     * pipeline is run on a sharded cluster.
     */
    enum class HostTypeRequirement {
        // Indicates that the stage can run either on mongoD or mongoS.
        kNone,
        // Indicates that the stage must run on the host to which it was originally sent and
        // cannot be forwarded from mongoS to the shards.
        kLocalOnly,
        // Indicates that the stage must run on the primary shard.
        kPrimaryShard,
        // Indicates that the stage must run on any participating shard.
        kAnyShard,
        // Indicates that the stage can only run on mongoS.
        kMongoS,
        // Indicates that the stage should run on all data-bearing nodes, primary and secondary, for
        // the participating shards. This is useful for stages like $currentOp which generate
        // node-specific metadata.
        kAllShardServers,
    };

    /**
     * A DiskUseRequirement indicates whether this stage writes permanent data to disk, or
     * whether it may spill temporary data to disk if its memory usage exceeds a given
     * threshold. Note that this only indicates that the stage has the ability to spill; if
     * 'allowDiskUse' is set to false, it will be prevented from doing so.
     *
     * This enum is purposefully ordered such that a "stronger" need to write data has a higher
     * enum value.
     */
    enum class DiskUseRequirement { kNoDiskUse, kWritesTmpData, kWritesPersistentData };

    /**
     * A ChangeStreamRequirement determines whether a particular stage is itself a ChangeStream
     * stage, whether it is allowed to exist in a $changeStream pipeline, or whether it can only
     * exist in a change stream pipeline.
     */
    enum class ChangeStreamRequirement {
        kChangeStreamStage,    // This stage is an actual change stream stage.
        kAllowlist,            // This stage is permitted in a change stream pipeline.
        kDenylist,             // This stage is banned from change stream pipelines.
        kRequiresChangeStream  // This stage is only allowed in a change stream pipeline.
    };

    /**
     * A FacetRequirement indicates whether this stage may be used within a $facet pipeline.
     */
    enum class FacetRequirement { kAllowed, kNotAllowed };

    /**
     * Indicates whether or not this stage is legal when the read concern for the aggregate has
     * readConcern level "snapshot" or is running inside of a multi-document transaction.
     */
    enum class TransactionRequirement { kNotAllowed, kAllowed };

    /**
     * Indicates whether or not this stage may be run as part of a $lookup pipeline.
     */
    enum class LookupRequirement { kNotAllowed, kAllowed };

    /**
     * Indicates whether or not this stage may be run as part of a $unionWith pipeline.
     */
    enum class UnionRequirement { kNotAllowed, kAllowed };

    /**
     * Returns the StageConstraints representing the strictest constraint of each type from the
     * given pipeline. Does not compare StreamType and PositionRequirement because those values
     * don't make sense as a property of a pipeline. Also does not compare HostTypeRequirement as
     * there is no strict ordering of possible values. Those three values in the returned
     * StageConstraints will always be the same as those passed in `defaultReqs`.
     */
    template <typename DocumentSourceContainer>
    static StageConstraints getStrictestConstraints(const DocumentSourceContainer& pipeline,
                                                    const StageConstraints& defaultReqs) {
        auto newReqs = defaultReqs;
        for (const auto& stage : pipeline) {
            const auto stageConstraints = stage->constraints();
            // Skip PositionRequirement, StreamType, and HostTypeRequirement, as it doesn't make
            // sense to compare those values.
            newReqs.diskRequirement =
                std::max(newReqs.diskRequirement, stageConstraints.diskRequirement);
            newReqs.facetRequirement =
                std::max(newReqs.facetRequirement, stageConstraints.facetRequirement);
            newReqs.transactionRequirement =
                std::min(newReqs.transactionRequirement, stageConstraints.transactionRequirement);
            newReqs.lookupRequirement =
                std::min(newReqs.lookupRequirement, stageConstraints.lookupRequirement);
            newReqs.unionRequirement =
                std::min(newReqs.unionRequirement, stageConstraints.unionRequirement);
        }
        return newReqs;
    }

    StageConstraints(
        StreamType streamType,
        PositionRequirement requiredPosition,
        HostTypeRequirement hostRequirement,
        DiskUseRequirement diskRequirement,
        FacetRequirement facetRequirement,
        TransactionRequirement transactionRequirement,
        LookupRequirement lookupRequirement,
        UnionRequirement unionRequirement,
        ChangeStreamRequirement changeStreamRequirement = ChangeStreamRequirement::kDenylist)
        : requiredPosition(requiredPosition),
          hostRequirement(hostRequirement),
          diskRequirement(diskRequirement),
          changeStreamRequirement(changeStreamRequirement),
          facetRequirement(facetRequirement),
          transactionRequirement(transactionRequirement),
          lookupRequirement(lookupRequirement),
          unionRequirement(unionRequirement),
          streamType(streamType) {
        // Stages which are allowed to run in $facet must not have any position requirements.
        invariant(!(isAllowedInsideFacetStage() && requiredPosition != PositionRequirement::kNone));

        // No change stream stages are permitted to run in a $facet or $lookup pipelines.
        invariant(!(isChangeStreamStage() && isAllowedInsideFacetStage()));
        invariant(!(isChangeStreamStage() && isAllowedInLookupPipeline()));

        // Stages which write persistent data cannot be used in a $lookup pipeline.
        invariant(!(isAllowedInLookupPipeline() && writesPersistentData()));
        invariant(
            !(isAllowedInLookupPipeline() && hostRequirement == HostTypeRequirement::kMongoS));

        // Only streaming stages are permitted in $changeStream pipelines.
        invariant(!(isAllowedInChangeStream() && streamType == StreamType::kBlocking));

        // A stage which is allowlisted for $changeStream cannot have a requirement to run on a
        // shard, since it needs to be able to run on mongoS in a cluster.
        invariant(!(changeStreamRequirement == ChangeStreamRequirement::kAllowlist &&
                    (hostRequirement == HostTypeRequirement::kAnyShard ||
                     hostRequirement == HostTypeRequirement::kPrimaryShard ||
                     hostRequirement == HostTypeRequirement::kAllShardServers)));

        // A stage which is allowlisted for $changeStream cannot have a position requirement.
        invariant(!(changeStreamRequirement == ChangeStreamRequirement::kAllowlist &&
                    requiredPosition != PositionRequirement::kNone));

        // Change stream stages should not be permitted with readConcern level "snapshot" or
        // inside of a multi-document transaction.
        if (isChangeStreamStage()) {
            invariant(!isAllowedInTransaction());
        }

        // Stages which write data to user collections should not be permitted with readConcern
        // level "snapshot" or inside of a multi-document transaction.
        // TODO (SERVER-36259): relax this requirement when $out and/or $merge (which write
        // persistent data) is allowed in a transaction.
        if (diskRequirement == DiskUseRequirement::kWritesPersistentData) {
            invariant(!isAllowedInTransaction());
        }

        tassert(
            7355706,
            "Stage can only broadcast to all shard servers if it must be the first stage in the "
            "pipeline.",
            hostRequirement != HostTypeRequirement::kAllShardServers ||
                (requiredPosition == PositionRequirement::kFirst));
    }

    /**
     * Returns the literal HostTypeRequirement used to initialize the StageConstraints, or the
     * effective HostTypeRequirement (kAnyShard or kMongoS) if kLocalOnly was specified.
     */
    HostTypeRequirement resolvedHostTypeRequirement(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
        return (hostRequirement != HostTypeRequirement::kLocalOnly
                    ? hostRequirement
                    : (expCtx->inMongos ? HostTypeRequirement::kMongoS
                                        : HostTypeRequirement::kAnyShard));
    }

    /**
     * True if this stage must run on the same host to which it was originally sent.
     */
    bool mustRunLocally() const {
        return hostRequirement == HostTypeRequirement::kLocalOnly;
    }

    /**
     * True if this stage is permitted to run in a $facet pipeline.
     */
    bool isAllowedInsideFacetStage() const {
        return facetRequirement == FacetRequirement::kAllowed;
    }

    /**
     * True if this stage is permitted to run in a pipeline which starts with $changeStream.
     */
    bool isAllowedInChangeStream() const {
        return changeStreamRequirement != ChangeStreamRequirement::kDenylist;
    }

    /**
     * True if this stage is itself a $changeStream stage, and is therefore implicitly allowed
     * to run in a pipeline which begins with $changeStream.
     */
    bool isChangeStreamStage() const {
        return changeStreamRequirement == ChangeStreamRequirement::kChangeStreamStage;
    }

    /**
     * True if this stage must run in a pipeline which starts with $changeStream.
     */
    bool requiresChangeStream() const {
        return changeStreamRequirement == ChangeStreamRequirement::kRequiresChangeStream;
    }

    /**
     * Returns true if this stage is legal when the readConcern level is "snapshot" or when this
     * aggregation is being run within a multi-document transaction.
     */
    bool isAllowedInTransaction() const {
        return transactionRequirement == TransactionRequirement::kAllowed;
    }

    /**
     * Returns true if this stage may be used inside a $lookup subpipeline.
     */
    bool isAllowedInLookupPipeline() const {
        return lookupRequirement == LookupRequirement::kAllowed;
    }

    /**
     * Returns true if this stage may be used inside a $unionWith subpipeline.
     */
    bool isAllowedInUnionPipeline() const {
        return unionRequirement == UnionRequirement::kAllowed;
    }

    /**
     * Returns true if this stage writes persistent data to disk.
     */
    bool writesPersistentData() const {
        return diskRequirement == DiskUseRequirement::kWritesPersistentData;
    }

    // Indicates whether this stage needs to be at a particular position in the pipeline.
    PositionRequirement requiredPosition;

    // Indicates whether this stage can only be executed on specific components of a sharded
    // cluster.
    HostTypeRequirement hostRequirement;

    // Indicates whether this stage may write persistent data to disk, or may spill to temporary
    // files if its memory usage becomes excessive.
    DiskUseRequirement diskRequirement;

    // Indicates whether this stage is itself a $changeStream stage, or if not whether it may
    // exist in a pipeline which begins with $changeStream.
    ChangeStreamRequirement changeStreamRequirement;

    // Indicates whether this stage may run inside a $facet stage.
    FacetRequirement facetRequirement;

    // Indicates whether this stage is legal when the readConcern level is "snapshot" or the
    // aggregate is running inside of a multi-document transaction.
    TransactionRequirement transactionRequirement;

    // Indicates whether this stage is allowed in a $lookup subpipeline.
    LookupRequirement lookupRequirement;

    // Indicates whether this stage is allowed in a $unionWith subpipeline.
    UnionRequirement unionRequirement;

    // Indicates whether this is a streaming or blocking stage.
    StreamType streamType;

    // True if this stage does not generate results itself, and instead pulls inputs from an
    // input DocumentSource (via 'pSource').
    bool requiresInputDocSource = true;

    // True if this stage operates on a global or database level, like $currentOp.
    bool isIndependentOfAnyCollection = false;

    // True if this stage can ever be safely swapped with a subsequent $match stage, provided
    // that the match does not depend on the paths returned by getModifiedPaths().
    //
    // Stages that want to participate in match swapping should set this to true. Such a stage
    // must also override getModifiedPaths() to provide information about which particular
    // $match predicates be swapped before itself.
    bool canSwapWithMatch = false;

    // True if this stage can be safely swapped with a stage which alters the number of documents in
    // the stream.
    //
    // For example, a $project can be safely swapped with a $skip, $limit, or $sample. But there are
    // some cases when we cannot perform such swap:
    // - $skip, $limit and $sample stages cannot be moved before any stage which will change the
    //   number of documents
    // - $skip, $limit and $sample stages cannot be swapped with any stage which will change the
    //   order of documents
    // - $sample cannot be swapped with stages which will change behavior based on the order of
    //   documents because our implementation of $sample shuffles the order
    bool canSwapWithSkippingOrLimitingStage = false;

    // If true, then any stage of kind 'DocumentSourceSingleDocumentTransformation' can be swapped
    // ahead of this stage.
    bool canSwapWithSingleDocTransform = false;

    // Indicates that a stage is allowed within a pipeline-stlye update.
    bool isAllowedWithinUpdatePipeline = false;

    // If true, then this stage may only appear in the pipeline once, though it can appear at an
    // arbitrary position. It is not necessary to consider this for stages which have a strict
    // PositionRequirement, since the presence of a second stage will violate that constraint.
    bool canAppearOnlyOnceInPipeline = false;

    // Indicates that a stage does not modify anything to do with a sort and can be done before a
    // following merge sort.
    bool preservesOrderAndMetadata = false;

    bool operator==(const StageConstraints& other) const {
        return requiredPosition == other.requiredPosition &&
            hostRequirement == other.hostRequirement && diskRequirement == other.diskRequirement &&
            changeStreamRequirement == other.changeStreamRequirement &&
            facetRequirement == other.facetRequirement &&
            transactionRequirement == other.transactionRequirement &&
            lookupRequirement == other.lookupRequirement && streamType == other.streamType &&
            requiresInputDocSource == other.requiresInputDocSource &&
            isIndependentOfAnyCollection == other.isIndependentOfAnyCollection &&
            canSwapWithMatch == other.canSwapWithMatch &&
            canSwapWithSkippingOrLimitingStage == other.canSwapWithSkippingOrLimitingStage &&
            canSwapWithSingleDocTransform == other.canSwapWithSingleDocTransform &&
            canAppearOnlyOnceInPipeline == other.canAppearOnlyOnceInPipeline &&
            isAllowedWithinUpdatePipeline == other.isAllowedWithinUpdatePipeline &&
            unionRequirement == other.unionRequirement &&
            preservesOrderAndMetadata == other.preservesOrderAndMetadata;
    }
};
}  // namespace mongo
