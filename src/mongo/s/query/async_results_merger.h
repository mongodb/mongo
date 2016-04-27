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
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class CursorResponse;

/**
 * AsyncResultsMerger is used to generate results from cursor-generating commands on one or more
 * remote hosts. A cursor-generating command (e.g. the find command) is one that establishes a
 * ClientCursor and a matching cursor id on the remote host. In order to retrieve all command
 * results, getMores must be issued against each of the remote cursors until they are exhausted. The
 * results from the remote nodes are merged to present either a single sorted or unsorted stream.
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
     * Constructs a new AsyncResultsMerger. The TaskExecutor* must remain valid for the lifetime of
     * the ARM.
     */
    AsyncResultsMerger(executor::TaskExecutor* executor, ClusterClientCursorParams&& params);

    /**
     * In order to be destroyed, either
     *   --the cursor must have been kill()'ed and the event return from kill() must have been
     *   signaled, or
     *   --all cursors must have been exhausted.
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
     * collection, may return boost::none before end-of-stream. (Tailable cursors remain open even
     * when there are no further results, and may subsequently return more results when they become
     * available.) The calling code is responsible for handling multiple boost::none return values,
     * keeping the cursor open in the tailable case.
     *
     * If there has been an error received from one of the shards, or there is an error in
     * processing results from a shard, then a non-ok status is returned.
     *
     * Invalid to call unless ready() has returned true (i.e., invalid to call if getting the next
     * result requires scheduling remote work).
     */
    StatusWith<boost::optional<BSONObj>> nextReady();

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
     */
    StatusWith<executor::TaskExecutor::EventHandle> nextEvent();

    /**
     * Starts shutting down this ARM. Returns a handle to an event which is signaled when this
     * cursor is safe to destroy.
     *
     * Returns an invalid handle if the underlying task executor is shutting down. In this case, it
     * is legal to destroy the cursor only after the task executor shutdown process is complete.
     *
     * An ARM can only be destroyed if either 1) all its results have been exhausted or 2) the kill
     * event returned by this method has been signaled.
     *
     * May be called multiple times (idempotent).
     */
    executor::TaskExecutor::EventHandle kill();

private:
    /**
     * We instantiate one of these per remote host. It contains the buffer of results we've
     * retrieved from the host but not yet returned, as well as the cursor id, and any error
     * reported from the remote.
     */
    struct RemoteCursorData {
        /**
         * Creates a new uninitialized remote cursor state, which will have to send a command in
         * order to establish its cursor id. Must only be used if the remote cursor ids are not yet
         * known.
         */
        RemoteCursorData(ShardId shardId, BSONObj cmdObj);

        /**
         * Instantiates a new initialized remote cursor, which has an established cursor id. It may
         * only be used for getMore operations.
         */
        RemoteCursorData(HostAndPort hostAndPort, CursorId establishedCursorId);

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
         * Given the shard id with which the cursor was initialized and a read preference, selects
         * a host on which the cursor should be created.
         *
         * May not be called once a cursor has already been established.
         */
        Status resolveShardIdToHostAndPort(const ReadPreferenceSetting& readPref);

        /**
         * Returns the Shard object associated with this remote cursor.
         */
        std::shared_ptr<Shard> getShard();

        // ShardId on which a cursor will be created.
        // TODO: This should always be set.
        const boost::optional<ShardId> shardId;

        // The command object for sending to the remote to establish the cursor. If a remote cursor
        // has not been established yet, this member will be set to a valid command object. If a
        // remote cursor has already been established, this member will be unset.
        boost::optional<BSONObj> initialCmdObj;

        // The cursor id for the remote cursor. If a remote cursor has not been established yet,
        // this member will be unset. If a remote cursor has been established and is not yet
        // exhausted, this member will be set to a valid non-zero cursor id. If a remote cursor was
        // established but is now exhausted, this member will be set to zero.
        boost::optional<CursorId> cursorId;

        std::queue<BSONObj> docBuffer;
        executor::TaskExecutor::CallbackHandle cbHandle;
        Status status = Status::OK();

        // Counts how many times we retried the initial cursor establishment command. It is used to
        // make a decision based on the error type and the retry count about whether we are allowed
        // to retry sending the request to another host from this shard.
        int retryCount = 0;

        // Count of fetched docs during ARM processing of the current batch. Used to reduce the
        // batchSize in getMore when mongod returned less docs than the requested batchSize.
        long long fetchedCount = 0;

    private:
        // For a cursor, which has shard id associated contains the exact host on which the remote
        // cursor resides.
        boost::optional<HostAndPort> _shardHostAndPort;
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
     * Callback run to handle a response from a killCursors command.
     */
    static void handleKillCursorsResponse(
        const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData);

    /**
     * Parses the find or getMore command response object to a CursorResponse.
     *
     * Returns a non-OK response if the response fails to parse or if there is a cursor id mismatch.
     */
    static StatusWith<CursorResponse> parseCursorResponse(const BSONObj& responseObj,
                                                          const RemoteCursorData& remote);

    /**
     * Helper to schedule a command asking the remote node for another batch of results.
     *
     * The 'remoteIndex' gives the position of the remote node from which we are retrieving the
     * batch in '_remotes'.
     *
     * Returns success if the command to retrieve the next batch was scheduled successfully.
     */
    Status askForNextBatch_inlock(size_t remoteIndex);

    /**
     * Checks whether or not the remote cursors are all exhausted.
     */
    bool remotesExhausted_inlock();

    //
    // Helpers for ready().
    //

    bool ready_inlock();
    bool readySorted_inlock();
    bool readyUnsorted_inlock();

    //
    // Helpers for nextReady().
    //

    boost::optional<BSONObj> nextReadySorted();
    boost::optional<BSONObj> nextReadyUnsorted();

    /**
     * When nextEvent() schedules remote work, it passes this method as a callback. The TaskExecutor
     * will call this function, passing the response from the remote.
     *
     * 'remoteIndex' is the position of the relevant remote node in '_remotes', and therefore
     * indicates which node the response came from and where the new result documents should be
     * buffered.
     */
    void handleBatchResponse(const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData,
                             size_t remoteIndex);

    /**
     * If there is a valid unsignaled event that has been requested via nextReady() and there are
     * buffered results that are ready to return, signals that event.
     *
     * Invalidates the current event, as we must signal the event exactly once and we only keep a
     * handle to a valid event if it is unsignaled.
     */
    void signalCurrentEventIfReady_inlock();

    /**
     * Returns true if this async cursor is waiting to receive another batch from a remote.
     */
    bool haveOutstandingBatchRequests_inlock();

    /**
     * Schedules a killCursors command to be run on all remote hosts that have open cursors.
     */
    void scheduleKillCursors_inlock();

    // Not owned here.
    executor::TaskExecutor* _executor;

    ClusterClientCursorParams _params;

    // The metadata obj to pass along with the command request. Used to indicate that the command is
    // ok to run on secondaries.
    BSONObj _metadataObj;

    // Must be acquired before accessing any data members (other than _params, which is read-only).
    // Must also be held when calling any of the '_inlock()' helper functions.
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
