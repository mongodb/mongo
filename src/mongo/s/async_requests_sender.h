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
 * The AsyncRequestsSender allows for sending requests to a set of remote shards in parallel and
 * automatically retrying on retriable errors according to a RetryPolicy. It can also allow for
 * retrieving partial results by ignoring shards that return errors.
 *
 * Work on remote nodes is accomplished by scheduling remote work in a TaskExecutor's event loop.
 *
 * Typical usage is:
 *
 * AsyncRequestsSender ars(txn, executor, db, requests, readPrefSetting);  // schedule the requests
 * auto responses = ars.waitForResponses(txn);  // wait for responses; retries on retriable erors
 *
 * Additionally, you can interrupt() (if you want waitForResponses() to wait for responses for
 * outstanding requests but stop scheduling retries) or kill() (if you want to cancel outstanding
 * requests) the ARS from another thread.
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
        Response(executor::RemoteCommandResponse response, HostAndPort hp);

        // Constructor that specifies the reason the response was not successfully received.
        Response(Status status);

        // The response or error from the remote.
        StatusWith<executor::RemoteCommandResponse> swResponse;

        // The exact host on which the remote command was run. Is unset if swResponse has a non-OK
        // status.
        boost::optional<HostAndPort> shardHostAndPort;
    };

    /**
     * Constructs a new AsyncRequestsSender. The TaskExecutor* must remain valid for the lifetime of
     * the ARS.
     */
    AsyncRequestsSender(OperationContext* txn,
                        executor::TaskExecutor* executor,
                        std::string db,
                        const std::vector<AsyncRequestsSender::Request>& requests,
                        const ReadPreferenceSetting& readPreference,
                        bool allowPartialResults = false);

    ~AsyncRequestsSender();

    /**
     * Returns a vector containing the responses or errors for each remote in the same order as the
     * input vector that was passed in the constructor.
     *
     * If we were killed, returns immediately.
     * If we were interrupted, returns when any outstanding requests have completed.
     * Otherwise, returns when each remote has received a response or error.
     */
    std::vector<Response> waitForResponses(OperationContext* txn);

    /**
     * Stops the ARS from retrying requests. Causes waitForResponses() to wait until any outstanding
     * requests have received a response or error.
     *
     * Use this if you no longer care about getting success responses, but need to do cleanup based
     * on responses for requests that have already been dispatched.
     */
    void interrupt();

    /**
     * Cancels all outstanding requests and makes waitForResponses() return immediately.
     *
     * Use this if you no longer care about getting success responses, and don't need to process
     * responses for outstanding requests.
     */
    void kill();

private:
    /**
     * Returns true if each remote has received a response or error. (If kill() has been called,
     * the error is the error assigned by the TaskExecutor when a callback is canceled).
     */
    bool _done();

    /**
     * Executes the logic of _done().
     */
    bool _done_inlock();

    /**
     * Replaces _notification with a new notification.
     *
     * If _stopRetrying is false, for each remote that does not have a response or outstanding
     * request, schedules work to send the command to the remote.
     *
     * Invalid to call if there is an existing Notification and it has not yet been signaled.
     */
    void _scheduleRequestsIfNeeded(OperationContext* txn);

    /**
     * Helper to schedule a command to a remote.
     *
     * The 'remoteIndex' gives the position of the remote node from which we are retrieving the
     * batch in '_remotes'.
     *
     * Returns success if the command to retrieve the next batch was scheduled successfully.
     */
    Status _scheduleRequest_inlock(OperationContext* txn, size_t remoteIndex);

    /**
     * The callback for a remote command.
     *
     * 'remoteIndex' is the position of the relevant remote node in '_remotes', and therefore
     * indicates which node the response came from and where the response should be buffered.
     *
     * On a retriable error, unless _stopRetrying is true, signals the notification so that the
     * request can be immediately retried.
     *
     * On a non-retriable error, if allowPartialResults is false, sets _stopRetrying to true.
     */
    void _handleResponse(const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData,
                         OperationContext* txn,
                         size_t remoteIndex);

    /**
     * If the existing notification has not yet been signaled, signals it and marks it as signaled.
     */
    void _signalCurrentNotification_inlock();

    /**
     * Wrapper around signalCurrentNotification_inlock(); only signals the notification if _done()
     * is true.
     */
    void _signalCurrentNotificationIfDone_inlock();

    /**
     * We instantiate one of these per remote host.
     */
    struct RemoteData {
        /**
         * Creates a new uninitialized remote state with a command to send.
         */
        RemoteData(ShardId shardId, BSONObj cmdObj);

        /**
         * Returns the resolved host and port on which the remote command was or will be run.
         */
        const HostAndPort& getTargetHost() const;

        /**
         * Given a read preference, selects a host on which the command should be run.
         */
        Status resolveShardIdToHostAndPort(const ReadPreferenceSetting& readPref);

        /**
         * Returns the Shard object associated with this remote.
         */
        std::shared_ptr<Shard> getShard();

        // ShardId of the shard to which the command will be sent.
        const ShardId shardId;

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
    };

    /**
     * Used internally to determine if the ARS should attempt to retry any requests. Is set to true
     * when:
     * - interrupt() or kill() is called
     * - allowPartialResults is false and some remote has a non-retriable error (or exhausts its
     *  retries for a retriable error).
     */
    bool _stopRetrying = false;

    // Not owned here.
    executor::TaskExecutor* _executor;

    // The metadata obj to pass along with the command remote. Used to indicate that the command is
    // ok to run on secondaries.
    BSONObj _metadataObj;

    // The database against which the commands are run.
    const std::string _db;

    // The readPreference to use for all requests.
    ReadPreferenceSetting _readPreference;

    // If set to true, allows for skipping over hosts that have non-retriable errors or exhaust
    // their retries.
    bool _allowPartialResults = false;

    // Must be acquired before accessing any data members.
    // Must also be held when calling any of the '_inlock()' helper functions.
    stdx::mutex _mutex;

    // Data tracking the state of our communication with each of the remote nodes.
    std::vector<RemoteData> _remotes;

    // A notification that gets signaled when a remote has a retriable error or the last outstanding
    // response is received.
    boost::optional<Notification<void>> _notification;
};

}  // namespace mongo
