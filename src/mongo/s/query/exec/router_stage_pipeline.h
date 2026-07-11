// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/exec/merge_cursors_stage.h"
#include "mongo/util/duration.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Inserts a pipeline into the router execution tree, drawing results from the input stage, feeding
 * them through the pipeline, and outputting the results of the pipeline.
 */
class RouterStagePipeline final : public RouterExecStage {
public:
    RouterStagePipeline(std::unique_ptr<Pipeline> mergePipeline);

    StatusWith<ClusterQueryResult> next() final;

    Status releaseMemory() final {
        try {
            if (_mergeCursorsStage) {
                _mergeCursorsStage->forceSpill();
            }
            // When I have the bytes I will return them from here. Now, I have nothing to return.
            return Status::OK();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    }

    void kill(OperationContext* opCtx) final;

    bool remotesExhausted() const final;

    std::size_t getNumRemotes() const final;

    BSONObj getPostBatchResumeToken() final;

    bool isEOF() const final {
        return _mergeExecPipeline->isEOF();
    }

    boost::optional<query_stats::DataBearingNodeMetrics> takeRemoteMetrics() final {
        if (_mergeCursorsStage) {
            return _mergeCursorsStage->takeRemoteMetrics();
        }
        return boost::none;
    }

protected:
    Status doSetAwaitDataTimeout(Milliseconds awaitDataTimeout) final;

    void doReattachToOperationContext() final;

    void doDetachFromOperationContext() final;

private:
    BSONObj _validateAndConvertToBSON(const Document& event);

    // TODO SERVER-105521 Consider removing the '_mergePipeline' member
    // ('_mergeExecPipeline' should suffice).
    std::unique_ptr<Pipeline> _mergePipeline;
    std::unique_ptr<exec::agg::Pipeline> _mergeExecPipeline;

    // May be null if this pipeline runs exclusively on mongos without contacting the shards at all.
    boost::intrusive_ptr<exec::agg::MergeCursorsStage> _mergeCursorsStage;
};
}  // namespace mongo
