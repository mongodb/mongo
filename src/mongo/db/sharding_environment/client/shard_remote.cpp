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


#include "mongo/db/sharding_environment/client/shard_remote.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_gen.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using executor::TaskExecutor;
using RemoteCommandCallbackArgs = TaskExecutor::RemoteCommandCallbackArgs;

namespace {

// Include kReplSetMetadataFieldName in a request to get the shard's ReplSetMetadata in the
// response.
const BSONObj kReplMetadata(BSON(rpc::kReplSetMetadataFieldName << 1));

/**
 * Returns a new BSONObj describing the same command and arguments as 'cmdObj', but with maxTimeMS
 * replaced by maxTimeMSOverride (or removed if maxTimeMSOverride is Milliseconds::max()).
 */
BSONObj appendMaxTimeToCmdObj(Milliseconds maxTimeMSOverride, const BSONObj& cmdObj) {
    BSONObjBuilder updatedCmdBuilder;

    // Remove the user provided maxTimeMS so we can attach the one from the override
    for (const auto& elem : cmdObj) {
        if (elem.fieldNameStringData() != query_request_helper::cmdOptionMaxTimeMS) {
            updatedCmdBuilder.append(elem);
        }
    }

    if (maxTimeMSOverride < Milliseconds::max()) {
        updatedCmdBuilder.append(query_request_helper::cmdOptionMaxTimeMS,
                                 durationCount<Milliseconds>(maxTimeMSOverride));
    }

    return updatedCmdBuilder.obj();
}

}  // unnamed namespace

ShardRemote::ShardRemote(const ShardId& id,
                         const ConnectionString& connString,
                         std::unique_ptr<RemoteCommandTargeter> targeter)
    : Shard(id), _connString(connString), _targeter(std::move(targeter)) {}

ShardRemote::~ShardRemote() = default;

bool ShardRemote::isRetriableError(ErrorCodes::Error code, RetryPolicy options) const {
    return remoteIsRetriableError(code, options);
}

// Any error code changes should possibly also be made to Shard::shouldErrorBePropagated!
void ShardRemote::updateReplSetMonitor(const HostAndPort& remoteHost,
                                       const Status& remoteCommandStatus) {
    _targeter->updateHostWithStatus(remoteHost, remoteCommandStatus);
}

std::string ShardRemote::toString() const {
    return getId().toString() + ":" + _connString.toString();
}

BSONObj ShardRemote::_appendMetadataForCommand(OperationContext* opCtx,
                                               const ReadPreferenceSetting& readPref) {
    BSONObjBuilder builder;
    readPref.toContainingBSON(&builder);

    if (isConfig())
        builder.appendElements(kReplMetadata);

    return builder.obj();
}

StatusWith<Shard::CommandResponse> ShardRemote::_runCommand(OperationContext* opCtx,
                                                            const ReadPreferenceSetting& readPref,
                                                            const DatabaseName& dbName,
                                                            Milliseconds maxTimeMSOverride,
                                                            const BSONObj& cmdObj) {
    boost::optional<RemoteCommandResponse> response;
    auto asyncStatus = _scheduleCommand(
        opCtx,
        readPref,
        dbName,
        maxTimeMSOverride,
        cmdObj,
        [&response](const RemoteCommandCallbackArgs& args) { response = args.response; });

    if (!asyncStatus.isOK()) {
        return asyncStatus.getStatus();
    }

    auto asyncHandle = asyncStatus.getValue();

    // Block until the command is carried out
    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    try {
        executor->wait(asyncHandle.handle, opCtx);
    } catch (const DBException& e) {
        // If waiting for the response is interrupted, then we still have a callback out and
        // registered with the TaskExecutor to run when the response finally does come back.
        // Since the callback references local state, it would be invalid for the callback to run
        // after leaving the scope of this method.  Therefore we cancel the callback and wait
        // uninterruptably for the callback to be run.
        executor->cancel(asyncHandle.handle);
        executor->wait(asyncHandle.handle);
        return e.toStatus();
    }

    // After wait returns successfully, the callback in _scheduleCommand is guaranteed to have run
    // and set the response.
    invariant(response);

    const auto& host = asyncHandle.hostTargetted;
    updateReplSetMonitor(host, response->status);

    if (!response->status.isOK()) {
        if (ErrorCodes::isExceededTimeLimitError(response->status.code())) {
            LOGV2(22739, "Operation timed out", "error"_attr = redact(response->status));
        }
        return response->status;
    }

    auto result = response->data.getOwned();
    auto commandStatus = getStatusFromCommandResult(result);
    auto writeConcernStatus = getWriteConcernStatusFromCommandResult(result);

    updateReplSetMonitor(host, commandStatus);
    updateReplSetMonitor(host, writeConcernStatus);

    return Shard::CommandResponse(std::move(host),
                                  std::move(result),
                                  std::move(commandStatus),
                                  std::move(writeConcernStatus));
}

