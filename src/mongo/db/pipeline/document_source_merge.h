/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_merge_gen.h"
#include "mongo/db/pipeline/document_source_out.h"

namespace mongo {

/**
 * A class for the $merge aggregation stage to handle all supported merge modes. Each instance of
 * this class must be initialized (via a constructor) with a 'MergeDescriptor', which defines a
 * a particular merge strategy for a pair of 'whenMatched' and 'whenNotMatched' merge  modes.
 */
class DocumentSourceMerge final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$merge"_sd;

    /**
     * Storage for a batch of BSON Objects to be inserted/updated to the write namespace. The
     * extracted 'on' field values are also stored in a batch, used by 'MergeStrategy' as the query
     * portion of the update or insert.
     */
    struct BatchedObjects {
        void emplace(write_ops::UpdateModification&& mod, BSONObj&& key) {
            modifications.emplace_back(std::move(mod));
            uniqueKeys.emplace_back(std::move(key));
        }

        bool empty() const {
            return modifications.empty();
        }

        size_t size() const {
            return modifications.size();
        }

        void clear() {
            modifications.clear();
            uniqueKeys.clear();
        }

        // For each element in the batch we store an UpdateModification which is either the new
        // document we want to upsert or insert into the collection (i.e. a 'classic' replacement
        // update), or the pipeline to run to compute the new document.
        std::vector<write_ops::UpdateModification> modifications;

        // Store the unique keys as BSON objects instead of Documents for compatibility with the
        // batch update command. (e.g. {q: <array of uniqueKeys>, u: <array of objects>})
        std::vector<BSONObj> uniqueKeys;
    };

    // A descriptor for a merge strategy. Holds a merge strategy function and a set of actions
    // the client should be authorized to perform in order to be able to execute a merge operation
    // using this merge strategy.
    struct MergeStrategyDescriptor {
        using WhenMatched = MergeWhenMatchedModeEnum;
        using WhenNotMatched = MergeWhenNotMatchedModeEnum;
        using MergeMode = std::pair<WhenMatched, WhenNotMatched>;
        // A function encapsulating a merge strategy for the $merge stage based on the pair of
        // whenMatched/whenNotMatched modes.
        using MergeStrategy = std::function<void(const boost::intrusive_ptr<ExpressionContext>&,
                                                 const NamespaceString&,
                                                 const WriteConcernOptions&,
                                                 boost::optional<OID>,
                                                 BatchedObjects&&)>;

        MergeMode mode;
        ActionSet actions;
        MergeStrategy strategy;
    };

    /**
     * A "lite parsed" $merge stage to disallow passthrough from mongos even if the source
     * collection is unsharded. This ensures that the unique index verification happens once on
     * mongos and can be bypassed on the shards.
     */
    class LiteParsed final : public LiteParsedDocumentSourceForeignCollections {
    public:
        using LiteParsedDocumentSourceForeignCollections::
            LiteParsedDocumentSourceForeignCollections;

        static std::unique_ptr<LiteParsed> parse(const AggregationRequest& request,
                                                 const BSONElement& spec);

        bool allowedToPassthroughFromMongos() const final {
            return false;
        }
    };

    /**
     * Builds a new $merge stage which will merge all documents into 'outputNs'. If
     * 'targetCollectionVersion' is provided then processing will stop with an error if the
     * collection's epoch changes during the course of execution. This is used as a mechanism to
     * prevent the shard key from changing.
     */
    DocumentSourceMerge(NamespaceString outputNs,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        const MergeStrategyDescriptor& descriptor,
                        boost::optional<std::vector<BSONObj>>&& pipeline,
                        std::set<FieldPath> mergeOnFields,
                        boost::optional<ChunkVersion> targetCollectionVersion,
                        bool serializeAsOutStage);

    virtual ~DocumentSourceMerge() = default;

