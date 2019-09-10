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

#include "mongo/db/pipeline/document_source_merge_gen.h"
#include "mongo/db/pipeline/document_source_writer.h"

namespace mongo {

/**
 * A class for the $merge aggregation stage to handle all supported merge modes. Each instance of
 * this class must be initialized (via a constructor) with a 'MergeDescriptor', which defines a
 * a particular merge strategy for a pair of 'whenMatched' and 'whenNotMatched' merge  modes.
 */
class DocumentSourceMerge final : public DocumentSourceWriter<MongoProcessInterface::BatchObject> {
public:
    static constexpr StringData kStageName = "$merge"_sd;

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

    virtual ~DocumentSourceMerge() = default;

    const char* getSourceName() const final override {
        return kStageName.rawData();
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
                TransactionRequirement::kNotAllowed,
                LookupRequirement::kNotAllowed};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final override {
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
        return DocumentSourceWriter::distributedPlanLogic();
    }

    Value serialize(
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final override;

    /**
     * Creates a new $merge stage from the given arguments.
     */
    static boost::intrusive_ptr<DocumentSource> create(
        NamespaceString outputNs,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        MergeStrategyDescriptor::WhenMatched whenMatched,
        MergeStrategyDescriptor::WhenNotMatched whenNotMatched,
        boost::optional<BSONObj> letVariables,
        boost::optional<std::vector<BSONObj>> pipeline,
        std::set<FieldPath> mergeOnFields,
        boost::optional<ChunkVersion> targetCollectionVersion);

    /**
     * Parses a $merge stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    auto getPipeline() const {
        return _pipeline;
    }

private:
    /**
     * Builds a new $merge stage which will merge all documents into 'outputNs'. If
     * 'targetCollectionVersion' is provided then processing will stop with an error if the
     * collection's epoch changes during the course of execution. This is used as a mechanism to
     * prevent the shard key from changing.
     */
    DocumentSourceMerge(NamespaceString outputNs,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        const MergeStrategyDescriptor& descriptor,
                        boost::optional<BSONObj> letVariables,
                        boost::optional<std::vector<BSONObj>> pipeline,
                        std::set<FieldPath> mergeOnFields,
                        boost::optional<ChunkVersion> targetCollectionVersion);

    /**
     * Creates an UpdateModification object from the given 'doc' to be used with the batched update.
     */
    auto makeBatchUpdateModification(const Document& doc) const {
        return _pipeline ? write_ops::UpdateModification(*_pipeline)
                         : write_ops::UpdateModification(doc.toBson());
    }

    /**
     * Resolves 'let' defined variables against the 'doc' and stores the results in the returned
     * BSON.
     */
    boost::optional<BSONObj> resolveLetVariablesIfNeeded(const Document& doc) const {
        // When we resolve 'let' variables, an empty BSON object or boost::none won't make any
        // difference at the end-point (in the PipelineExecutor), as in both cases we will end up
        // with the update pipeline ExpressionContext not being populated with any variables, so we
        // are not making a distinction between these two cases here.
        if (!_letVariables || _letVariables->empty()) {
            return boost::none;
        }

        BSONObjBuilder bob;
        for (auto&& [name, expr] : *_letVariables) {
            bob << name << expr->evaluate(doc, &pExpCtx->variables);
        }
        return bob.obj();
    }

    void spill(BatchedObjects&& batch) override {
        DocumentSourceWriteBlock writeBlock(pExpCtx->opCtx);

        try {
            auto targetEpoch = _targetCollectionVersion
                ? boost::optional<OID>(_targetCollectionVersion->epoch())
                : boost::none;

            _descriptor.strategy(pExpCtx, _outputNs, _writeConcern, targetEpoch, std::move(batch));
        } catch (const ExceptionFor<ErrorCodes::ImmutableField>& ex) {
            uassertStatusOKWithContext(ex.toStatus(),
                                       "$merge failed to update the matching document, did you "
                                       "attempt to modify the _id or the shard key?");
        }
    }

    void waitWhileFailPointEnabled() override;

    std::pair<BatchObject, int> makeBatchObject(Document&& doc) const override;

    boost::optional<ChunkVersion> _targetCollectionVersion;

    // A merge descriptor contains a merge strategy function describing how to merge two
    // collections, as well as some other metadata needed to perform the merge operation. This is
    // a reference to an element in a static const map 'kMergeStrategyDescriptors', which owns the
    // descriptor.
    const MergeStrategyDescriptor& _descriptor;

    // Holds 'let' variables defined in this stage. These variables are propagated to the
    // ExpressionContext of the pipeline update for use in the inner pipeline execution. The key
    // of the map is a variable name as defined in the $merge spec 'let' argument, and the value is
    // a parsed Expression, defining how the variable value must be evaluated.
    boost::optional<stdx::unordered_map<std::string, boost::intrusive_ptr<Expression>>>
        _letVariables;

    // A custom pipeline to compute a new version of merging documents.
    boost::optional<std::vector<BSONObj>> _pipeline;

    // Holds the fields used for uniquely identifying documents. There must exist a unique index
    // with this key pattern. Default is "_id" for unsharded collections, and "_id" plus the shard
    // key for sharded collections.
    std::set<FieldPath> _mergeOnFields;

    // True if '_mergeOnFields' contains the _id. We store this as a separate boolean to avoid
    // repeated lookups into the set.
    bool _mergeOnFieldsIncludesId;
};

}  // namespace mongo