StatusWith<Shard::QueryResponse> ShardRemote::_runExhaustiveCursorCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const DatabaseName& dbName,
    Milliseconds maxTimeMSOverride,
    const BSONObj& cmdObj) {
    const auto host = _targeter->findHost(opCtx, readPref);
    if (!host.isOK()) {
        return host.getStatus();
    }

    QueryResponse response;

    // If for some reason the callback never gets invoked, we will return this status in response.
    Status status =
        Status(ErrorCodes::InternalError, "Internal error running cursor callback in command");

    auto fetcherCallback = [&status, &response](const Fetcher::QueryResponseStatus& dataStatus,
                                                Fetcher::NextAction* nextAction,
                                                BSONObjBuilder* getMoreBob) {
        // Throw out any accumulated results on error
        if (!dataStatus.isOK()) {
            status = dataStatus.getStatus();
            response.docs.clear();
            return;
        }

        const auto& data = dataStatus.getValue();

        if (data.otherFields.metadata.hasField(rpc::kReplSetMetadataFieldName)) {
            // Sharding users of ReplSetMetadata do not require the wall clock time field to be set.
            auto replParseStatus =
                rpc::ReplSetMetadata::readFromMetadata(data.otherFields.metadata);
            if (!replParseStatus.isOK()) {
                status = replParseStatus.getStatus();
                response.docs.clear();
                return;
            }

            const auto& replSetMetadata = replParseStatus.getValue();
            response.opTime = replSetMetadata.getLastOpCommitted().opTime;
        }

        for (const BSONObj& doc : data.documents) {
            response.docs.push_back(doc.getOwned());
        }

        status = Status::OK();

        if (!getMoreBob) {
            return;
        }
        getMoreBob->append("getMore", data.cursorId);
        getMoreBob->append("collection", data.nss.coll());
    };

    const Milliseconds requestTimeout = [&] {
        auto minMaxTimeMS = std::min(opCtx->getRemainingMaxTimeMillis(), maxTimeMSOverride);
        if (minMaxTimeMS < Milliseconds::max()) {
            return minMaxTimeMS;
        }
        // The Fetcher expects kNoTimeout when there is no maxTimeMS instead of Milliseconds::max().
        return RemoteCommandRequest::kNoTimeout;
    }();

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    Fetcher fetcher(executor.get(),
                    host.getValue(),
                    dbName,
                    cmdObj,
                    fetcherCallback,
                    _appendMetadataForCommand(opCtx, readPref),
                    requestTimeout, /* command network timeout */
                    requestTimeout /* getMore network timeout */);

    Status scheduleStatus = fetcher.schedule();
    if (!scheduleStatus.isOK()) {
        return scheduleStatus;
    }

    Status joinStatus = fetcher.join(opCtx);
    if (!joinStatus.isOK()) {
        if (ErrorCodes::isExceededTimeLimitError(joinStatus.code())) {
            LOGV2(6195000, "Operation timed out", "error"_attr = joinStatus);
        }

        return joinStatus;
    }

    updateReplSetMonitor(host.getValue(), status);

    if (!status.isOK()) {
        if (ErrorCodes::isExceededTimeLimitError(status.code())) {
            LOGV2(22740, "Operation timed out", "error"_attr = status);
        }
        return status;
    }

    return response;
}

Milliseconds getExhaustiveFindOnConfigMaxTimeMS(OperationContext* opCtx,
                                                const NamespaceString& nss) {
    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        // Don't use a timeout on the config server to guarantee it can always refresh.
        return Milliseconds::max();
    }

    return std::min(opCtx->getRemainingMaxTimeMillis(),
                    Shard::getConfiguredTimeoutForOperationOnNamespace(nss));
}

