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

#include "mongo/executor/task_executor.h"
#include "mongo/s/query/async_results_merger.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/router_exec_stage.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

namespace {
using EventHandle = executor::TaskExecutor::EventHandle;
}  // namespace

/**
 * Draws results from the AsyncResultsMerger, which is the underlying source of the stream of merged
 * documents manipulated by the RouterExecStage pipeline. Used to present a stream of documents
 * merged from the shards to the stages later in the pipeline.
 */
class RouterStageMerge final : public RouterExecStage {
public:
    RouterStageMerge(OperationContext* opCtx,
                     executor::TaskExecutor* executor,
                     ClusterClientCursorParams* params);

    StatusWith<ClusterQueryResult> next(ExecContext) final;

    void kill(OperationContext* opCtx) final;

    bool remotesExhausted() final;

    /**
     * Adds the cursors in 'newShards' to those being merged by the ARM.
     */
    void addNewShardCursors(std::vector<ClusterClientCursorParams::RemoteCursor>&& newShards);

protected:
    Status doSetAwaitDataTimeout(Milliseconds awaitDataTimeout) final;

    void doReattachToOperationContext() override {
        _arm.reattachToOperationContext(getOpCtx());
    }

    virtual void doDetachFromOperationContext() {
        _arm.detachFromOperationContext();
    }

private:
    /**
     * Returns the next document received by the ARM, blocking indefinitely until we either have a
     * new result or exhaust the remote cursors.
     */
    StatusWith<ClusterQueryResult> blockForNextNoTimeout(ExecContext execCtx);

    /**
     * Awaits the next result from the ARM up to a specified time limit. If this is the user's
     * initial find or we have already obtained at least one result for this batch, this method
     * returns EOF immediately rather than blocking.
     */
    StatusWith<ClusterQueryResult> awaitNextWithTimeout(ExecContext execCtx);

    /**
     * Returns the next event to wait upon - either a new event from the ARM, or a valid preceding
     * event which we scheduled during the previous call to next().
     */
    StatusWith<EventHandle> getNextEvent();

    // Not owned here.
    executor::TaskExecutor* _executor;
    EventHandle _leftoverEventFromLastTimeout;

    ClusterClientCursorParams* _params;

    // Schedules remote work and merges results from 'remotes'.
    AsyncResultsMerger _arm;
};

}  // namespace mongo
