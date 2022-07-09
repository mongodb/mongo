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

#include <boost/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {

class OperationContext;
class RemoteCommandTargeter;

/**
 * Presents an interface for talking to shards, regardless of whether that shard is remote or is
 * the current (local) shard.
 */
class Shard {
public:
    struct CommandResponse {
        CommandResponse(boost::optional<HostAndPort> hostAndPort,
                        BSONObj response,
                        Status commandStatus,
                        Status writeConcernStatus)
            : hostAndPort(std::move(hostAndPort)),
              response(std::move(response)),
              commandStatus(std::move(commandStatus)),
              writeConcernStatus(std::move(writeConcernStatus)) {}

        /**
         * Takes the response from running a batch write command and writes the appropriate response
         * into batchResponse, while also returning the Status of the operation.
         */
        static Status processBatchWriteResponse(StatusWith<CommandResponse> response,
                                                BatchedCommandResponse* batchResponse);

        /**
         * Returns an error status if either commandStatus or writeConcernStatus has an error.
         */
        static Status getEffectiveStatus(const StatusWith<CommandResponse>& swResponse);

        boost::optional<HostAndPort> hostAndPort;
        BSONObj response;
        Status commandStatus;
        Status writeConcernStatus;
    };

    struct QueryResponse {
        std::vector<BSONObj> docs;
        repl::OpTime opTime;
    };

    enum class RetryPolicy {
        kIdempotent,
        kIdempotentOrCursorInvalidated,
        kNotIdempotent,
        kNoRetry,
    };

    virtual ~Shard() = default;

    const ShardId& getId() const {
        return _id;
    }

    /**
     * Returns true if this shard object represents the config server.
     */
    bool isConfig() const;

    /**
     * Returns the current connection string for the shard.
     */
    virtual ConnectionString getConnString() const = 0;

    /**
     * Returns the RemoteCommandTargeter for the hosts in this shard.
     *
     * This is only valid to call on ShardRemote instances.
     */
    virtual std::shared_ptr<RemoteCommandTargeter> getTargeter() const = 0;

    /**
     * Notifies the RemoteCommandTargeter owned by the shard of a particular mode of failure for
     * the specified host.
     *
     * This is only valid to call on ShardRemote instances.
     */
    virtual void updateReplSetMonitor(const HostAndPort& remoteHost,
                                      const Status& remoteCommandStatus) = 0;

    /**
     * Returns a string description of this shard entry.
     */
    virtual std::string toString() const = 0;

    /**
     * Returns whether a server operation which failed with the given error code should be retried
     * (i.e. is safe to retry and has the potential to succeed next time).  The 'options' argument
     * describes whether the operation that generated the given code was idempotent, which affects
     * which codes are safe to retry on.
     */
    virtual bool isRetriableError(ErrorCodes::Error code, RetryPolicy options) = 0;

    /**
     * Runs the specified command returns the BSON command response plus parsed out Status of this
     * response and write concern error (if present). Retries failed operations according to the
     * given "retryPolicy".  Retries indefinitely until/unless a non-retriable error is encountered,
     * the maxTimeMs on the OperationContext expires, or the operation is interrupted.
     */
    StatusWith<CommandResponse> runCommand(OperationContext* opCtx,
                                           const ReadPreferenceSetting& readPref,
                                           const std::string& dbName,
                                           const BSONObj& cmdObj,
                                           RetryPolicy retryPolicy);

    /**
     * Same as the other variant of runCommand, but allows the operation timeout to be overriden.
     * Runs for the lesser of the remaining time on the operation context or the specified maxTimeMS
     * override.
     */
    StatusWith<CommandResponse> runCommand(OperationContext* opCtx,
                                           const ReadPreferenceSetting& readPref,
                                           const std::string& dbName,
                                           const BSONObj& cmdObj,
                                           Milliseconds maxTimeMSOverride,
                                           RetryPolicy retryPolicy);

    /**
     * Same as runCommand, but will only retry failed operations up to 3 times, regardless of
     * the retryPolicy or the remaining maxTimeMs.
     * Wherever possible this method should be avoided in favor of runCommand.
     */
    StatusWith<CommandResponse> runCommandWithFixedRetryAttempts(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const std::string& dbName,
        const BSONObj& cmdObj,
        RetryPolicy retryPolicy);

