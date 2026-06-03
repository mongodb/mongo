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

#include "mongo/base/string_data.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_internal_document_results_and_metadata.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <set>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

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
    static constexpr StringData kStageName = "$_internalDocumentResultsAndMetadata"_sd;

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

    StringData getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const override;

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        // TODO: SERVER-126255
        return boost::none;
    }

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

    DocumentSourceInternalDocumentResultsAndMetadata(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::intrusive_ptr<DocumentSource> sourceStage,
        boost::optional<MetadataBindSpec> metadata,
        bool returnCursor);

private:
    boost::intrusive_ptr<DocumentSource> _sourceStage;
    boost::optional<MetadataBindSpec> _metadata;
    bool _returnCursor;
};

}  // namespace mongo
