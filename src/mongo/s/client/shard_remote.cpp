/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

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
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

using std::string;

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using executor::TaskExecutor;
using rpc::TrackingMetadata;
using RemoteCommandCallbackArgs = TaskExecutor::RemoteCommandCallbackArgs;

namespace {
// Include kReplSetMetadataFieldName in a request to get the shard's ReplSetMetadata in the
// response.
const BSONObj kReplMetadata(BSON(rpc::kReplSetMetadataFieldName << 1));

// Allow the command to be executed on a secondary (see ServerSelectionMetadata).
const BSONObj kSecondaryOkMetadata{rpc::ServerSelectionMetadata(true, boost::none).toBSON()};

/**
 * Returns a new BSONObj describing the same command and arguments as 'cmdObj', but with maxTimeMS
 * replaced by maxTimeMSOverride (or removed if maxTimeMSOverride is Milliseconds::max()).
 */
BSONObj appendMaxTimeToCmdObj(Milliseconds maxTimeMSOverride, const BSONObj& cmdObj) {
    BSONObjBuilder updatedCmdBuilder;

    // Remove the user provided maxTimeMS so we can attach the one from the override
    for (const auto& elem : cmdObj) {
        if (!str::equals(elem.fieldName(), QueryRequest::cmdOptionMaxTimeMS)) {
            updatedCmdBuilder.append(elem);
        }
    }

    if (maxTimeMSOverride < Milliseconds::max()) {
        updatedCmdBuilder.append(QueryRequest::cmdOptionMaxTimeMS,
                                 durationCount<Milliseconds>(maxTimeMSOverride));
    }

    return updatedCmdBuilder.obj();
}

}  // unnamed namespace

ShardRemote::ShardRemote(const ShardId& id,
                         const ConnectionString& originalConnString,
                         std::unique_ptr<RemoteCommandTargeter> targeter)
    : Shard(id), _originalConnString(originalConnString), _targeter(targeter.release()) {}

ShardRemote::~ShardRemote() = default;

bool ShardRemote::isRetriableError(ErrorCodes::Error code, RetryPolicy options) {
    if (options == RetryPolicy::kNoRetry) {
        return false;
    }

    const auto& retriableErrors = options == RetryPolicy::kIdempotent
        ? RemoteCommandRetryScheduler::kAllRetriableErrors
        : RemoteCommandRetryScheduler::kNotMasterErrors;
    return std::find(retriableErrors.begin(), retriableErrors.end(), code) != retriableErrors.end();
}

const ConnectionString ShardRemote::getConnString() const {
    return _targeter->connectionString();
}

// Any error code changes should possibly also be made to Shard::shouldErrorBePropagated!
void ShardRemote::updateReplSetMonitor(const HostAndPort& remoteHost,
                                       const Status& remoteCommandStatus) {
    if (remoteCommandStatus.isOK())
        return;

    if (ErrorCodes::isNotMasterError(remoteCommandStatus.code()) ||
        (remoteCommandStatus == ErrorCodes::InterruptedDueToReplStateChange) ||
        (remoteCommandStatus == ErrorCodes::PrimarySteppedDown)) {
        _targeter->markHostNotMaster(remoteHost, remoteCommandStatus);
    } else if (ErrorCodes::isNetworkError(remoteCommandStatus.code())) {
        _targeter->markHostUnreachable(remoteHost, remoteCommandStatus);
    } else if (remoteCommandStatus == ErrorCodes::NotMasterOrSecondary) {
        _targeter->markHostUnreachable(remoteHost, remoteCommandStatus);
    } else if (remoteCommandStatus == ErrorCodes::ExceededTimeLimit) {
        _targeter->markHostUnreachable(remoteHost, remoteCommandStatus);
    }
}

std::string ShardRemote::toString() const {
    return getId().toString() + ":" + _originalConnString.toString();
}

