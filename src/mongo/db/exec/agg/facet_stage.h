/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/tee_buffer.h"

#include <cstddef>

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

    FacetStage(StringData stageName,
               const std::vector<DocumentSourceFacet::FacetPipeline>& facetPipelines,
               const boost::intrusive_ptr<ExpressionContext>& expCtx,
               size_t bufferSizeBytes,
               size_t maxOutputDocBytes,
               const std::shared_ptr<DSFacetExecStatsWrapper>& execStatsWrapper);
    ~FacetStage() final;

    /**
     * Sets 'source' as the source of '_teeBuffer'.
     */
    void setSource(Stage* source) final;

    bool usedDisk() const final;

    const SpecificStats* getSpecificStats() const final {
        return &_stats;
    }

    void detachFromOperationContext() final;
    void reattachToOperationContext(OperationContext* opCtx) final;
    bool validateOperationContext(const OperationContext* opCtx) const final;

private:
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
