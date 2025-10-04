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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/baton.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/future.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

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
 *         // safe to destroy the ARS. It will automatically cancel pending I/O.
 *     }
 * }
 *
 * Does not throw exceptions.
 */
class AsyncRequestsSender {
    AsyncRequestsSender(const AsyncRequestsSender&) = delete;
    AsyncRequestsSender& operator=(const AsyncRequestsSender&) = delete;

public:
    /**
     * Defines a request to a remote shard that can be run by the ARS.
     */
    struct Request {
        Request(ShardId shardId, BSONObj cmdObj, std::shared_ptr<Shard> shard = nullptr);

        // ShardId of the shard to which the command will be sent.
        const ShardId shardId;

        // The command object to send to the remote host.
        const BSONObj cmdObj;

        // Optional. The shard registry type to send the request to. Cleared for retries.
        const std::shared_ptr<Shard> shard;
    };

    /**
     * Defines a response for a request to a remote shard.
     */
    struct Response {
        // The shard to which the request was sent.
        ShardId shardId;

        // The response or error from the remote.
        //
        // The mapping between the RemoteCommandResponse returned by the task executor and this
        // field is fairly specific:
        //
        // Status is set when:
        //   * An error is returned when scheduling the task
        //   * A status is returned in the response.status field
        //
        // The value is set when:
        //   * There are no errors
        //   * Errors exist only remotely (I.e. by reading response.data for ok:0 or write errors
        //
        // I.e. if a value is set, swResponse.getValue().status.isOK()
        StatusWith<executor::RemoteCommandResponse> swResponse;

        // The exact host on which the remote command was run. Is unset if the shard could not be
        // found or no shard hosts matching the readPreference could be found.
        boost::optional<HostAndPort> shardHostAndPort;

        /**
         * Returns the effective status of the response sent by the server.
         */
        static Status getEffectiveStatus(const AsyncRequestsSender::Response& response);
    };

    typedef stdx::unordered_map<ShardId, HostAndPort> ShardHostMap;

    /**
     * Constructs a new AsyncRequestsSender. The OperationContext* and TaskExecutor* must remain
     * valid for the lifetime of the ARS.
     *
     * The designatedHostsMap overrides the read preference for the shards specified, and requires
     * those shards target only the host in the map.
     */
    AsyncRequestsSender(OperationContext* opCtx,
                        std::shared_ptr<executor::TaskExecutor> executor,
                        const DatabaseName& dbName,
                        const std::vector<AsyncRequestsSender::Request>& requests,
                        const ReadPreferenceSetting& readPreference,
                        Shard::RetryPolicy retryPolicy,
                        std::unique_ptr<ResourceYielder> resourceYielder,
                        const ShardHostMap& designatedHostsMap);

    /**
     * Returns true if responses for all requests have been returned via next().
     */
    bool done() const noexcept;

    /**
     * Returns the next available response or error.
     *
     * If the operation is interrupted, the status of some responses may be CallbackCanceled.
     *
     * If stopRetrying() has not been called, schedules retries for any remotes that have had a
     * retriable error and have not exhausted their retries.
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
    void stopRetrying() noexcept;

private:
    /**
     * We instantiate one of these per remote host.
     */
    class RemoteData {
    public:
        using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;

        /**
         * Creates a new uninitialized remote state with a command to send.
         */
        RemoteData(AsyncRequestsSender* ars,
                   ShardId shardId,
                   BSONObj cmdObj,
                   HostAndPort designatedHost,
                   std::shared_ptr<Shard> shard = nullptr);

        RemoteData(const RemoteData&) = delete;
        RemoteData& operator=(const RemoteData&) = delete;
        RemoteData(RemoteData&&) = default;
        RemoteData& operator=(RemoteData&&) = default;

        /**
         * Returns a SemiFuture containing a shard object associated with this remote.
         *
         * This will return a SemiFuture with a ShardNotFound error status in case the shard is not
         * found.
         *
         * Additionally this call can trigger a refresh of the ShardRegistry so it could possibly
         * return other network error status related to the refresh.
         */
        SemiFuture<std::shared_ptr<Shard>> getShard();

        /**
         * Returns true if we've already queued a response from the remote.
         */
        explicit operator bool() const {
            return _done;
        }

        /**
         * Extracts a failed response from the remote, given an interruption status.
         */
        Response makeFailedResponse(Status status) && {
            return {std::move(_shardId), std::move(status), std::move(_shardHostAndPort)};
        }

