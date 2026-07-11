// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_internal_document_results_and_metadata.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <functional>
#include <set>
#include <string_view>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * The $_internalDocumentResultsAndMetadata stage wraps a source stage that emits two separate
 * streams: a document-results stream and a metadata stream. The stage definition looks like:
 *
 *   {
 *     $_internalDocumentResultsAndMetadata: {
 *       source: { <DocumentSource> },
 *       metadata: { as: <PipelineVariable> }, // Optional
 *       returnCursor: false // Default false.
 *     }
 *   }
 *
 * The 'source' is a single agg stage (e.g. $search) that produces both document results stream and
 * a metadata stream. During pipeline translation this stage is compiled into a
 * DocumentSourceExchange that fans the results output into two consumer pipelines: the document
 * stream flows into the top-level pipeline, while the metadata stream is handled according to the
 * 'metadata' and 'returnCursor' fields.
 *
 * If 'metadata' is present, the metadata stream is bound to the named pipeline variable
 * (e.g. $$SEARCH_META) via $setVariableFromSubPipeline, making it available to downstream
 * stages. optimizeAt() removes 'metadata' when no downstream stage references the
 * variable, eliding the metadata consumer entirely.
 *
 * On sharded queries the router serializes this stage to shards with 'returnCursor: true'.
 * In that mode the metadata stream is returned as a secondary cursor alongside the main results
 * cursor, and the router inserts a $setVariableFromSubPipeline in the merging pipeline to read
 * from it.
 */
class DocumentSourceInternalDocumentResultsAndMetadata final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$_internalDocumentResultsAndMetadata"sv;

    using ShardedPlanSpec = mongo::ShardedPlanSpec;
    using ShardedPlanSource = mongo::ShardedPlanSource;

    /**
     * Creates a DocumentSource from StageParams produced by the LiteParsed layer.
     */
    static DocumentSourceContainer createFromStageParams(
        const InternalDocumentResultsAndMetadataStageParams& params,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Constructs the stage directly from already-validated arguments.
     */
    static boost::intrusive_ptr<DocumentSourceInternalDocumentResultsAndMetadata> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::intrusive_ptr<DocumentSource> source,
        boost::optional<MetadataBindSpec> metadata,
        bool returnCursor = false);

    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const override;

    DepsTracker::State getDependencies(DepsTracker* deps) const override;

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    void serializeToArray(std::vector<Value>& array,
                          const query_shape::SerializationOptions& opts =
                              query_shape::SerializationOptions{}) const final;

    // This stage lowers to multiple exec::agg stages (Exchange + $replaceRoot [+ $setVar]) at
    // build time, so at executionStats verbosity serializeToArray() emits one entry per exec stage.
    bool serializesToMultipleExecStatsExplainOps() const final {
        return true;
    }

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const final;

    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final;

    // Returns true if the current stage can move past this stage to the shard side during
    // pipeline splitting. Blocks stages that reference $$SEARCH_META or don't preserve order
    // and metadata.
    static bool canMovePastDuringSplit(const DocumentSource& ds);

    // The metadata variable is set by this stage, not referenced by it.
    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    const boost::intrusive_ptr<DocumentSource>& getSourceStage() const {
        return _sourceStage;
    }

    const boost::optional<MetadataBindSpec>& getMetadata() const {
        return _metadata;
    }

    bool getReturnCursor() const {
        return _returnCursor;
    }

    void setShardedPlan(ShardedPlanSource source) {
        _shardedPlan = std::move(source);
    }

    void stashAdditionalCursorPipeline(std::unique_ptr<Pipeline> pipeline) {
        tassert(12615009, "Additional cursor already stashed", !_additionalCursorPipeline);
        _additionalCursorPipeline = std::move(pipeline);
    }

    // The additional cursor pipeline is stashed by the exec translation function when
    // returnCursor is true. run_aggregate.cpp takes it once via takeAdditionalCursorPipeline()
    // to register as a secondary SearchMetaResult executor. After taking, it becomes null.
    bool hasAdditionalCursorPipeline() const {
        return _additionalCursorPipeline != nullptr;
    }

    std::unique_ptr<Pipeline> takeAdditionalCursorPipeline() {
        return std::move(_additionalCursorPipeline);
    }

    DocumentSourceInternalDocumentResultsAndMetadata(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::intrusive_ptr<DocumentSource> sourceStage,
        boost::optional<MetadataBindSpec> metadata,
        bool returnCursor);

private:
    void elideMetadata();

    boost::intrusive_ptr<DocumentSource> _sourceStage;
    boost::optional<MetadataBindSpec> _metadata;
    bool _returnCursor;
    // Stashed by the exec translation function when returnCursor is true. run_aggregate.cpp takes
    // it via takeAdditionalCursorPipeline() to register as a secondary SearchMetaResult executor.
    std::unique_ptr<Pipeline> _additionalCursorPipeline;
    // Source of the sharded distributed-plan info, if it exists: either a live callback from the
    // extension AST node, or the spec the router cached into the IDL when serializing this stage
    // into a subpipeline
    boost::optional<ShardedPlanSource> _shardedPlan;
};

}  // namespace mongo