    const char* getSourceName() const final override {
        return kStageName.rawData();
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final override {
        deps->needWholeDocument = true;
        return DepsTracker::State::EXHAUSTIVE_ALL;
    }

    GetModPathsReturn getModifiedPaths() const final override {
        // For purposes of tracking which fields come from where, this stage does not modify any
        // fields.
        return {GetModPathsReturn::Type::kFiniteSet, std::set<std::string>{}, {}};
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final override {
        // A $merge to an unsharded collection should merge on the primary shard to perform local
        // writes. A $merge to a sharded collection has no requirement, since each shard can perform
        // its own portion of the write. We use 'kAnyShard' to direct it to execute on one of the
        // shards in case some of the writes happen to end up being local.
        //
        // Note that this decision is inherently racy and subject to become stale. This is okay
        // because either choice will work correctly, we are simply applying a heuristic
        // optimization.
        return {StreamType::kStreaming,
                PositionRequirement::kLast,
                pExpCtx->mongoProcessInterface->isSharded(pExpCtx->opCtx, _outputNs)
                    ? HostTypeRequirement::kAnyShard
                    : HostTypeRequirement::kPrimaryShard,
                DiskUseRequirement::kWritesPersistentData,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kNotAllowed};
    }

    boost::optional<MergingLogic> mergingLogic() final override {
        // It should always be faster to avoid splitting the pipeline if the output collection is
        // sharded. If we avoid splitting the pipeline then each shard can perform the writes to the
        // target collection in parallel.
        //
        // Note that this decision is inherently racy and subject to become stale. This is okay
        // because either choice will work correctly, we are simply applying a heuristic
        // optimization.
        if (pExpCtx->mongoProcessInterface->isSharded(pExpCtx->opCtx, _outputNs)) {
            return boost::none;
        }
        // {shardsStage, mergingStage, sortPattern}
        return MergingLogic{nullptr, this, boost::none};
    }

    bool canRunInParallelBeforeOut(
        const std::set<std::string>& nameOfShardKeyFieldsUponEntryToStage) const final override {
        // If someone is asking the question, this must be the $merge stage in question, so yes!
        return true;
    }

    GetNextResult getNext() final override;

    Value serialize(
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final override;

    const NamespaceString& getOutputNs() const {
        return _outputNs;
    }

    /**
     * Creates a new $merge stage from the given arguments.
     */
    static boost::intrusive_ptr<DocumentSource> create(
        NamespaceString outputNs,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        MergeStrategyDescriptor::WhenMatched whenMatched,
        MergeStrategyDescriptor::WhenNotMatched whenNotMatched,
        boost::optional<std::vector<BSONObj>>&& pipeline,
        std::set<FieldPath> mergeOnFields,
        boost::optional<ChunkVersion> targetCollectionVersion,
        bool serializeAsOutStage);

    /**
     * Parses a $merge stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    /**
     * Writes the documents in 'batch' to the output namespace.
     */
    void spill(BatchedObjects&& batch) {
        OutStageWriteBlock writeBlock(pExpCtx->opCtx);

        try {
            _descriptor.strategy(
                pExpCtx, _outputNs, _writeConcern, _targetEpoch(), std::move(batch));
        } catch (const ExceptionFor<ErrorCodes::ImmutableField>& ex) {
            uassertStatusOKWithContext(ex.toStatus(),
                                       "$merge failed to update the matching document, did you "
                                       "attempt to modify the _id or the shard key?");
        }
    };

    boost::optional<OID> _targetEpoch() {
        return _targetCollectionVersion ? boost::optional<OID>(_targetCollectionVersion->epoch())
                                        : boost::none;
    }

    // Stash the writeConcern of the original command as the operation context may change by the
    // time we start to spill $merge writes. This is because certain aggregations (e.g. $exchange)
    // establish cursors with batchSize 0 then run subsequent getMore's which use a new operation
    // context. The getMore's will not have an attached writeConcern however we still want to
    // respect the writeConcern of the original command.
    WriteConcernOptions _writeConcern;

    const NamespaceString _outputNs;
    boost::optional<ChunkVersion> _targetCollectionVersion;

    bool _initialized = false;
    bool _done = false;

    // A merge descriptor contains a merge strategy function describing how to merge two
    // collections, as well as some other metadata needed to perform the merge operation. This is
    // a reference to an element in a static const map 'kMergeStrategyDescriptors', which owns the
    // descriptor.
    const MergeStrategyDescriptor& _descriptor;

    // A custom pipeline to compute a new version of merging documents.
    boost::optional<std::vector<BSONObj>> _pipeline;

    // Holds the fields used for uniquely identifying documents. There must exist a unique index
    // with this key pattern. Default is "_id" for unsharded collections, and "_id" plus the shard
    // key for sharded collections.
    std::set<FieldPath> _mergeOnFields;

    // True if '_mergeOnFields' contains the _id. We store this as a separate boolean to avoid
    // repeated lookups into the set.
    bool _mergeOnFieldsIncludesId;

    // If true, display this stage in the explain output as an $out stage rather that $merge. This
    // is used when the $merge stage was used an alias for $out's 'insertDocuments' and
    // 'replaceDocuments' modes.
    bool _serializeAsOutStage;
};

}  // namespace mongo
