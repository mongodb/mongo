// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_document_source_nested_pipelines.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class BSONElement;
class ExpressionContext;
class NamespaceString;

class DSFacetExecStatsWrapper {
public:
    class StatsProvider {
    public:
        virtual std::vector<Value> getStats(size_t facetId,
                                            const query_shape::SerializationOptions& opts) = 0;
        virtual ~StatsProvider() = default;
    };

    /**
     * Retrieves the execution statistics tracked by the pipeline given by 'facetId'.
     */
    std::vector<Value> getExecStats(size_t facetId, const query_shape::SerializationOptions& opts) {
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

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(Facet);

/**
 * A $facet stage contains multiple sub-pipelines. Each input to the $facet stage will feed into
 * each of the sub-pipelines. The $facet stage is blocking, and outputs only one document,
 * containing an array of results for each sub-pipeline.
 *
 * For example, {$facet: {facetA: [{$skip: 1}], facetB: [{$limit: 1}]}} would describe a $facet
 * stage which will produce a document like the following:
 * {facetA: [<all input documents except the first one>], facetB: [<the first document>]}.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] DocumentSourceFacet final : public DocumentSource {
public:
    [[MONGO_MOD_NEEDS_REPLACEMENT]] static constexpr std::string_view kStageName = "$facet"sv;
    static constexpr std::string_view kTeeConsumerStageName = "$internalFacetTeeConsumer"sv;
    struct FacetPipeline {
        FacetPipeline(std::string name, std::unique_ptr<Pipeline> pipeline)
            : name(std::move(name)), pipeline(std::move(pipeline)) {}

        std::string name;
        std::unique_ptr<Pipeline> pipeline;
    };

    class LiteParsed final : public LiteParsedDocumentSourceNestedPipelines<LiteParsed> {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options);

        LiteParsed(const BSONElement& spec, std::vector<OwnedLiteParsedPipeline> pipelines)
            : LiteParsedDocumentSourceNestedPipelines(spec, boost::none, std::move(pipelines)) {}

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return requiredPrivilegesBasic(isMongos, bypassDocumentValidation);
        };

        bool requiresAuthzChecks() const override {
            return false;
        }

        std::unique_ptr<StageParams> getStageParams() const override {
            return std::make_unique<FacetStageParams>(_originalBson);
        }
    };

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSourceFacet> create(
        std::vector<FacetPipeline> facetPipelines,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        size_t bufferSizeBytes = loadMemoryLimit(StageMemoryLimit::QueryFacetBufferSizeBytes).get(),
        size_t maxOutputDocBytes = internalQueryFacetMaxOutputDocSizeBytes.load());

    /**
     * Optimizes inner pipelines.
     */
    boost::intrusive_ptr<DocumentSource> optimize();

    /**
     * Takes a union of all sub-pipelines, and adds them to 'deps'.
     */
    DepsTracker::State getDependencies(DepsTracker* deps) const final;
    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    std::string_view getSourceName() const final {
        return DocumentSourceFacet::kStageName;
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
    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
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

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    std::vector<FacetPipeline> _facets;
    std::shared_ptr<DSFacetExecStatsWrapper> _execStatsWrapper;

    const size_t _bufferSizeBytes;
    const size_t _maxOutputDocSizeBytes;
};
}  // namespace mongo
