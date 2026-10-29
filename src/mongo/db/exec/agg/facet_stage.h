// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/tee_buffer.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class FacetStage final : public Stage {
public:
    struct FacetPipeline {
        FacetPipeline(std::string name, std::unique_ptr<Pipeline> execPipeline)
            : name(std::move(name)), execPipeline(std::move(execPipeline)) {}

        std::string name;
        std::unique_ptr<Pipeline> execPipeline;
    };

    FacetStage(std::string_view stageName,
               const std::vector<DocumentSourceFacet::FacetPipeline>& facetPipelines,
               const boost::intrusive_ptr<ExpressionContext>& expCtx,
               size_t bufferSizeBytes,
               size_t maxOutputDocBytes,
               const std::shared_ptr<DSFacetExecStatsWrapper>& execStatsWrapper);
    ~FacetStage() final;

    bool usedDisk() const final;

    const SpecificStats* getSpecificStats() const final {
        return &_stats;
    }

    void detachFromOperationContext() final;
    void reattachToOperationContext(OperationContext* opCtx) final;
    bool validateOperationContext(const OperationContext* opCtx) const final;

private:
    /**
     * Sets 'source' as the source of '_teeBuffer'.
     */
    void setSource(Stage* source) final;

    /**
     * Blocking call. Will consume all input and produces one output document.
     */
    GetNextResult doGetNext() final;
    void doDispose() final;

    boost::intrusive_ptr<TeeBuffer> _teeBuffer;
    std::vector<FacetPipeline> _facets;

    const size_t _maxOutputDocSizeBytes;

    bool _done = false;

    DocumentSourceFacetStats _stats;
    std::shared_ptr<DSFacetExecStatsWrapper> _execStatsWrapper;
};

}  // namespace mongo::exec::agg