BSONObj ShardRemote::_appendMetadataForCommand(OperationContext* txn,
                                               const ReadPreferenceSetting& readPref) {
    BSONObjBuilder builder;
    if (logger::globalLogDomain()->shouldLog(
            logger::LogComponent::kTracking,
            logger::LogSeverity::Debug(1))) {  // avoid performance overhead if not logging
        if (!TrackingMetadata::get(txn).getIsLogged()) {
            if (!TrackingMetadata::get(txn).getOperId()) {
                TrackingMetadata::get(txn).initWithOperName("NotSet");
            }
            MONGO_LOG_COMPONENT(1, logger::LogComponent::kTracking)
                << TrackingMetadata::get(txn).toString();
            TrackingMetadata::get(txn).setIsLogged(true);
        }

        TrackingMetadata metadata = TrackingMetadata::get(txn).constructChildMetadata();
        metadata.writeToMetadata(&builder);
    }

    if (isConfig()) {
        if (readPref.pref == ReadPreference::PrimaryOnly) {
            builder.appendElements(kReplMetadata);
        } else {
            builder.appendElements(kSecondaryOkMetadata);
            builder.appendElements(kReplMetadata);
        }
    } else {
        if (readPref.pref != ReadPreference::PrimaryOnly) {
            builder.appendElements(kSecondaryOkMetadata);
        }
    }
    return builder.obj();
}

Shard::HostWithResponse ShardRemote::_runCommand(OperationContext* txn,
                                                 const ReadPreferenceSetting& readPref,
                                                 const string& dbName,
                                                 Milliseconds maxTimeMSOverride,
                                                 const BSONObj& cmdObj) {

    ReadPreferenceSetting readPrefWithMinOpTime(readPref);
    if (getId() == "config") {
        readPrefWithMinOpTime.minOpTime = grid.configOpTime();
    }
    const auto host = _targeter->findHost(txn, readPrefWithMinOpTime);
    if (!host.isOK()) {
        return Shard::HostWithResponse(boost::none, host.getStatus());
    }

    const Milliseconds requestTimeout =
        std::min(txn->getRemainingMaxTimeMillis(), maxTimeMSOverride);

    const RemoteCommandRequest request(
        host.getValue(),
        dbName,
        appendMaxTimeToCmdObj(maxTimeMSOverride, cmdObj),
        _appendMetadataForCommand(txn, readPrefWithMinOpTime),
        txn,
        requestTimeout < Milliseconds::max() ? requestTimeout : RemoteCommandRequest::kNoTimeout);

    RemoteCommandResponse swResponse =
        Status(ErrorCodes::InternalError, "Internal error running command");

    TaskExecutor* executor = Grid::get(txn)->getExecutorPool()->getFixedExecutor();
    auto callStatus = executor->scheduleRemoteCommand(
        request,
        [&swResponse](const RemoteCommandCallbackArgs& args) { swResponse = args.response; });
    if (!callStatus.isOK()) {
        return Shard::HostWithResponse(host.getValue(), callStatus.getStatus());
    }

    // Block until the command is carried out
    executor->wait(callStatus.getValue());

    updateReplSetMonitor(host.getValue(), swResponse.status);

    if (!swResponse.isOK()) {
        if (swResponse.status.compareCode(ErrorCodes::ExceededTimeLimit)) {
            LOG(0) << "Operation timed out with status " << redact(swResponse.status);
        }
        return Shard::HostWithResponse(host.getValue(), swResponse.status);
    }

    BSONObj responseObj = swResponse.data.getOwned();
    BSONObj responseMetadata = swResponse.metadata.getOwned();
    Status commandStatus = getStatusFromCommandResult(responseObj);
    Status writeConcernStatus = getWriteConcernStatusFromCommandResult(responseObj);

    // Tell the replica set monitor of any errors
    updateReplSetMonitor(host.getValue(), commandStatus);
    updateReplSetMonitor(host.getValue(), writeConcernStatus);

    return Shard::HostWithResponse(host.getValue(),
                                   CommandResponse(std::move(responseObj),
                                                   std::move(responseMetadata),
                                                   std::move(commandStatus),
                                                   std::move(writeConcernStatus)));
}

