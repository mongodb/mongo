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
#include <boost/optional.hpp>
#include <memory>
#include <vector>

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {

class BSONElement;
class TeeBuffer;
class DocumentSourceTeeConsumer;
class ExpressionContext;
class NamespaceString;

/**
 * A $facet stage contains multiple sub-pipelines. Each input to the $facet stage will feed into
 * each of the sub-pipelines. The $facet stage is blocking, and outputs only one document,
 * containing an array of results for each sub-pipeline.
 *
 * For example, {$facet: {facetA: [{$skip: 1}], facetB: [{$limit: 1}]}} would describe a $facet
 * stage which will produce a document like the following:
 * {facetA: [<all input documents except the first one>], facetB: [<the first document>]}.
 */
class DocumentSourceFacet final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$facet"_sd;
    struct FacetPipeline {
        FacetPipeline(std::string name, std::unique_ptr<Pipeline, PipelineDeleter> pipeline)
            : name(std::move(name)), pipeline(std::move(pipeline)) {}

        std::string name;
        std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
    };

    class LiteParsed : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const AggregationRequest& request,
                                                 const BSONElement& spec);

        LiteParsed(std::vector<LiteParsedPipeline> liteParsedPipelines, PrivilegeVector privileges)
            : _liteParsedPipelines(std::move(liteParsedPipelines)),
              _requiredPrivileges(std::move(privileges)) {}

        PrivilegeVector requiredPrivileges(bool isMongos) const final {
            return _requiredPrivileges;
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final;

        bool allowShardedForeignCollection(NamespaceString nss) const final;

        bool allowedToPassthroughFromMongos() const {
            // If any of the sub-pipelines doesn't allow pass through, then return false.
            return std::all_of(_liteParsedPipelines.cbegin(),
                               _liteParsedPipelines.cend(),
                               [](const auto& subPipeline) {
                                   return subPipeline.allowedToPassthroughFromMongos();
                               });
        }

    private:
        const std::vector<LiteParsedPipeline> _liteParsedPipelines;
        const PrivilegeVector _requiredPrivileges;
    };

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSourceFacet> create(
        std::vector<FacetPipeline> facetPipelines,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Optimizes inner pipelines.
     */
    boost::intrusive_ptr<DocumentSource> optimize() final;

    /**
     * Takes a union of all sub-pipelines, and adds them to 'deps'.
     */
    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    const char* getSourceName() const final {
        return DocumentSourceFacet::kStageName.rawData();
    }

    /**
     * Sets 'source' as the source of '_teeBuffer'.
     */
    void setSource(DocumentSource* source) final;

    /**
     * The $facet stage must be run on the merging shard.
     *
     * TODO SERVER-24154: Should be smarter about splitting so that parts of the sub-pipelines can
     * potentially be run in parallel on multiple shards.
     */
    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        // {shardsStage, mergingStage, sortPattern}
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    const std::vector<FacetPipeline>& getFacetPipelines() const {
        return _facets;
    }

    auto& getFacetPipelines() {
        return _facets;
    }

    // The following are overridden just to forward calls to sub-pipelines.
    void addInvolvedCollections(stdx::unordered_set<NamespaceString>* involvedNssSet) const final;
    void detachFromOperationContext() final;
    void reattachToOperationContext(OperationContext* opCtx) final;
    StageConstraints constraints(Pipeline::SplitState pipeState) const final;
    bool usedDisk() final;

protected:
    /**
     * Blocking call. Will consume all input and produces one output document.
     */
    GetNextResult doGetNext() final;
    void doDispose() final;

private:
    DocumentSourceFacet(std::vector<FacetPipeline> facetPipelines,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    boost::intrusive_ptr<TeeBuffer> _teeBuffer;
    std::vector<FacetPipeline> _facets;

    bool _done = false;
};
}  // namespace mongo
