/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata_gen.h"
#include "mongo/db/pipeline/lite_parsed_document_source_nested_pipelines.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"

#include <functional>
#include <memory>
#include <vector>

namespace mongo {

class ExpressionContext;

/**
 * Sort pattern for merge-sorting the document results stream across shards, and the merge pipeline
 * for the metadata stream on the router. Provided by the extension source via a callback that
 * survives the LiteParsed -> StageParams -> DocumentSource handoff.
 */
struct DocResultsShardedPlanSpec {
    BSONObj resultsSortPattern;
    std::vector<BSONObj> metaMergePipeline;
};

using DocResultsShardedPlanProvider =
    std::function<const DocResultsShardedPlanSpec&(ExpressionContext*)>;

/**
 * Stage params produced by InternalDocumentResultsAndMetadataLiteParsed and consumed by
 * DocumentSourceInternalDocumentResultsAndMetadata::createFromStageParams.
 */
class InternalDocumentResultsAndMetadataStageParams : public StageParams {
public:
    InternalDocumentResultsAndMetadataStageParams(boost::optional<MetadataBindSpec> metadata,
                                                  bool returnCursor,
                                                  OwnedLiteParsedPipeline sourcePipeline,
                                                  DocResultsShardedPlanProvider shardedPlanProvider)
        : _metadata(std::move(metadata)),
          _returnCursor(returnCursor),
          _sourcePipeline(std::move(sourcePipeline)),
          _shardedPlanProvider(std::move(shardedPlanProvider)) {}

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

    const DocResultsShardedPlanProvider& getShardedPlanProvider() const {
        return _shardedPlanProvider;
    }

private:
    boost::optional<MetadataBindSpec> _metadata;
    bool _returnCursor;
    OwnedLiteParsedPipeline _sourcePipeline;
    // Carries the extension's DPL callback through the LiteParsed -> StageParams handoff so the
    // resulting DocumentSource can wire up its sharded plan logic. Null on non-extension paths.
    DocResultsShardedPlanProvider _shardedPlanProvider;
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
            _metadata,
            _returnCursor,
            OwnedLiteParsedPipeline(_pipelines.front()),
            _shardedPlanProvider);
    }

    void setShardedPlanProvider(DocResultsShardedPlanProvider provider) {
        _shardedPlanProvider = std::move(provider);
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
    DocResultsShardedPlanProvider _shardedPlanProvider;
};

}  // namespace mongo