StatusWith<Shard::QueryResponse> ShardRemote::_exhaustiveFindOnConfig(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const repl::ReadConcernLevel& readConcernLevel,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit,
    const boost::optional<BSONObj>& hint) {

    invariant(isConfig());

    const auto configTime = [&] {
        const auto currentTime = VectorClock::get(opCtx)->getTime();
        return currentTime.configTime();
    }();

    const auto readPrefWithConfigTime = [&] {
        ReadPreferenceSetting readPrefToReturn{readPref};
        readPrefToReturn.minClusterTime = configTime.asTimestamp();
        return readPrefToReturn;
    }();


    invariant(readConcernLevel == repl::ReadConcernLevel::kMajorityReadConcern ||
              readConcernLevel == repl::ReadConcernLevel::kSnapshotReadConcern);
    repl::ReadConcernArgs readConcern{configTime /* afterClusterTime */, readConcernLevel};

    const Milliseconds maxTimeMS = getExhaustiveFindOnConfigMaxTimeMS(opCtx, nss);

    BSONObjBuilder findCmdBuilder;

    {
        FindCommandRequest findCommand(nss);
        findCommand.setFilter(query.getOwned());
        findCommand.setSort(sort.getOwned());
        findCommand.setReadConcern(readConcern);
        findCommand.setLimit(limit ? static_cast<boost::optional<std::int64_t>>(*limit)
                                   : boost::none);
        if (hint) {
            findCommand.setHint(*hint);
        }

        if (maxTimeMS < Milliseconds::max()) {
            findCommand.setMaxTimeMS(durationCount<Milliseconds>(maxTimeMS));
        }

        findCommand.serialize(&findCmdBuilder);
    }

    return _runExhaustiveCursorCommand(
        opCtx, readPrefWithConfigTime, nss.dbName(), maxTimeMS, findCmdBuilder.done());
}

void ShardRemote::runFireAndForgetCommand(OperationContext* opCtx,
                                          const ReadPreferenceSetting& readPref,
                                          const DatabaseName& dbName,
                                          const BSONObj& cmdObj) {
    _scheduleCommand(opCtx,
                     readPref,
                     dbName,
                     Milliseconds::max(),
                     cmdObj,
                     [](const RemoteCommandCallbackArgs&) {})
        .getStatus()
        .ignore();
}

