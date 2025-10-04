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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/*
 * Maintains the targeting and command execution logic for a single shard. Performs polling of
 * the shard (if replica set).
 */
class ShardRemote : public Shard {
    ShardRemote(const ShardRemote&) = delete;
    ShardRemote& operator=(const ShardRemote&) = delete;

public:
    /**
     * Instantiates a new shard connection management object for the specified shard.
     */
    ShardRemote(const ShardId& id,
                const ConnectionString& connString,
                std::unique_ptr<RemoteCommandTargeter> targeter);

    ~ShardRemote() override;

    const ConnectionString& getConnString() const override {
        return _connString;
    }

    std::shared_ptr<RemoteCommandTargeter> getTargeter() const override {
        return _targeter;
    }

    void updateReplSetMonitor(const HostAndPort& remoteHost,
                              const Status& remoteCommandStatus) override;

    std::string toString() const override;

    bool isRetriableError(ErrorCodes::Error code,
                          std::span<const std::string> errorLabels,
                          RetryPolicy options) const final;

    void runFireAndForgetCommand(OperationContext* opCtx,
                                 const ReadPreferenceSetting& readPref,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) final;

    BatchedCommandResponse runBatchWriteCommand(OperationContext* opCtx,
                                                Milliseconds maxTimeMS,
                                                const BatchedCommandRequest& batchRequest,
                                                const WriteConcernOptions& writeConcern,
                                                RetryPolicy retryPolicy) final;

private:
    struct AsyncCmdHandle {
        HostAndPort hostTargetted;
        executor::TaskExecutor::CallbackHandle handle;
    };

    /**
     * Returns the metadata that should be used when running commands against this shard with
     * the given read preference.
     */
    BSONObj _appendMetadataForCommand(OperationContext* opCtx,
                                      const ReadPreferenceSetting& readPref);

    StatusWith<Shard::CommandResponse> _runCommand(OperationContext* opCtx,
                                                   const ReadPreferenceSetting& readPref,
                                                   const DatabaseName& dbName,
                                                   Milliseconds maxTimeMSOverride,
                                                   const BSONObj& cmdObj) final;

    RetryStrategy::Result<Shard::QueryResponse> _runExhaustiveCursorCommand(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const DatabaseName& dbName,
        Milliseconds maxTimeMSOverride,
        const BSONObj& cmdObj) final;

    RetryStrategy::Result<QueryResponse> _exhaustiveFindOnConfig(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const repl::ReadConcernLevel& readConcernLevel,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit,
        const boost::optional<BSONObj>& hint = boost::none) final;

    RetryStrategy::Result<std::monostate> _runAggregation(
        OperationContext* opCtx,
        const AggregateCommandRequest& aggRequest,
        std::function<bool(const std::vector<BSONObj>& batch,
                           const boost::optional<BSONObj>& postBatchResumeToken)> callback) final;


    StatusWith<AsyncCmdHandle> _scheduleCommand(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const DatabaseName& dbName,
        Milliseconds maxTimeMSOverride,
        const BSONObj& cmdObj,
        const executor::TaskExecutor::RemoteCommandCallbackFn& cb);

    /**
     * Connection string for the shard at the creation time.
     */
    ConnectionString _connString;

    /**
     * Targeter for obtaining hosts from which to read or to which to write.
     */
    std::shared_ptr<RemoteCommandTargeter> _targeter;
};

}  // namespace mongo
