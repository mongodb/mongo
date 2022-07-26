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


#include "mongo/s/client/shard_remote.h"

#include <algorithm>
#include <string>

#include "mongo/client/fetcher.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/vector_clock.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/client/shard_remote_gen.h"
#include "mongo/s/grid.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using executor::TaskExecutor;
using rpc::TrackingMetadata;
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

bool ShardRemote::isRetriableError(ErrorCodes::Error code, RetryPolicy options) {
    if (gInternalProhibitShardOperationRetry.loadRelaxed()) {
        return false;
    }

    switch (options) {
        case RetryPolicy::kNoRetry: {
            return false;
        } break;

        case RetryPolicy::kIdempotent: {
            return isMongosRetriableError(code);
        } break;

        case RetryPolicy::kIdempotentOrCursorInvalidated: {
            return isRetriableError(code, Shard::RetryPolicy::kIdempotent) ||
                ErrorCodes::isCursorInvalidatedError(code);
        } break;

        case RetryPolicy::kNotIdempotent: {
            return ErrorCodes::isNotPrimaryError(code);
        } break;
    }

    MONGO_UNREACHABLE;
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
    if (shouldLog(logv2::LogComponent::kTracking,
                  logv2::LogSeverity::Debug(1))) {  // avoid performance overhead if not logging
        if (!TrackingMetadata::get(opCtx).getIsLogged()) {
            if (!TrackingMetadata::get(opCtx).getOperId()) {
                TrackingMetadata::get(opCtx).initWithOperName("NotSet");
            }
            LOGV2_DEBUG_OPTIONS(20164,
                                1,
                                logv2::LogOptions{logv2::LogComponent::kTracking},
                                "{trackingMetadata}",
                                "trackingMetadata"_attr = TrackingMetadata::get(opCtx));
            TrackingMetadata::get(opCtx).setIsLogged(true);
        }

        TrackingMetadata metadata = TrackingMetadata::get(opCtx).constructChildMetadata();
        metadata.writeToMetadata(&builder);
    }

    readPref.toContainingBSON(&builder);

    if (isConfig())
        builder.appendElements(kReplMetadata);

    return builder.obj();
}

StatusWith<Shard::CommandResponse> ShardRemote::_runCommand(OperationContext* opCtx,
                                                            const ReadPreferenceSetting& readPref,
                                                            StringData dbName,
                                                            Milliseconds maxTimeMSOverride,
                                                            const BSONObj& cmdObj) {
    RemoteCommandResponse response =
        Status(ErrorCodes::InternalError,
               str::stream() << "Failed to run remote command request cmd: " << cmdObj);

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

    const auto& host = asyncHandle.hostTargetted;
    updateReplSetMonitor(host, response.status);

    if (!response.status.isOK()) {
        if (ErrorCodes::isExceededTimeLimitError(response.status.code())) {
            LOGV2(22739,
                  "Operation timed out {error}",
                  "Operation timed out",
                  "error"_attr = redact(response.status));
        }
        return response.status;
    }

    auto result = response.data.getOwned();
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
    StringData dbName,
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

    const Milliseconds requestTimeout =
        std::min(opCtx->getRemainingMaxTimeMillis(), maxTimeMSOverride);

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    Fetcher fetcher(executor.get(),
                    host.getValue(),
                    dbName.toString(),
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
            LOGV2(6195000,
                  "Operation timed out {error}",
                  "Operation timed out",
                  "error"_attr = joinStatus);
        }

        return joinStatus;
    }

    updateReplSetMonitor(host.getValue(), status);

    if (!status.isOK()) {
        if (ErrorCodes::isExceededTimeLimitError(status.code())) {
            LOGV2(
                22740, "Operation timed out {error}", "Operation timed out", "error"_attr = status);
        }
        return status;
    }

    return response;
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

    BSONObj readConcernObj = [&] {
        invariant(readConcernLevel == repl::ReadConcernLevel::kMajorityReadConcern);
        repl::OpTime configOpTime{configTime.asTimestamp(),
                                  mongo::repl::OpTime::kUninitializedTerm};
        repl::ReadConcernArgs readConcern{configOpTime, readConcernLevel};
        BSONObjBuilder bob;
        readConcern.appendInfo(&bob);
        return bob.done().getObjectField(repl::ReadConcernArgs::kReadConcernFieldName).getOwned();
    }();

    const Milliseconds maxTimeMS =
        std::min(opCtx->getRemainingMaxTimeMillis(),
                 nss == ChunkType::ConfigNS ? Milliseconds(gFindChunksOnConfigTimeoutMS.load())
                                            : kDefaultConfigCommandTimeout);

    BSONObjBuilder findCmdBuilder;

    {
        FindCommandRequest findCommand(nss);
        findCommand.setFilter(query.getOwned());
        findCommand.setSort(sort.getOwned());
        findCommand.setReadConcern(readConcernObj.getOwned());
        findCommand.setLimit(limit ? static_cast<boost::optional<std::int64_t>>(*limit)
                                   : boost::none);
        if (hint) {
            findCommand.setHint(*hint);
        }

        if (maxTimeMS < Milliseconds::max()) {
            findCommand.setMaxTimeMS(durationCount<Milliseconds>(maxTimeMS));
        }

        findCommand.serialize(BSONObj(), &findCmdBuilder);
    }

    return _runExhaustiveCursorCommand(
        opCtx, readPrefWithConfigTime, nss.db().toString(), maxTimeMS, findCmdBuilder.done());
}

void ShardRemote::runFireAndForgetCommand(OperationContext* opCtx,
                                          const ReadPreferenceSetting& readPref,
                                          const std::string& dbName,
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
                    aggRequest.getNamespace().db().toString(),
                    aggregation_request_helper::serializeToCommandObj(aggRequest),
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


StatusWith<ShardRemote::AsyncCmdHandle> ShardRemote::_scheduleCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    StringData dbName,
    Milliseconds maxTimeMSOverride,
    const BSONObj& cmdObj,
    const TaskExecutor::RemoteCommandCallbackFn& cb) {

    const auto readPrefWithConfigTime = [&]() -> ReadPreferenceSetting {
        if (isConfig()) {
            const auto vcTime = VectorClock::get(opCtx)->getTime();
            ReadPreferenceSetting readPrefToReturn{readPref};
            readPrefToReturn.minClusterTime = vcTime.configTime().asTimestamp();
            return readPrefToReturn;
        } else {
            return {readPref};
        }
    }();

    const auto swHost = _targeter->findHost(opCtx, readPrefWithConfigTime);
    if (!swHost.isOK()) {
        return swHost.getStatus();
    }

    AsyncCmdHandle asyncHandle;
    asyncHandle.hostTargetted = std::move(swHost.getValue());

    const Milliseconds requestTimeout =
        std::min(opCtx->getRemainingMaxTimeMillis(), maxTimeMSOverride);

    const RemoteCommandRequest request(
        asyncHandle.hostTargetted,
        dbName.toString(),
        appendMaxTimeToCmdObj(requestTimeout, cmdObj),
        _appendMetadataForCommand(opCtx, readPrefWithConfigTime),
        opCtx,
        requestTimeout < Milliseconds::max() ? requestTimeout : RemoteCommandRequest::kNoTimeout);

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto swHandle = executor->scheduleRemoteCommand(request, cb);

    if (!swHandle.isOK()) {
        return swHandle.getStatus();
    }

    asyncHandle.handle = std::move(swHandle.getValue());
    return asyncHandle;
}

}  // namespace mongo
