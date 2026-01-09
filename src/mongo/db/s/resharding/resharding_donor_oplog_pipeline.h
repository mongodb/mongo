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

#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface_factory.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {
class ReshardingDonorOplogPipelineInterface {
public:
    virtual ~ReshardingDonorOplogPipelineInterface() = default;

    class ScopedPipeline {
    public:
        explicit ScopedPipeline(OperationContext* opCtx,
                                ReshardingDonorOplogPipelineInterface* pipeline);
        ~ScopedPipeline();

        std::vector<repl::OplogEntry> getNextBatch(size_t batchSize);

        /**
         * Cleanly detaches the pipeline from the operation context. This must be called in order to
         * keep the pipeline. Otherwise, disposed() will be called if it was not cleanly detached
         * when this ScopedPipeline is destroyed.
         */
        void detachFromOperationContext();

    private:
        OperationContext* _opCtx;
        ReshardingDonorOplogPipelineInterface* _pipeline;
    };

    /**
     * Initializes the pipeline with the given operation context. This will reattach to the stored
     * pipeline if pipeline was cleanly detached. Otherwise, it will initialize with a new pipeline.
     */
    virtual ScopedPipeline initWithOperationContext(OperationContext* opCtx,
                                                    ReshardingDonorOplogId resumeToken) = 0;

    /**
     * Resets the internal pipeline state.
     */
    virtual void dispose(OperationContext* opCtx) = 0;

protected:
    /**
     * Detaches the pipeline from the operation context. The pipeline must be initialized again with
     * initWithOperationContext() before it can be used again.
     */
    virtual void _detachFromOperationContext() = 0;

    /**
     * Returns the next batch.
     */
    virtual std::vector<repl::OplogEntry> _getNextBatch(size_t batchSize) = 0;
};

/**
 * Class responsible for fetching oplog entries from the local resharding oplog buffer collection.
 *
 * Instances of this class are not thread-safe.
 */
class ReshardingDonorOplogPipeline : public ReshardingDonorOplogPipelineInterface {
public:
    ReshardingDonorOplogPipeline(
        NamespaceString oplogBufferNss,
        std::unique_ptr<MongoProcessInterfaceFactory> mongoProcessInterfaceFactory);

    ScopedPipeline initWithOperationContext(OperationContext* opCtx,
                                            ReshardingDonorOplogId resumeToken) override;

    void dispose(OperationContext* opCtx) override;

protected:
    void _detachFromOperationContext() override;

    std::vector<repl::OplogEntry> _getNextBatch(size_t batchSize) override;

private:
    /**
     * Returns a pipeline that can be used for executing the query to fetch the oplog.
     */
    std::unique_ptr<Pipeline> _makePipeline(
        OperationContext* opCtx,
        std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
        ReshardingDonorOplogId resumeToken);

    const NamespaceString _oplogBufferNss;

    // The raw pipeline that contains the stages for fetching the oplog entries.
    // This by itself is not executable but is kept alive for _execPipeline.
    std::unique_ptr<Pipeline> _pipeline;

    // The pipeline that is for fetching the oplog entries and is fully executable.
    std::unique_ptr<exec::agg::Pipeline> _execPipeline;

    std::unique_ptr<MongoProcessInterfaceFactory> _mongoProcessInterfaceFactory;
};

}  // namespace mongo
