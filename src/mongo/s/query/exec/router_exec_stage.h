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
        invariant(_child);  // The default implementation forwards to the child stage.
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
        invariant(_child);  // The default implementation forwards to the child stage.
        return _child->getNumRemotes();
    }

    /**
     * Returns whether or not all the remote cursors are exhausted.
     */
    virtual bool remotesExhausted() const {
        invariant(_child);  // The default implementation forwards to the child stage.
        return _child->remotesExhausted();
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
