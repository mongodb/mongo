// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/query/query_stats/data_bearing_node_metrics.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <memory>

#include <boost/optional.hpp>

namespace mongo {

class OperationContext;

/**
 * This is the lightweight mongoS analogue of the PlanStage abstraction used to execute queries on
 * mongoD (see mongo/db/plan_stage.h).
 *
 * Each subclass is a query execution stage which executes on the merging node. In general, the
 * execution plan on mongos could have a tree of execution stages, but currently each node has at
 * most one child. The leaf stage of the pipeline receives query result documents merged from the
 * shards. The pipeline may then transform the result set in various ways before being returned by
 * the root stage.
 */
class RouterExecStage {
public:
    RouterExecStage(OperationContext* opCtx) : _opCtx(opCtx) {}
    RouterExecStage(OperationContext* opCtx, std::unique_ptr<RouterExecStage> child)
        : _opCtx(opCtx), _child(std::move(child)) {}

    virtual ~RouterExecStage() = default;

    /**
     * Returns the next query result, or an error.
     *
     * If there are no more results, returns an EOF ClusterQueryResult.
     *
     * All returned BSONObjs are owned. They may own a buffer larger than the object. If you are
     * holding on to a subset of the returned results and need to minimize memory usage, call copy()
     * on the BSONObjs.
     */
    virtual StatusWith<ClusterQueryResult> next() = 0;

    virtual Status releaseMemory() {
        tassert(9745608,
                "router stage should have a child or provide alternative implementation",
                _child);
        return _child->releaseMemory();
    }

    /**
     * Must be called before destruction to abandon a not-yet-exhausted plan. May block waiting for
     * responses from remote hosts.
     *
     * Note that 'opCtx' may or may not be the same as the operation context to which this cursor is
     * currently attached. This is so that a killing thread may call this method with its own
     * operation context.
     */
    virtual void kill(OperationContext* opCtx) {
        tassert(11052344,
                "router stage should have a child or provide alternative implementation",
                _child);  // The default implementation forwards to the child stage.
        _child->kill(opCtx);
    }

    /**
     * Returns true if only a subset of the all relevant results are being returned by this cursor.
     * Only applicable if the 'allowPartialResults' option was enabled in the query request.
     */
    virtual bool partialResultsReturned() const {
        return _child ? _child->partialResultsReturned() : false;
    }

    /**
     * Returns the number of remote hosts involved in this execution plan.
     */
    virtual std::size_t getNumRemotes() const {
        tassert(11052345,
                "router stage should have a child or provide alternative implementation",
                _child);  // The default implementation forwards to the child stage.
        return _child->getNumRemotes();
    }

    /**
     * Returns whether or not all the remote cursors are exhausted.
     */
    virtual bool remotesExhausted() const {
        tassert(11052346,
                "router stage should have a child or provide alternative implementation",
                _child);  // The default implementation forwards to the child stage.
        return _child->remotesExhausted();
    }

    /*
     * Returns true if there are no more results to return. Can be false-negative, but not
     * false-positive. By default returns false. Stages that can easily determine if they reached
     * EOF should implement this.
     */
    virtual bool isEOF() const {
        return false;
    }

    /**
     * Sets the maxTimeMS value that the cursor should forward with any internally issued getMore
     * requests.
     *
     * Returns a non-OK status if this cursor type does not support maxTimeMS on getMore (i.e. if
     * the cursor is not tailable + awaitData).
     */
    Status setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
        if (_child) {
            auto childStatus = _child->setAwaitDataTimeout(awaitDataTimeout);
            if (!childStatus.isOK()) {
                return childStatus;
            }
        }
        return doSetAwaitDataTimeout(awaitDataTimeout);
    }

    /**
     * Returns the postBatchResumeToken if this RouterExecStage tree is executing a $changeStream;
     * otherwise, returns an empty BSONObj. Default implementation forwards to the stage's child.
     */
    virtual BSONObj getPostBatchResumeToken() {
        return _child ? _child->getPostBatchResumeToken() : BSONObj();
    }

    /**
     * Sets the current operation context to be used by the router stage.
     */
    void reattachToOperationContext(OperationContext* opCtx) {
        invariant(!_opCtx);
        _opCtx = opCtx;

        if (_child) {
            _child->reattachToOperationContext(opCtx);
        }

        doReattachToOperationContext();
    }

    /**
     * Discards the stage's current OperationContext, setting it to 'nullptr'.
     */
    void detachFromOperationContext() {
        invariant(_opCtx);
        _opCtx = nullptr;

        if (_child) {
            _child->detachFromOperationContext();
        }

        doDetachFromOperationContext();
    }

    /**
     * Returns a pointer to the current OperationContext, or nullptr if there is no context.
     */
    OperationContext* getOpCtx() {
        return _opCtx;
    }

    /**
     * Returns any metrics gathered from remote hosts and resets the stored values to a default
     * state so as to avoid double-counting.
     */
    virtual boost::optional<query_stats::DataBearingNodeMetrics> takeRemoteMetrics() {
        return _child ? _child->takeRemoteMetrics() : boost::none;
    }

protected:
    /**
     * Performs any stage-specific reattach actions. Called after the OperationContext has been set
     * and is available via getOpCtx().
     */
    virtual void doReattachToOperationContext() {}

    /**
     * Performs any stage-specific detach actions. Called after the OperationContext has been set to
     * nullptr.
     */
    virtual void doDetachFromOperationContext() {}

    /**
     * Performs any stage-specific await data timeout actions.
     */
    virtual Status doSetAwaitDataTimeout(Milliseconds awaitDataTimeout) {
        return Status::OK();
    }

    /**
     * Returns an unowned pointer to the child stage, or nullptr if there is no child.
     */
    RouterExecStage* getChildStage() const {
        return _child.get();
    }

private:
    OperationContext* _opCtx = nullptr;
    std::unique_ptr<RouterExecStage> _child;
};

}  // namespace mongo
