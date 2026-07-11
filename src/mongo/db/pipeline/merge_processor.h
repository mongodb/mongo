// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_merge_modes_gen.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/modules.h"

#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

struct [[MONGO_MOD_PRIVATE]] MergeStatistics {
    size_t totalDocsProcessed = 0;
    size_t docsMatched = 0;
    const size_t minInsertAttempts =
        static_cast<size_t>(internalQueryMergeMinInsertAttempts.loadRelaxed());
    const double maxInsertErrorRate = internalQueryMergeMaxInsertErrorRate.loadRelaxed();
};

// A descriptor for a merge strategy. Holds a merge strategy function and a set of actions the
// client should be authorized to perform in order to be able to execute a merge operation using
// this merge strategy. Additionally holds a 'BatchedCommandGenerator' that will initialize a
// BatchedWriteRequest for executing the batch write. If a 'BatchTransform' function is
// provided, it will be called when constructing a batch object to transform updates.
struct MergeStrategyDescriptor {
    using WhenMatched = MergeWhenMatchedModeEnum;
    using WhenNotMatched = MergeWhenNotMatchedModeEnum;
    using MergeMode = std::pair<WhenMatched, WhenNotMatched>;
    using BatchTransform = std::function<void(MongoProcessInterface::BatchObject&)>;
    using UpsertType = MongoProcessInterface::UpsertType;
    // A function encapsulating a merge strategy for the $merge stage based on the pair of
    // whenMatched/whenNotMatched modes.
    using MergeStrategy = std::function<void(const boost::intrusive_ptr<ExpressionContext>&,
                                             const NamespaceString&,
                                             const std::set<FieldPath>&,
                                             const WriteConcernOptions&,
                                             boost::optional<OID>,
                                             MongoProcessInterface::BatchedObjects&&,
                                             BatchedCommandRequest&&,
                                             UpsertType upsert,
                                             MergeStatistics*)>;

    // A function object that will be invoked to generate a BatchedCommandRequest.
    using BatchedCommandGenerator = std::function<BatchedCommandRequest(
        const boost::intrusive_ptr<ExpressionContext>&, const NamespaceString&)>;

    static constexpr auto kReplaceInsertMode =
        MergeMode{WhenMatched::kReplace, WhenNotMatched::kInsert};
    static constexpr auto kReplaceFailMode =
        MergeMode{WhenMatched::kReplace, WhenNotMatched::kFail};
    static constexpr auto kReplaceDiscardMode =
        MergeMode{WhenMatched::kReplace, WhenNotMatched::kDiscard};
    static constexpr auto kMergeInsertMode =
        MergeMode{WhenMatched::kMerge, WhenNotMatched::kInsert};
    static constexpr auto kMergeFailMode = MergeMode{WhenMatched::kMerge, WhenNotMatched::kFail};
    static constexpr auto kMergeDiscardMode =
        MergeMode{WhenMatched::kMerge, WhenNotMatched::kDiscard};
    static constexpr auto kKeepExistingInsertMode =
        MergeMode{WhenMatched::kKeepExisting, WhenNotMatched::kInsert};
    static constexpr auto kFailInsertMode = MergeMode{WhenMatched::kFail, WhenNotMatched::kInsert};
    static constexpr auto kPipelineInsertMode =
        MergeMode{WhenMatched::kPipeline, WhenNotMatched::kInsert};
    static constexpr auto kPipelineFailMode =
        MergeMode{WhenMatched::kPipeline, WhenNotMatched::kFail};
    static constexpr auto kPipelineDiscardMode =
        MergeMode{WhenMatched::kPipeline, WhenNotMatched::kDiscard};

    MergeMode mode;
    ActionSet actions;
    MergeStrategy strategy;
    BatchTransform transform;
    UpsertType upsertType;
    BatchedCommandGenerator batchedCommandGenerator;
    bool isInsertWithUpdateBackupStrategy = false;
};

/**
 * This class is used by the aggregation framework and streams enterprise module
 * to perform the document processing needed for $merge.
 */