StatusWith<Shard::QueryResponse> ShardRemote::_exhaustiveFindOnConfig(
    OperationContext* txn,
    const ReadPreferenceSetting& readPref,
    const repl::ReadConcernLevel& readConcernLevel,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {
    invariant(getId() == "config");
    ReadPreferenceSetting readPrefWithMinOpTime(readPref);
    readPrefWithMinOpTime.minOpTime = grid.configOpTime();

    const auto host = _targeter->findHost(txn, readPrefWithMinOpTime);
    if (!host.isOK()) {
        return host.getStatus();
    }

    QueryResponse response;

    // If for some reason the callback never gets invoked, we will return this status in response.
    Status status = Status(ErrorCodes::InternalError, "Internal error running find command");

    auto fetcherCallback =
        [this, &status, &response](const Fetcher::QueryResponseStatus& dataStatus,
                                   Fetcher::NextAction* nextAction,
                                   BSONObjBuilder* getMoreBob) {

            // Throw out any accumulated results on error
            if (!dataStatus.isOK()) {
                status = dataStatus.getStatus();
                response.docs.clear();
                return;
            }

            auto& data = dataStatus.getValue();
            if (data.otherFields.metadata.hasField(rpc::kReplSetMetadataFieldName)) {
                auto replParseStatus =
                    rpc::ReplSetMetadata::readFromMetadata(data.otherFields.metadata);

                if (!replParseStatus.isOK()) {
                    status = replParseStatus.getStatus();
                    response.docs.clear();
                    return;
                }

                response.opTime = replParseStatus.getValue().getLastOpCommitted();

                // We return the config opTime that was returned for this particular request, but as
                // a safeguard we ensure our global configOpTime is at least as large as it.
                invariant(grid.configOpTime() >= response.opTime);
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

    BSONObj readConcernObj;
    {
        invariant(readConcernLevel == repl::ReadConcernLevel::kMajorityReadConcern);
        const repl::ReadConcernArgs readConcern{grid.configOpTime(), readConcernLevel};
        BSONObjBuilder bob;
        readConcern.appendInfo(&bob);
        readConcernObj =
            bob.done().getObjectField(repl::ReadConcernArgs::kReadConcernFieldName).getOwned();
    }

    const Milliseconds maxTimeMS =
        std::min(txn->getRemainingMaxTimeMillis(), kDefaultConfigCommandTimeout);

    BSONObjBuilder findCmdBuilder;

    {
        QueryRequest qr(nss);
        qr.setFilter(query);
        qr.setSort(sort);
        qr.setReadConcern(readConcernObj);
        qr.setLimit(limit);

        if (maxTimeMS < Milliseconds::max()) {
            qr.setMaxTimeMS(durationCount<Milliseconds>(maxTimeMS));
        }

        qr.asFindCommand(&findCmdBuilder);
    }

    Fetcher fetcher(Grid::get(txn)->getExecutorPool()->getFixedExecutor(),
                    host.getValue(),
                    nss.db().toString(),
                    findCmdBuilder.done(),
                    fetcherCallback,
                    _appendMetadataForCommand(txn, readPrefWithMinOpTime),
                    maxTimeMS);
    Status scheduleStatus = fetcher.schedule();
    if (!scheduleStatus.isOK()) {
        return scheduleStatus;
    }

    fetcher.join();

    updateReplSetMonitor(host.getValue(), status);

    if (!status.isOK()) {
        if (status.compareCode(ErrorCodes::ExceededTimeLimit)) {
            LOG(0) << "Operation timed out " << causedBy(status);
        }
        return status;
    }

    return response;
}

Status ShardRemote::createIndexOnConfig(OperationContext* txn,
                                        const NamespaceString& ns,
                                        const BSONObj& keys,
                                        bool unique) {
    MONGO_UNREACHABLE;
}

}  // namespace mongo