    /**
     * Same as runCommand, but will only retry failed operations up to 3 times, regardless of
     * the retryPolicy or the remaining maxTimeMs.
     * Wherever possible this method should be avoided in favor of runCommand.
     */
    StatusWith<CommandResponse> runCommandWithFixedRetryAttempts(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const std::string& dbName,
        const BSONObj& cmdObj,
        Milliseconds maxTimeMSOverride,
        RetryPolicy retryPolicy);

    /**
     * Schedules the command to be sent to the shard asynchronously. Does not provide any guarantee
     * on whether the command is actually sent or even scheduled successfully.
     */
    virtual void runFireAndForgetCommand(OperationContext* opCtx,
                                         const ReadPreferenceSetting& readPref,
                                         const std::string& dbName,
                                         const BSONObj& cmdObj) = 0;

    /**
     * Runs a cursor command, exhausts the cursor, and pulls all data into memory. Performs retries
     * if the command fails in accordance with the kIdempotent RetryPolicy.
     */
    StatusWith<QueryResponse> runExhaustiveCursorCommand(OperationContext* opCtx,
                                                         const ReadPreferenceSetting& readPref,
                                                         const std::string& dbName,
                                                         const BSONObj& cmdObj,
                                                         Milliseconds maxTimeMSOverride);

    /**
     * Synchronously run the aggregation request, with a best effort honoring of request
     * options. `callback` will be called with the batch and resume token contained in each
     * response. `callback` should return `true` to execute another getmore. Returning `false` will
     * send a `killCursors`. If the aggregation results are exhausted, there will be no additional
     * calls to `callback`.
     */
    virtual Status runAggregation(
        OperationContext* opCtx,
        const AggregateCommandRequest& aggRequest,
        std::function<bool(const std::vector<BSONObj>& batch,
                           const boost::optional<BSONObj>& postBatchResumeToken)> callback) = 0;

    /**
     * Runs a write command against a shard. This is separate from runCommand, because write
     * commands return errors in a different format than regular commands do, so checking for
     * retriable errors must be done differently.
     */
    BatchedCommandResponse runBatchWriteCommand(OperationContext* opCtx,
                                                Milliseconds maxTimeMS,
                                                const BatchedCommandRequest& batchRequest,
                                                RetryPolicy retryPolicy);

    /**
     * Warning: This method exhausts the cursor and pulls all data into memory.
     * Do not use other than for very small (i.e., admin or metadata) collections.
     * Performs retries if the query fails in accordance with the kIdempotent RetryPolicy.
     *
     * ShardRemote instances expect "readConcernLevel" to always be kMajorityReadConcern, whereas
     * ShardLocal instances expect either kLocalReadConcern or kMajorityReadConcern.
     */
    StatusWith<QueryResponse> exhaustiveFindOnConfig(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const repl::ReadConcernLevel& readConcernLevel,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit,
        const boost::optional<BSONObj>& hint = boost::none);

    // This timeout will be used by default in operations against the config server, unless
    // explicitly overridden
    static const Milliseconds kDefaultConfigCommandTimeout;

    /**
     * Returns false if the error is a retriable error and/or causes a replset monitor update. These
     * errors, if from a remote call, should not be further propagated back to another server
     * because that server will interpret them as orignating on this server rather than the one this
     * server called.
     */
    static bool shouldErrorBePropagated(ErrorCodes::Error code);

protected:
    Shard(const ShardId& id);

private:
    /**
     * Runs the specified command against the shard backed by this object with a timeout set to the
     * minimum of maxTimeMSOverride or the timeout of the OperationContext.
     *
     * The return value exposes RemoteShard's host for calls to updateReplSetMonitor.
     *
     * NOTE: LocalShard implementation will not return a valid host and so should be ignored.
     */
    virtual StatusWith<CommandResponse> _runCommand(OperationContext* opCtx,
                                                    const ReadPreferenceSetting& readPref,
                                                    StringData dbname,
                                                    Milliseconds maxTimeMSOverride,
                                                    const BSONObj& cmdObj) = 0;

    virtual StatusWith<QueryResponse> _runExhaustiveCursorCommand(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        StringData dbName,
        Milliseconds maxTimeMSOverride,
        const BSONObj& cmdObj) = 0;

    virtual StatusWith<QueryResponse> _exhaustiveFindOnConfig(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const repl::ReadConcernLevel& readConcernLevel,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit,
        const boost::optional<BSONObj>& hint = boost::none) = 0;

    /**
     * Identifier of the shard as obtained from the configuration data (i.e. shard0000).
     */
    const ShardId _id;
};

}  // namespace mongo
