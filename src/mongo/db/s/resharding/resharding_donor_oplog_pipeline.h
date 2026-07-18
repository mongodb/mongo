// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface_factory.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/db/s/resharding/resharding_executable_pipeline.h"
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

    resharding::ReshardingExecutablePipeline _pipeline;

    std::unique_ptr<MongoProcessInterfaceFactory> _mongoProcessInterfaceFactory;
};

}  // namespace mongo