class MergeProcessor {
public:
    /**
     * The strictly-typed flag to allow or disable merge strategires that try insert first and
     * fallback to update if errors happen.
     */
    enum AllowInsertWithUpdateBackupStrategies : bool {};

    /**
     * If 'collectionPlacementVersion' is provided then processing will stop with an error if the
     * collection's epoch changes during the course of execution. This is used as a mechanism to
     * prevent the shard key from changing.
     */
    MergeProcessor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                   MergeStrategyDescriptor::WhenMatched whenMatched,
                   MergeStrategyDescriptor::WhenNotMatched whenNotMatched,
                   boost::optional<BSONObj> letVariables,
                   boost::optional<std::vector<BSONObj>> pipeline,
                   boost::optional<ChunkVersion> collectionPlacementVersion,
                   bool allowMergeOnNullishValues,
                   AllowInsertWithUpdateBackupStrategies allowInsertWithUpdateBackupStrategies);

    const MergeStrategyDescriptor& getMergeStrategyDescriptor() const {
        return _descriptor;
    }

    const auto& getLetVariables() const {
        return _letVariables;
    }

    const auto& getPipeline() const {
        return _pipeline;
    }

    const auto& getCollectionPlacementVersion() const {
        return _collectionPlacementVersion;
    }

    bool getAllowMergeOnNullishValues() const {
        return _allowMergeOnNullishValues;
    }

    MongoProcessInterface::BatchObject makeBatchObject(Document doc,
                                                       const std::set<FieldPath>& mergeOnFieldPaths,
                                                       bool mergeOnFieldPathsIncludeId) const;

    void flush(const NamespaceString& outputNs,
               const std::set<FieldPath>& mergeOnFieldPaths,
               BatchedCommandRequest bcr,
               MongoProcessInterface::BatchedObjects batch);

    bool shouldFlush(size_t currentBatchSize);

private:
    /**
     * Creates an UpdateModification object from the given 'doc' to be used with the batched update.
     */
    auto makeBatchUpdateModification(const Document& doc) const {
        return _pipeline ? write_ops::UpdateModification(*_pipeline)
                         : write_ops::UpdateModification(
                               doc.toBson(), write_ops::UpdateModification::ReplacementTag{});
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
        if (_letVariables.empty()) {
            return boost::none;
        }

        BSONObjBuilder bob;
        for (const auto& letVar : _letVariables) {
            bob << letVar.name << letVar.expression->evaluate(doc, &_expCtx->variables);
        }
        return bob.obj();
    }

    /**
     * Extracts the fields of $merge 'on' from 'doc' and returns the key as a BSONObj. Throws if any
     * field of the 'on' extracted from 'doc' is an array.
     */
    BSONObj _extractMergeOnFieldsFromDoc(const Document& doc,
                                         const std::set<FieldPath>& mergeOnFields) const;


    boost::intrusive_ptr<ExpressionContext> _expCtx;

    WriteConcernOptions _writeConcern;

    // A merge descriptor contains a merge strategy function describing how to merge two
    // collections, as well as some other metadata needed to perform the merge operation. This is
    // a reference to an element in a static const map 'mergeStrategyDescriptors', which owns the
    // descriptor.
    const MergeStrategyDescriptor& _descriptor;

    // Holds 'let' variables defined in $merge stage. These variables are propagated to the
    // ExpressionContext of the pipeline update for use in the inner pipeline execution.
    //
    // 'let' variables are stored in the vector in order to ensure the stability in the query shape
    // serialization.
    std::vector<LetVariable> _letVariables;

    // A custom pipeline to compute a new version of merging documents.
    boost::optional<std::vector<BSONObj>> _pipeline;

    boost::optional<ChunkVersion> _collectionPlacementVersion;
    bool _allowMergeOnNullishValues;

    MergeStatistics _mergeStats;
};

const std::map<const MergeStrategyDescriptor::MergeMode, const MergeStrategyDescriptor>&
getMergeStrategyDescriptors(
    MergeProcessor::AllowInsertWithUpdateBackupStrategies allowInsertWithUpdateBackupStrategies);

}  // namespace mongo
