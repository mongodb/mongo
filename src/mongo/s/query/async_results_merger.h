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
#include <queue>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/cursor_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/cluster_query_result.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class CursorResponse;

/**
 * Given a set of cursorIds across one or more shards, the AsyncResultsMerger calls getMore on the
 * cursors to present a single sorted or unsorted stream of documents.
 *
 * (A cursor-generating command (e.g. the find command) is one that establishes a ClientCursor and a
 * matching cursorId on the remote host. In order to retrieve all document results, getMores must be
 * issued against each of the remote cursors until they are exhausted).
 *
 * The ARM offers a non-blocking interface: if no results are immediately available on this host for
 * retrieval, calling nextEvent() schedules work on the remote hosts in order to generate further
 * results. The event is signaled when further results are available.
 *
 * Work on remote nodes is accomplished by scheduling remote work in TaskExecutor's event loop.
 *
 * Task-scheduling behavior differs depending on whether there is a sort. If the result documents
 * must be sorted, we pass the sort through to the remote nodes and then merge the sorted streams.
 * This requires waiting until we have a response from every remote before returning results.
 * Without a sort, we are ready to return results as soon as we have *any* response from a remote.
 *
 * On any error, the caller is responsible for shutting down the ARM using the kill() method.
 *
 * Does not throw exceptions.
 */
class AsyncResultsMerger {
    MONGO_DISALLOW_COPYING(AsyncResultsMerger);

public:
    /**
     * Takes ownership of the cursors from ClusterClientCursorParams by storing their cursorIds and
     * the hosts on which they exist in _remotes.
     *
     * Additionally copies each remote's first batch of results, if one exists, into that remote's
     * docBuffer. If a sort is specified in the ClusterClientCursorParams, places the remotes with
     * buffered results onto _mergeQueue.
     *
     * The TaskExecutor* must remain valid for the lifetime of the ARM.
     *
     * If 'opCtx' may be deleted before this AsyncResultsMerger, the caller must call
     * detachFromOperationContext() before deleting 'opCtx', and call reattachToOperationContext()
     * with a new, valid OperationContext before the next use.
     */
    AsyncResultsMerger(OperationContext* opCtx,
                       executor::TaskExecutor* executor,
                       ClusterClientCursorParams* params);

    /**
     * In order to be destroyed, either the ARM must have been kill()'ed or all cursors must have
     * been exhausted. This is so that any unexhausted cursors are cleaned up by the ARM.
     */
    virtual ~AsyncResultsMerger();

    /**
     * Returns true if all of the remote cursors are exhausted.
     */
    bool remotesExhausted();

    /**
     * Sets the maxTimeMS value that the ARM should forward with any internally issued getMore
     * requests.
     *
     * Returns a non-OK status if this cursor type does not support maxTimeMS on getMore (i.e. if
     * the cursor is not tailable + awaitData).
     */
    Status setAwaitDataTimeout(Milliseconds awaitDataTimeout);

    /**
     * Signals to the AsyncResultsMerger that the caller is finished using it in the current
     * context.
     */
    void detachFromOperationContext();

    /**
     * Provides a new OperationContext to be used by the AsyncResultsMerger - the caller must call
     * detachFromOperationContext() before 'opCtx' is deleted.
     */
    void reattachToOperationContext(OperationContext* opCtx);

    /**
     * Returns true if there is no need to schedule remote work in order to take the next action.
     * This means that either
     *   --there is a buffered result which we can return,
     *   --or all of the remote cursors have been closed and we are done,
     *   --or an error was received and the next call to nextReady() will return an error status,
     *   --or the ARM has been killed and is in the process of shutting down. In this case,
     *   nextReady() will report an error when called.
     *
     * A return value of true indicates that it is safe to call nextReady().
     */
    bool ready();

    /**
     * If there is a result available that has already been retrieved from a remote node and
     * buffered, then return it along with an ok status.
     *
     * If we have reached the end of the stream of results, returns boost::none along with an ok
     * status.
     *
     * If this AsyncResultsMerger is fetching results from a remote cursor tailing a capped
     * collection, may return an empty ClusterQueryResult before end-of-stream. (Tailable cursors
     * remain open even when there are no further results, and may subsequently return more results
     * when they become available.) The calling code is responsible for handling multiple empty,
     * ClusterQueryResult return values, keeping the cursor open in the tailable case.
     *
     * If there has been an error received from one of the shards, or there is an error in
     * processing results from a shard, then a non-ok status is returned.
     *
     * Invalid to call unless ready() has returned true (i.e., invalid to call if getting the next
     * result requires scheduling remote work).
     */
    StatusWith<ClusterQueryResult> nextReady();

