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
