/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/s/query/cluster_query_result.h"
#include "mongo/util/time_support.h"

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
    enum class ExecContext {
        kInitialFind,
        kGetMoreNoResultsYet,
        kGetMoreWithAtLeastOneResultInBatch,
    };

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
    virtual StatusWith<ClusterQueryResult> next(ExecContext) = 0;

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
     * Returns whether or not all the remote cursors are exhausted.
     */
    virtual bool remotesExhausted() {
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
    RouterExecStage* getChildStage() {
        return _child.get();
    }

    /**
     * Returns a pointer to the current OperationContext, or nullptr if there is no context.
     */
    OperationContext* getOpCtx() {
        return _opCtx;
    }

private:
    OperationContext* _opCtx = nullptr;
    std::unique_ptr<RouterExecStage> _child;
};

}  // namespace mongo