Status ShardRemote::runAggregation(
    OperationContext* opCtx,
    const AggregateCommandRequest& aggRequest,
    std::function<bool(const std::vector<BSONObj>& batch,
                       const boost::optional<BSONObj>& postBatchResumeToken)> callback) {

    BSONObj readPrefMetadata;

    ReadPreferenceSetting readPreference =
        uassertStatusOK(ReadPreferenceSetting::fromContainingBSON(
            aggRequest.getUnwrappedReadPref().value_or(BSONObj()),
            ReadPreference::SecondaryPreferred));

    auto swHost = _targeter->findHost(opCtx, readPreference);
    if (!swHost.isOK()) {
        return swHost.getStatus();
    }
    HostAndPort host = swHost.getValue();

    BSONObjBuilder builder;
    readPreference.toContainingBSON(&builder);
    readPrefMetadata = builder.obj();

    Status status =
        Status(ErrorCodes::InternalError, "Internal error running cursor callback in command");
    auto fetcherCallback = [&status, callback](const Fetcher::QueryResponseStatus& dataStatus,
                                               Fetcher::NextAction* nextAction,
                                               BSONObjBuilder* getMoreBob) {
        // Throw out any accumulated results on error
        if (!dataStatus.isOK()) {
            status = dataStatus.getStatus();
            return;
        }

        const auto& data = dataStatus.getValue();

        if (data.otherFields.metadata.hasField(rpc::kReplSetMetadataFieldName)) {
            // Sharding users of ReplSetMetadata do not require the wall clock time field to be set.
            auto replParseStatus =
                rpc::ReplSetMetadata::readFromMetadata(data.otherFields.metadata);
            if (!replParseStatus.isOK()) {
                status = replParseStatus.getStatus();
                return;
            }
        }

        try {
            // The "postBatchResumeToken" is the highest _id seen in the collection scan. The
            // document with that _id  may or may not be part of the batch itself.
            // (a) It could be that the document got filtered out from the batch due
            //     to $match.
            // (b) It could be that the document just hasn't been yet in the batch due to response
            //     size limit.
            // Due to (b), it is only correct to consult the postBatchResumeToken when the batch is
            // empty.
            boost::optional<BSONObj> postBatchResumeToken =
                data.documents.empty() ? data.otherFields.postBatchResumeToken : boost::none;
            if (!callback(data.documents, postBatchResumeToken)) {
                *nextAction = Fetcher::NextAction::kNoAction;
            }
        } catch (...) {
            status = exceptionToStatus();
            return;
        }

        status = Status::OK();

        if (!getMoreBob) {
            return;
        }
        getMoreBob->append("getMore", data.cursorId);
        getMoreBob->append("collection", data.nss.coll());
    };

    Milliseconds requestTimeout(-1);
    if (aggRequest.getMaxTimeMS()) {
        requestTimeout = Milliseconds(aggRequest.getMaxTimeMS().value_or(0));
    }

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    Fetcher fetcher(executor.get(),
                    host,
                    aggRequest.getNamespace().dbName(),
                    aggRequest.toBSON(),
                    fetcherCallback,
                    readPrefMetadata,
                    requestTimeout, /* command network timeout */
                    requestTimeout /* getMore network timeout */);

    Status scheduleStatus = fetcher.schedule();
    if (!scheduleStatus.isOK()) {
        return scheduleStatus;
    }

    Status joinStatus = fetcher.join(opCtx);
    if (!joinStatus.isOK()) {
        return joinStatus;
    }

    updateReplSetMonitor(host, status);

    return status;
}


BatchedCommandResponse ShardRemote::runBatchWriteCommand(OperationContext* opCtx,
                                                         const Milliseconds maxTimeMS,
                                                         const BatchedCommandRequest& batchRequest,
                                                         const WriteConcernOptions& writeConcern,
                                                         RetryPolicy retryPolicy) {
    const DatabaseName dbName = batchRequest.getNS().dbName();
    const BSONObj cmdObj = [&] {
        BSONObjBuilder cmdObjBuilder;
        batchRequest.serialize(&cmdObjBuilder);
        cmdObjBuilder.append(WriteConcernOptions::kWriteConcernField, writeConcern.toBSON());
        return cmdObjBuilder.obj();
    }();

    return _submitBatchWriteCommand(opCtx, cmdObj, dbName, maxTimeMS, retryPolicy);
}


StatusWith<ShardRemote::AsyncCmdHandle> ShardRemote::_scheduleCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const DatabaseName& dbName,
    Milliseconds maxTimeMSOverride,
    const BSONObj& cmdObj,
    const TaskExecutor::RemoteCommandCallbackFn& cb) {

    const auto swHost = _targeter->findHost(opCtx, readPref);
    if (!swHost.isOK()) {
        return swHost.getStatus();
    }

    AsyncCmdHandle asyncHandle;
    asyncHandle.hostTargetted = std::move(swHost.getValue());

    const Milliseconds requestTimeout =
        std::min(opCtx->getRemainingMaxTimeMillis(), maxTimeMSOverride);
    auto hasMaxTimeMS = requestTimeout < Milliseconds::max();

    RemoteCommandRequest request(asyncHandle.hostTargetted,
                                 dbName,
                                 appendMaxTimeToCmdObj(requestTimeout, cmdObj),
                                 _appendMetadataForCommand(opCtx, readPref),
                                 opCtx,
                                 hasMaxTimeMS ? requestTimeout : RemoteCommandRequest::kNoTimeout);

    if (hasMaxTimeMS)
        request.timeoutCode = boost::make_optional<ErrorCodes::Error>(ErrorCodes::MaxTimeMSExpired);

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto swHandle = executor->scheduleRemoteCommand(request, cb);

    if (!swHandle.isOK()) {
        return swHandle.getStatus();
    }

    asyncHandle.handle = std::move(swHandle.getValue());
    return asyncHandle;
}

}  // namespace mongo
