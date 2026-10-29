// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * TODO SERVER-112775: Remove 'server_backup_restore' dependency on this class.
 * TODO SERVER-112776: Remove 'data_movement' dependency on this class.
 * TODO SERVER-112777: Remove 'atlas_streams' dependency on this class.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] Pipeline {
public:
    using StageContainer = std::vector<StagePtr>;

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    Pipeline(StageContainer&& stages, boost::intrusive_ptr<ExpressionContext> expCtx);

    ~Pipeline();

    const StageContainer& getStages() const {
        return _stages;
    }

    /**
     * Returns the next document from the pipeline, or boost::none if there are no more documents.
     */
    boost::optional<Document> getNext();

    /**
     * Returns the next result from the pipeline.
     */
    MONGO_COMPILER_ALWAYS_INLINE GetNextResult getNextResult() {
        return _stages.back()->getNext();
    }

    /**
     * Returns true if the last stage in the pipeline reports EOF. This is a conservative
     * check: it may return false even when the pipeline is actually exhausted, but will
     * never return true when there is more data available.
     */
    bool isEOF() const {
        return _stages.back()->isEOF();
    }

    /**
     * Method to accumulate the plan summary stats from all stages of the pipeline into the given
     * `planSummaryStats` object.
     */
    void accumulatePlanSummaryStats(PlanSummaryStats& planSummaryStats) const;

    /**
     * Sets the OperationContext of '_expCtx' to nullptr and calls 'detachFromOperationContext()' on
     * all underlying DocumentSources.
     */
    void detachFromOperationContext();

    /**
     * Sets the OperationContext of '_expCtx' to 'opCtx', and reattaches all underlying
     * DocumentSources to 'opCtx'.
     */
    void reattachToOperationContext(OperationContext* opCtx);

    /**
     * Recursively validate the operation contexts associated with this pipeline. Return true if
     * all document sources and subpipelines point to the given operation context.
     */
    bool validateOperationContext(const OperationContext* opCtx) const;

    /**
     * Asserts whether operation contexts associated with this pipeline are consistent across
     * sources.
     */
    void checkValidOperationContext() const;

    const boost::intrusive_ptr<ExpressionContext>& getContext() const {
        return _expCtx;
    }

    /**
     * Write the pipeline's operators to a std::vector<Value>, providing the level of detail
     * specified by 'verbosity'.
     */
    std::vector<Value> writeExplainOps(
        const query_shape::SerializationOptions& opts = query_shape::SerializationOptions{}) const;

    void forceSpill();

    /**
     * Checks to see if disk is ever used within the pipeline.
     */
    bool usedDisk() const;

    /**
     * Releases any resources held by this pipeline such as PlanExecutors or in-memory structures.
     * Must be called before deleting a Pipeline. There are multiple cleanup scenarios:
     *  - This Pipeline will only ever use one OperationContext. In this case the destructor will
     *    automatically call 'dispose()' before deleting the Pipeline, and the owner does not need
     * to call 'dispose()'.
     *  - This Pipeline may use multiple OperationContexts over its lifetime. In this case it is the
     *    owner's responsibility to call 'dispose()' with a valid OperationContext being installed
     *    before deleting the Pipeline.
     */
    void dispose();

    bool isDisposed() const {
        return _disposed;
    }

private:
    // The '_stages' container is guaranteed to be non-empty after the constructor successfully
    // executed.
    StageContainer _stages;

    boost::intrusive_ptr<ExpressionContext> _expCtx;
    bool _disposed{false};
};
}  // namespace mongo::exec::agg