    /**
     * Schedules remote work as required in order to make further results available. If there is an
     * error in scheduling this work, returns a non-ok status. On success, returns an event handle.
     * The caller can pass this event handle to 'executor' in order to be blocked until further
     * results are available.
     *
     * Invalid to call unless ready() has returned false (i.e. invalid to call if the next result is
     * available without scheduling remote work).
     *
     * Also invalid to call if there is an outstanding event, created by a previous call to this
     * function, that has not yet been signaled. If there is an outstanding unsignaled event,
     * returns an error.
     *
     * If there is a sort, the event is signaled when there are buffered results for all
     * non-exhausted remotes.
     * If there is no sort, the event is signaled when some remote has a buffered result.
     */
    StatusWith<executor::TaskExecutor::EventHandle> nextEvent();

    /**
     * Schedules a getMore on any remote hosts which:
     *  - Do not have an error status set already.
     *  - Don't already have a request outstanding.
     *  - We don't currently have any results buffered.
     *  - Are not exhausted (have a non-zero cursor id).
     * Returns an error if any of the remotes responded with an error, or if we encounter an error
     * while scheduling the getMore requests..
     *
     * In most cases users should call nextEvent() instead of this method, but this can be necessary
     * if the caller of nextEvent() calls detachFromOperationContext() before the event is signaled.
     * In such cases, the ARM cannot schedule getMores itself, and will need to be manually prompted
     * after calling reattachToOperationContext().
     *
     * It is illegal to call this method if the ARM is not attached to an OperationContext.
     */
    Status scheduleGetMores();

    /**
     * Adds the specified shard cursors to the set of cursors to be merged.  The results from the
     * new cursors will be returned as normal through nextReady().
     */
    void addNewShardCursors(const std::vector<ClusterClientCursorParams::RemoteCursor>& newCursors);

    /**
     * Starts shutting down this ARM by canceling all pending requests. Returns a handle to an event
     * that is signaled when this ARM is safe to destroy.
     * If there are no pending requests, schedules killCursors and signals the event immediately.
     * Otherwise, the last callback that runs after kill() is called schedules killCursors and
     * signals the event.
     *
     * Returns an invalid handle if the underlying task executor is shutting down. In this case,
     * killing is considered complete and the ARM may be destroyed immediately.
     *
     * May be called multiple times (idempotent).
     *
     * Note that 'opCtx' may or may not be the same as the operation context to which this cursor is
     * currently attached. This is so that a killing thread may call this method with its own
     * operation context.
     */
    executor::TaskExecutor::EventHandle kill(OperationContext* opCtx);

private:
    /**
     * We instantiate one of these per remote host. It contains the buffer of results we've
     * retrieved from the host but not yet returned, as well as the cursor id, and any error
     * reported from the remote.
     */
    struct RemoteCursorData {
        RemoteCursorData(HostAndPort hostAndPort,
                         NamespaceString cursorNss,
                         CursorId establishedCursorId);

        /**
         * Returns the resolved host and port on which the remote cursor resides.
         */
        const HostAndPort& getTargetHost() const;

        /**
         * Returns whether there is another buffered result available for this remote node.
         */
        bool hasNext() const;

        /**
         * Returns whether the remote has given us all of its results (i.e. whether it has closed
         * its cursor).
         */
        bool exhausted() const;

        /**
         * Returns the Shard object associated with this remote cursor.
         */
        std::shared_ptr<Shard> getShard();

        // Used when merging tailable awaitData cursors in sorted order. In order to return any
        // result to the client we have to know that no shard will ever return anything that sorts
        // before it. This object represents a promise from the remote that it will never return a
        // result with a sort key lower than this.
        boost::optional<BSONObj> promisedMinSortKey;

        // The cursor id for the remote cursor. If a remote cursor is not yet exhausted, this member
        // will be set to a valid non-zero cursor id. If a remote cursor is now exhausted, this
        // member will be set to zero.
        CursorId cursorId;

        // The namespace this cursor belongs to - note this may be different than the namespace of
        // the operation if there is a view.
        NamespaceString cursorNss;

        // The exact host in the shard on which the cursor resides.
        HostAndPort shardHostAndPort;

        // The buffer of results that have been retrieved but not yet returned to the caller.
        std::queue<ClusterQueryResult> docBuffer;

        // Is valid if there is currently a pending request to this remote.
        executor::TaskExecutor::CallbackHandle cbHandle;

        // Set to an error status if there is an error retrieving a response from this remote or if
        // the command result contained an error.
        Status status = Status::OK();

        // Count of fetched docs during ARM processing of the current batch. Used to reduce the
        // batchSize in getMore when mongod returned less docs than the requested batchSize.
        long long fetchedCount = 0;
    };

    class MergingComparator {
    public:
        MergingComparator(const std::vector<RemoteCursorData>& remotes, const BSONObj& sort)
            : _remotes(remotes), _sort(sort) {}

        bool operator()(const size_t& lhs, const size_t& rhs);

    private:
        const std::vector<RemoteCursorData>& _remotes;

