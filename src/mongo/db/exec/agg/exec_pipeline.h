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

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/platform/compiler.h"

#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
class Pipeline {
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
     * Method to accumulate the plan summary stats from all stages of the pipeline into the given
     * `planSummaryStats` object.
     */
    void accumulatePlanSummaryStats(PlanSummaryStats& planSummaryStats) const;

    /**
     * Sets the OperationContext of 'expCtx' to nullptr and calls 'detachFromOperationContext()' on
     * all underlying DocumentSources.
     */
    void detachFromOperationContext();

    /**
     * Sets the OperationContext of 'expCtx' to 'opCtx', and reattaches all underlying
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
        return expCtx;
    }

    /**
     * Write the pipeline's operators to a std::vector<Value>, providing the level of detail
     * specified by 'verbosity'.
     */
    std::vector<Value> writeExplainOps(
        const SerializationOptions& opts = SerializationOptions{}) const;

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

    /**
     * Deactivates disposing the pipeline in the destructor.
     */
    void dismissDisposal() {
        _disposeInDestructor = false;
    }

private:
    // The '_stages' container is guaranteed to be non-empty after the constructor successfully
    // executed.
    StageContainer _stages;

    boost::intrusive_ptr<ExpressionContext> expCtx;
    bool _disposed{false};

    // Call 'dispose()' in destructor.
    bool _disposeInDestructor{true};
};
}  // namespace mongo::exec::agg
