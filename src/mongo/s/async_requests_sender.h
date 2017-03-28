/**
 *    Copyright (C) 2017 MongoDB Inc.
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
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/shard_id.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * The AsyncRequestsSender allows for sending requests to a set of remote shards in parallel.
 * Work on remote nodes is accomplished by scheduling remote work in a TaskExecutor's event loop.
 *
 * Typical usage is:
 *
 * // Add some requests
 * std::vector<AsyncRequestSender::Request> requests;
 *
 * // Creating the ARS schedules the requests immediately
 * AsyncRequestsSender ars(opCtx, executor, db, requests, readPrefSetting);
 *
 * while (!ars.done()) {
 *     // Schedule a round of retries if needed and wait for next response or error.
 *     auto response = ars.next();
 *
 *     if (!response.swResponse.isOK()) {
 *         // If partial results are tolerable, process the error as needed and continue.
 *         continue;
 *
 *         // If partial results are not tolerable but you need to retrieve responses for all
 *         // dispatched requests, use stopRetrying() and continue.
 *         ars.stopRetrying();
 *         continue;
 *
 *         // If partial results are not tolerable and you don't care about dispatched requests,
 *         // safe to destroy the ARS. It will automatically cancel pending I/O and wait for the
 *         // outstanding callbacks to complete on destruction.
 *     }
 * }
 *
 * Does not throw exceptions.
 */
class AsyncRequestsSender {
    MONGO_DISALLOW_COPYING(AsyncRequestsSender);

public:
    /**
     * Defines a request to a remote shard that can be run by the ARS.
     */
    struct Request {
        Request(ShardId shardId, BSONObj cmdObj);

        // ShardId of the shard to which the command will be sent.
        const ShardId shardId;

        // The command object to send to the remote host.
        const BSONObj cmdObj;
    };

    /**
     * Defines a response for a request to a remote shard.
     */
    struct Response {
        // Constructor for a response that was successfully received.
        Response(ShardId shardId, executor::RemoteCommandResponse response, HostAndPort hp);

        // Constructor that specifies the reason the response was not successfully received.
        Response(ShardId shardId, Status status, boost::optional<HostAndPort> hp);

        // The shard to which the request was sent.
        ShardId shardId;

        // The response or error from the remote.
        StatusWith<executor::RemoteCommandResponse> swResponse;

        // The exact host on which the remote command was run. Is unset if the shard could not be
        // found or no shard hosts matching the readPreference could be found.
        boost::optional<HostAndPort> shardHostAndPort;
    };

    /**
     * Constructs a new AsyncRequestsSender. The OperationContext* and TaskExecutor* must remain
     * valid for the lifetime of the ARS.
     */
    AsyncRequestsSender(OperationContext* opCtx,
                        executor::TaskExecutor* executor,
                        StringData db,
                        const std::vector<AsyncRequestsSender::Request>& requests,
                        const ReadPreferenceSetting& readPreference);

    /**
     * Ensures pending network I/O for any outstanding requests has been canceled and waits for
     * outstanding callbacks to complete.
     */
    ~AsyncRequestsSender();

    /**
     * Returns true if responses for all requests have been returned via next().
     */
    bool done();

    /**
     * Returns the next available response or error.
     *
     * If the operation is interrupted, the status of some responses may be CallbackCanceled.
     *
     * If neither cancelPendingRequests() nor stopRetrying() have been called, schedules retries for
     * any remotes that have had a retriable error and have not exhausted their retries.
     *
     * Note: Must only be called from one thread at a time, and invalid to call if done() is true.
     */
    Response next();

    /**
     * Stops the ARS from retrying requests.
     *
     * Use this if you no longer care about getting success responses, but need to do cleanup based
     * on responses for requests that have already been dispatched.
     */
    void stopRetrying();

private:
    /**
     * We instantiate one of these per remote host.
     */
    struct RemoteData {
        /**
         * Creates a new uninitialized remote state with a command to send.
         */
        RemoteData(ShardId shardId, BSONObj cmdObj);

        /**
         * Given a read preference, selects a host on which the command should be run.
         */
        Status resolveShardIdToHostAndPort(const ReadPreferenceSetting& readPref);

        /**
         * Returns the Shard object associated with this remote.
         */
        std::shared_ptr<Shard> getShard();

        // ShardId of the shard to which the command will be sent.
        ShardId shardId;

        // The command object to send to the remote host.
        BSONObj cmdObj;

        // The response or error from the remote. Is unset until a response or error has been
        // received.
        boost::optional<StatusWith<executor::RemoteCommandResponse>> swResponse;

        // The exact host on which the remote command was run. Is unset until a request has been
        // sent.
        boost::optional<HostAndPort> shardHostAndPort;

        // The number of times we've retried sending the command to this remote.
        int retryCount = 0;

        // The callback handle to an outstanding request for this remote.
        executor::TaskExecutor::CallbackHandle cbHandle;

        // Whether this remote's result has been returned.
        bool done = false;
    };

    /**
     * Cancels all outstanding requests on the TaskExecutor and sets the _stopRetrying flag.
     */
    void _cancelPendingRequests();

    /**
     * Replaces _notification with a new notification.
     *
     * If _stopRetrying is false, schedules retries for remotes that have had a retriable error.
     *
     * If any remote has successfully received a response, returns a Response for it.
     * If any remote has an error response that can't be retried, returns a Response for it.
     * Otherwise, returns boost::none.
     */
    boost::optional<Response> _ready();

    /**
     * For each remote that had a response, checks if it had a retriable error, and clears its
     * response if so.
     *
     * For each remote without a response or pending request, schedules the remote request.
     *
     * On failure to schedule a request, signals the notification.
     */
    void _scheduleRequests_inlock();

    /**
     * Helper to schedule a command to a remote.
     *
     * The 'remoteIndex' gives the position of the remote node from which we are retrieving the
     * batch in '_remotes'.
     *
     * Returns success if the command was scheduled successfully.
     */
    Status _scheduleRequest_inlock(size_t remoteIndex);

    /**
     * The callback for a remote command.
     *
     * 'remoteIndex' is the position of the relevant remote node in '_remotes', and therefore
     * indicates which node the response came from and where the response should be buffered.
     *
     * Stores the response or error in the remote and signals the notification.
     */
    void _handleResponse(const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData,
                         size_t remoteIndex);

    // Not owned here.
    OperationContext* _opCtx;

    // Not owned here.
    executor::TaskExecutor* _executor;

    // The metadata obj to pass along with the command remote. Used to indicate that the command is
    // ok to run on secondaries.
    BSONObj _metadataObj;

    // The database against which the commands are run.
    const StringData _db;

    // The readPreference to use for all requests.
    ReadPreferenceSetting _readPreference;

    // Used to determine whether to check for interrupt when waiting for a remote to be ready.
    // Set to false if we are interrupted, so that we can still wait for callbacks to complete.
    // This is only accessed by the thread in next().
    bool _checkForInterrupt = true;

    // Must be acquired before accessing the below data members.
    // Must also be held when calling any of the '_inlock()' helper functions.
    stdx::mutex _mutex;

    // Data tracking the state of our communication with each of the remote nodes.
    std::vector<RemoteData> _remotes;

    // A notification that gets signaled when a remote is ready for processing (i.e., we failed to
    // schedule a request to it or received a response from it).
    boost::optional<Notification<void>> _notification;

    // Used to determine if the ARS should attempt to retry any requests. Is set to true when
    // stopRetrying() or cancelPendingRequests() is called.
    bool _stopRetrying = false;
};

}  // namespace mongo