        const BSONObj& _sort;
    };

    enum LifecycleState { kAlive, kKillStarted, kKillComplete };

    /**
     * Parses the find or getMore command response object to a CursorResponse.
     *
     * Returns a non-OK response if the response fails to parse or if there is a cursor id mismatch.
     */
    static StatusWith<CursorResponse> _parseCursorResponse(const BSONObj& responseObj,
                                                           const RemoteCursorData& remote);

    /**
     * Helper to schedule a command asking the remote node for another batch of results.
     *
     * The 'remoteIndex' gives the position of the remote node from which we are retrieving the
     * batch in '_remotes'.
     *
     * Returns success if the command to retrieve the next batch was scheduled successfully.
     */
    Status _askForNextBatch(WithLock, size_t remoteIndex);

    /**
     * Checks whether or not the remote cursors are all exhausted.
     */
    bool _remotesExhausted(WithLock);

    //
    // Helpers for ready().
    //

    bool _ready(WithLock);
    bool _readySorted(WithLock);
    bool _readySortedTailable(WithLock);
    bool _readyUnsorted(WithLock);

    //
    // Helpers for nextReady().
    //

    ClusterQueryResult _nextReadySorted(WithLock);
    ClusterQueryResult _nextReadyUnsorted(WithLock);

    using CbData = executor::TaskExecutor::RemoteCommandCallbackArgs;
    using CbResponse = executor::TaskExecutor::ResponseStatus;

    /**
     * When nextEvent() schedules remote work, the callback uses this function to process results.
     *
     * 'remoteIndex' is the position of the relevant remote node in '_remotes', and therefore
     * indicates which node the response came from and where the new result documents should be
     * buffered.
     */
    void _handleBatchResponse(WithLock, CbData const&, size_t remoteIndex);

    /**
     * Cleans up if the remote cursor was killed while waiting for a response.
     */
    void _cleanUpKilledBatch(WithLock);

    /**
     * Cleans up after remote query failure.
     */
    void _cleanUpFailedBatch(WithLock lk, Status status, size_t remoteIndex);

    /**
     * Processes results from a remote query.
     */
    void _processBatchResults(WithLock, CbResponse const&, size_t remoteIndex);

    /**
     * Adds the batch of results to the RemoteCursorData. Returns false if there was an error
     * parsing the batch.
     */
    bool _addBatchToBuffer(WithLock, size_t remoteIndex, const CursorResponse& response);

    /**
     * If there is a valid unsignaled event that has been requested via nextEvent() and there are
     * buffered results that are ready to return, signals that event.
     *
     * Invalidates the current event, as we must signal the event exactly once and we only keep a
     * handle to a valid event if it is unsignaled.
     */
    void _signalCurrentEventIfReady(WithLock);

    /**
     * Returns true if this async cursor is waiting to receive another batch from a remote.
     */
    bool _haveOutstandingBatchRequests(WithLock);


    /**
     * Schedules a getMore on any remote hosts which we need another batch from.
     */
    Status _scheduleGetMores(WithLock);

    /**
     * Schedules a killCursors command to be run on all remote hosts that have open cursors.
     */
    void _scheduleKillCursors(WithLock, OperationContext* opCtx);

    /**
     * Updates 'remote's metadata (e.g. the cursor id) based on information in 'response'.
     */
    void updateRemoteMetadata(RemoteCursorData* remote, const CursorResponse& response);

    OperationContext* _opCtx;
    executor::TaskExecutor* _executor;
    ClusterClientCursorParams* _params;

    // The metadata obj to pass along with the command request. Used to indicate that the command is
    // ok to run on secondaries.
    BSONObj _metadataObj;

    // Must be acquired before accessing any data members (other than _params, which is read-only).
    stdx::mutex _mutex;

    // Data tracking the state of our communication with each of the remote nodes.
    std::vector<RemoteCursorData> _remotes;

    // The top of this priority queue is the index into '_remotes' for the remote host that has the
    // next document to return, according to the sort order. Used only if there is a sort.
    std::priority_queue<size_t, std::vector<size_t>, MergingComparator> _mergeQueue;

    // The index into '_remotes' for the remote from which we are currently retrieving results.
    // Used only if there is *not* a sort.
    size_t _gettingFromRemote = 0;

    Status _status = Status::OK();

    executor::TaskExecutor::EventHandle _currentEvent;

    // For tailable cursors, set to true if the next result returned from nextReady() should be
    // boost::none.
    bool _eofNext = false;

    boost::optional<Milliseconds> _awaitDataTimeout;

    //
    // Killing
    //

    LifecycleState _lifecycleState = kAlive;

    // Signaled when all outstanding batch request callbacks have run, and all killCursors commands
    // have been scheduled. This means that the ARM is safe to delete.
    executor::TaskExecutor::EventHandle _killCursorsScheduledEvent;
};

}  // namespace mongo
