// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata_gen.h"
#include "mongo/db/pipeline/lite_parsed_document_source_nested_pipelines.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"

#include <functional>
#include <memory>
#include <variant>
#include <vector>

namespace mongo {

class ExpressionContext;

using DocResultsShardedPlanProvider = std::function<const ShardedPlanSpec&(ExpressionContext*)>;

// The sharded distributed-plan info comes from exactly one of two mutually exclusive sources:
//   * DocResultsShardedPlanProvider — live callback from the extension AST node, attached during
//     in-process construction.
//   * ShardedPlanSpec — values the router cached into the IDL spec so a shard re-parsing this
//     stage inside a subpipeline can reconstruct DPL without the callback.
using ShardedPlanSource = std::variant<DocResultsShardedPlanProvider, ShardedPlanSpec>;

/**
 * Stage params produced by InternalDocumentResultsAndMetadataLiteParsed and consumed by
 * DocumentSourceInternalDocumentResultsAndMetadata::createFromStageParams.
 */
class InternalDocumentResultsAndMetadataStageParams : public StageParams {
public:
    InternalDocumentResultsAndMetadataStageParams(boost::optional<MetadataBindSpec> metadata,
                                                  bool returnCursor,
                                                  OwnedLiteParsedPipeline sourcePipeline,
                                                  boost::optional<ShardedPlanSource> shardedPlan)
        : _metadata(std::move(metadata)),
          _returnCursor(returnCursor),
          _sourcePipeline(std::move(sourcePipeline)),
          _shardedPlan(std::move(shardedPlan)) {}

    static const Id& id;
    Id getId() const final {
        return id;
    }

    const boost::optional<MetadataBindSpec>& getMetadata() const {
        return _metadata;
    }

    bool getReturnCursor() const {
        return _returnCursor;
    }

    const OwnedLiteParsedPipeline& getSourcePipeline() const {
        return _sourcePipeline;
    }

    const boost::optional<ShardedPlanSource>& getShardedPlan() const {
        return _shardedPlan;
    }

private:
    boost::optional<MetadataBindSpec> _metadata;
    bool _returnCursor;
    OwnedLiteParsedPipeline _sourcePipeline;
    // Carries the extension's DPL callback through the LiteParsed -> StageParams handoff so the
    // resulting DocumentSource can wire up its sharded plan logic. Null on non-extension paths.
    boost::optional<ShardedPlanSource> _shardedPlan;
};

/**
 * Lite-parsed representation of the $_internalDocumentResultsAndMetadata stage.
 */
class InternalDocumentResultsAndMetadataLiteParsed final
    : public LiteParsedDocumentSourceNestedPipelines<InternalDocumentResultsAndMetadataLiteParsed> {
public:
    static std::unique_ptr<InternalDocumentResultsAndMetadataLiteParsed> parse(
        const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options);

    InternalDocumentResultsAndMetadataLiteParsed(const BSONElement& spec,
                                                 boost::optional<MetadataBindSpec> metadata,
                                                 bool returnCursor,
                                                 OwnedLiteParsedPipeline sourcePipeline)
        : LiteParsedDocumentSourceNestedPipelines(
              spec, boost::none, std::vector<OwnedLiteParsedPipeline>{std::move(sourcePipeline)}),
          _metadata(std::move(metadata)),
          _returnCursor(returnCursor) {}

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return requiredPrivilegesBasic(isMongos, bypassDocumentValidation);
    }

    bool isInitialSource() const final {
        return true;
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        // Clone the inner sub-pipeline so any view info bound onto it via bindResolvedNamespace()
        // travels with the StageParams to DocumentSource construction.
        return std::make_unique<InternalDocumentResultsAndMetadataStageParams>(
            _metadata, _returnCursor, OwnedLiteParsedPipeline(_pipelines.front()), _shardedPlan);
    }

    void setShardedPlan(ShardedPlanSource source) {
        _shardedPlan = std::move(source);
    }

    bool isRankedStage() const final {
        return _pipelines.front()->isRankedPipeline();
    }

    bool isScoredStage() const final {
        return _pipelines.front()->isScoredPipeline();
    }

    bool isScoreDetailsStage() const final {
        return _pipelines.front()->isScoreDetailsPipeline();
    }

    bool isSelectionStage() const final {
        return _pipelines.front()->isSelectionPipeline();
    }

    FirstStageViewApplicationPolicy getFirstStageViewApplicationPolicy() const override {
        // $_internalDocumentResultsAndMetadata is a source stage; the view (if any) must NOT be
        // prepended in front of it. Any view application happens via bindResolvedNamespace() below,
        // which forwards the view info to the inner source stage this stage wraps.
        return FirstStageViewApplicationPolicy::kDoNothing;
    }

    bool shouldResolveSubpipelineViews() const override {
        // We explicitly handle injecting the view info into the subpipeline in
        // `bindResolvedNamespace`, so we do not need to recurse during view resolution.
        return false;
    }

    void bindResolvedNamespace(const ResolvedNamespace& view,
                               const ResolvedNamespaceMap& resolvedNamespaces) override {
        // Propagate the bound view info to the single source stage so the inner stage can apply the
        // view itself. The constructor guarantees exactly one inner sub-pipeline.
        tassert(12755800,
                str::stream() << "$_internalDocumentResultsAndMetadata expected exactly one inner "
                                 "source sub-pipeline, found "
                              << _pipelines.size(),
                _pipelines.size() == 1);
        _pipelines.front()->bindResolvedNamespaceToStages(view, resolvedNamespaces);
    }

private:
    boost::optional<MetadataBindSpec> _metadata;
    bool _returnCursor;
    // Set when this LiteParsed was constructed from a host-defined AST node carrying the
    // extension's distributed-plan callback. Threaded through getStageParams() to the resulting
    // DocumentSource so distributedPlanLogic() can produce a merge-sort pattern and metadata
    // merge pipeline. Null when constructed via the BSON parse path.
    boost::optional<ShardedPlanSource> _shardedPlan;
};

}  // namespace mongo
