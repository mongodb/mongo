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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/unordered_set.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class BSONElement;
class ExpressionContext;
class NamespaceString;

class DSFacetExecStatsWrapper {
public:
    class StatsProvider {
    public:
        virtual std::vector<Value> getStats(size_t facetId, const SerializationOptions& opts) = 0;
        virtual ~StatsProvider() = default;
    };

    /**
     * Retrieves the execution statistics tracked by the pipeline given by 'facetId'.
     */
    std::vector<Value> getExecStats(size_t facetId, const SerializationOptions& opts) {
        if (!_provider) {
            return {};
        }
        return _provider->getStats(facetId, opts);
    }

    void attachStatsProvider(std::unique_ptr<StatsProvider> provider) {
        _provider = std::move(provider);
    }

    bool isStatsProviderAttached() const {
        return bool(_provider);
    }

private:
    std::unique_ptr<StatsProvider> _provider{nullptr};
};

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
    static constexpr StringData kTeeConsumerStageName = "$internalFacetTeeConsumer"_sd;
    struct FacetPipeline {
        FacetPipeline(std::string name, std::unique_ptr<Pipeline> pipeline)
            : name(std::move(name)), pipeline(std::move(pipeline)) {}

        std::string name;
        std::unique_ptr<Pipeline> pipeline;
    };

    class LiteParsed final : public LiteParsedDocumentSourceNestedPipelines {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options);

        LiteParsed(std::string parseTimeName, std::vector<LiteParsedPipeline> pipelines)
            : LiteParsedDocumentSourceNestedPipelines(
                  std::move(parseTimeName), boost::none, std::move(pipelines)) {}

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return requiredPrivilegesBasic(isMongos, bypassDocumentValidation);
        };

        bool requiresAuthzChecks() const override {
            return false;
        }
    };

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSourceFacet> create(
        std::vector<FacetPipeline> facetPipelines,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        size_t bufferSizeBytes = loadMemoryLimit(StageMemoryLimit::QueryFacetBufferSizeBytes),
        size_t maxOutputDocBytes = internalQueryFacetMaxOutputDocSizeBytes.load());

    /**
     * Optimizes inner pipelines.
     */
    boost::intrusive_ptr<DocumentSource> optimize() final;

    /**
     * Takes a union of all sub-pipelines, and adds them to 'deps'.
     */
    DepsTracker::State getDependencies(DepsTracker* deps) const final;
    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    const char* getSourceName() const final {
        return DocumentSourceFacet::kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

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
    void detachSourceFromOperationContext() final;
    void reattachSourceToOperationContext(OperationContext* opCtx) final;
    bool validateSourceOperationContext(const OperationContext* opCtx) const final;
    StageConstraints constraints(PipelineSplitState pipeState) const final;

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceFacetToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    DocumentSourceFacet(std::vector<FacetPipeline> facetPipelines,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        size_t bufferSizeBytes,
                        size_t maxOutputDocBytes);

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    std::vector<FacetPipeline> _facets;
    std::shared_ptr<DSFacetExecStatsWrapper> _execStatsWrapper;

    const size_t _bufferSizeBytes;
    const size_t _maxOutputDocSizeBytes;
};
}  // namespace mongo