        /**
         * Executes the request for the given shard, this includes any necessary retries and ends
         * with a Response getting written to the response queue.
         *
         * This is implemented by calling scheduleRequest, which handles retries internally in its
         * future chain.
         */
        void executeRequest();

        /**
         * Executes a single attempt to:
         *
         * 1. resolveShardIdToHostAndPort
         * 2. scheduleRemoteCommand
         * 3. handlResponse
         *
         * for the given shard.
         */
        SemiFuture<RemoteCommandCallbackArgs> scheduleRequest();

        /**
         * Given a read preference, selects a lists of hosts on which the command can run.
         */
        SemiFuture<std::vector<HostAndPort>> resolveShardIdToHostAndPorts(
            const ReadPreferenceSetting& readPref);

        /**
         * Schedules the remote command on the ARS's TaskExecutor
         */
        SemiFuture<RemoteCommandCallbackArgs> scheduleRemoteCommand(
            std::span<const HostAndPort> hostAndPort);

        /**
         * Handles the remote response
         */
        SemiFuture<RemoteCommandCallbackArgs> handleResponse(RemoteCommandCallbackArgs rcr);

    private:
        bool _done = false;

        /**
         * Simple structure that ensures the retry strategy is created with an owner.
         */
        struct OwnedRetryStrategy {
            OwnedRetryStrategy(std::shared_ptr<Shard> shard, Shard::RetryPolicy retryPolicy)
                : strategy{*shard, retryPolicy}, owner{std::move(shard)} {}

            Shard::RetryStrategy strategy;
            std::shared_ptr<Shard> owner;
        };

        AsyncRequestsSender* const _ars;

        // ShardId of the shard to which the command will be sent.
        ShardId _shardId;

        // The command object to send to the remote host.
        BSONObj _cmdObj;

        // The designated host and port to send the command to, if provided.  Otherwise is empty().
        HostAndPort _designatedHostAndPort;

        // Optional shard from shard registry for given shard id.
        std::shared_ptr<Shard> _shard;

        // The retry strategy that will be used to evaluate if we should retry.
        boost::optional<OwnedRetryStrategy> _ownedRetryStrategy;

        // The exact host on which the remote command was run. Is unset until a request has been
        // sent.
        boost::optional<HostAndPort> _shardHostAndPort;

        // The number of times we've retried sending the command to this remote.
        // TODO: SERVER-110000: Consider using the retry count from the retry strategy.
        int _retryCount = 0;

        // Record the last writeConcernError received during any retry attempt and return this
        // response if a further retry attempt results in an error signaling a write was not
        // performed.
        boost::optional<RemoteCommandCallbackArgs> _writeConcernErrorRCR;
    };

    OperationContext* _opCtx;

    // The metadata obj to pass along with the command remote. Used to indicate that the command is
    // ok to run on secondaries.
    BSONObj _metadataObj;

    // The database against which the commands are run.
    const DatabaseName _db;

    // The readPreference to use for all requests.
    ReadPreferenceSetting _readPreference;

    // The policy to use when deciding whether to retry on an error.
    Shard::RetryPolicy _retryPolicy;

    // Data tracking the state of our communication with each of the remote nodes.
    std::vector<RemoteData> _remotes;

    // Number of remotes we haven't returned final results from.
    size_t _remotesLeft;

    // Queue of responses.  We don't actually take advantage of the thread safety of the queue, but
    // instead use it to collect results while waiting on a condvar (which allows us to use our
    // underlying baton).
    SingleProducerSingleConsumerQueue<Response> _responseQueue;

    // Used to determine if the ARS should attempt to retry any requests. Is set to true when
    // stopRetrying() is called.
    bool _stopRetrying = false;

    Status _interruptStatus = Status::OK();

    // Set to true if unyielding fails, even after a successful remote response.
    bool _failedUnyield = false;

    // NOTE: it's important that these two members go last in this class.  That ensures that we:
    // 1. cancel/ensure no more callbacks run which touch the ARS
    // 2. cancel any outstanding work in the task executor

    // Scoped task executor which handles clean up of any handles after the ARS goes out of scope
    executor::ScopedTaskExecutor _subExecutor;

    // Scoped baton holder which ensures any callbacks which touch this ARS are called with a
    // not-okay status (or not run, in the case of ExecutorFuture continuations).
    Baton::SubBatonHolder _subBaton;

    // Interface for yielding and unyielding resources while waiting on results from the network.
    // Null if yielding isn't necessary.
    std::unique_ptr<ResourceYielder> _resourceYielder;

    // A cancellation source to stop any requests that are waiting for backoff.
    CancellationSource _cancellationSource;
};

}  // namespace mongo
