// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/facet_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/agg/tee_consumer_stage.h"
#include "mongo/db/pipeline/document_source_facet.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceFacetToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource = dynamic_cast<DocumentSourceFacet*>(source.get());

    tassert(10537000, "expected 'DocumentSourceFacet' type", documentSource);

    return make_intrusive<exec::agg::FacetStage>(documentSource->kStageName,
                                                 documentSource->_facets,
                                                 documentSource->getExpCtx(),
                                                 documentSource->_bufferSizeBytes,
                                                 documentSource->_maxOutputDocSizeBytes,
                                                 documentSource->_execStatsWrapper);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(facetStage, DocumentSourceFacet::id, documentSourceFacetToStageFn);

namespace {

class StatsProviderImpl : public DSFacetExecStatsWrapper::StatsProvider {
public:
    StatsProviderImpl(const std::vector<FacetStage::FacetPipeline>& facets) : _facets(facets) {}
    std::vector<Value> getStats(size_t facetId,
                                const query_shape::SerializationOptions& opts) override {
        return _facets[facetId].execPipeline->writeExplainOps(opts);
    }

private:
    // The lifetime of this object is bound to the lifetime of the FacetStage, so the reference is
    // always valid.
    const std::vector<FacetStage::FacetPipeline>& _facets;
};
}  // namespace

FacetStage::FacetStage(std::string_view stageName,
                       const std::vector<DocumentSourceFacet::FacetPipeline>& facetPipelines,
                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       size_t bufferSizeBytes,
                       size_t maxOutputDocBytes,
                       const std::shared_ptr<DSFacetExecStatsWrapper>& execStatsWrapper)
    : Stage(stageName, expCtx),
      _teeBuffer(TeeBuffer::create(facetPipelines.size(), bufferSizeBytes)),
      _maxOutputDocSizeBytes(maxOutputDocBytes),
      _execStatsWrapper(execStatsWrapper) {
    _facets.reserve(facetPipelines.size());
    for (size_t facetId = 0; facetId < facetPipelines.size(); ++facetId) {
        auto& facet = facetPipelines[facetId];
        tassert(10537001, "$facet sub-pipeline is empty", !facet.pipeline->empty());
        FacetPipeline facetPipeline{facet.name, buildPipeline(facet.pipeline->freeze())};
        auto teeConsumerStage =
            dynamic_cast<TeeConsumerStage*>(facetPipeline.execPipeline->getStages().front().get());
        tassert(
            10537002, "$facet sub-pipeline does not start with TeeConsumerStage", teeConsumerStage);
        teeConsumerStage->setTeeBuffer(_teeBuffer);
        _facets.push_back(std::move(facetPipeline));
    }
    auto statsProvider = std::make_unique<StatsProviderImpl>(_facets);
    _execStatsWrapper->attachStatsProvider(std::move(statsProvider));
}

FacetStage::~FacetStage() {
    // Reset the provider.
    _execStatsWrapper->attachStatsProvider(nullptr);
}

void FacetStage::setSource(Stage* source) {
    _teeBuffer->setSource(source);
}

bool FacetStage::usedDisk() const {
    return _done ? _stats.planSummaryStats.usedDisk
                 : std::any_of(_facets.begin(), _facets.end(), [](const auto& facet) {
                       return facet.execPipeline && facet.execPipeline->usedDisk();
                   });
}

void FacetStage::doDispose() {
    for (auto&& facet : _facets) {
        facet.execPipeline->dispose();
    }
}

DocumentSource::GetNextResult FacetStage::doGetNext() {
    if (_done) {
        // stats update (previously done in usedDisk()).
        _stats.planSummaryStats.usedDisk =
            std::any_of(_facets.begin(), _facets.end(), [](const auto& facet) {
                return facet.execPipeline && facet.execPipeline->usedDisk();
            });

        return GetNextResult::makeEOF();
    }

    const size_t maxBytes = _maxOutputDocSizeBytes;
    auto ensureUnderMemoryLimit = [usedBytes = 0ul, &maxBytes](long long additional) mutable {
        usedBytes += additional;
        uassert(ErrorCodes::ExceededMemoryLimit,
                str::stream() << "document constructed by $facet is " << usedBytes
                              << " bytes, which exceeds the limit of " << maxBytes << " bytes",
                usedBytes <= maxBytes);
    };

    std::vector<std::vector<Value>> results(_facets.size());
    bool allPipelinesEOF = false;
    while (!allPipelinesEOF) {
        allPipelinesEOF = true;  // Set this to false if any pipeline isn't EOF.
        for (size_t facetId = 0; facetId < _facets.size(); ++facetId) {
            auto& execPipeline = *_facets[facetId].execPipeline;
            auto next = execPipeline.getNextResult();
            for (; next.isAdvanced(); next = execPipeline.getNextResult()) {
                ensureUnderMemoryLimit(next.getDocument().getApproximateSize());
                results[facetId].emplace_back(next.releaseDocument());
            }
            allPipelinesEOF = allPipelinesEOF && next.isEOF();
            execPipeline.accumulatePlanSummaryStats(_stats.planSummaryStats);
        }
    }

    MutableDocument resultDoc;
    for (size_t facetId = 0; facetId < _facets.size(); ++facetId) {
        resultDoc[_facets[facetId].name] = Value(std::move(results[facetId]));
    }

    _done = true;  // We will only ever produce one result.
    return resultDoc.freeze();
}

void FacetStage::detachFromOperationContext() {
    for (auto&& facet : _facets) {
        facet.execPipeline->detachFromOperationContext();
    }
}

void FacetStage::reattachToOperationContext(OperationContext* opCtx) {
    for (auto&& facet : _facets) {
        facet.execPipeline->reattachToOperationContext(opCtx);
    }
}

bool FacetStage::validateOperationContext(const OperationContext* opCtx) const {
    return getContext()->getOperationContext() == opCtx &&
        std::all_of(_facets.begin(), _facets.end(), [opCtx](const auto& f) {
               return (!f.execPipeline || f.execPipeline->validateOperationContext(opCtx));
           });
}
}  // namespace exec::agg
}  // namespace mongo
