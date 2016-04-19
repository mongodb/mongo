/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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

#include "mongo/s/client/shard.h"

#include <string>

#include "mongo/client/query_fetcher.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using executor::TaskExecutor;
using RemoteCommandCallbackArgs = TaskExecutor::RemoteCommandCallbackArgs;

namespace {

const Seconds kConfigCommandTimeout{30};

// Include kReplSetMetadataFieldName in a request to get the shard's ReplSetMetadata in the
// response.
const BSONObj kReplMetadata(BSON(rpc::kReplSetMetadataFieldName << 1));

// Allow the command to be executed on a secondary (see ServerSelectionMetadata).
const BSONObj kSecondaryOkMetadata{rpc::ServerSelectionMetadata(true, boost::none).toBSON()};

// Helper for requesting ReplSetMetadata in the response as well as allowing the command to be
// executed on a secondary.
const BSONObj kReplSecondaryOkMetadata{[] {
    BSONObjBuilder o;
    o.appendElements(kSecondaryOkMetadata);
    o.appendElements(kReplMetadata);
    return o.obj();
}()};

}  // unnamed namespace

Shard::Shard(const ShardId& id,
             const ConnectionString& originalConnString,
             std::unique_ptr<RemoteCommandTargeter> targeter)
    : _id(id), _originalConnString(originalConnString), _targeter(targeter.release()) {}

Shard::~Shard() = default;

bool Shard::isConfig() const {
    return _id == "config";
}

const ConnectionString Shard::getConnString() const {
    return _targeter->connectionString();
}

std::string Shard::toString() const {
    return _id + ":" + _originalConnString.toString();
}

// TODO: the external interface for this is TBD; for now it exactly matches the internal command's.
StatusWith<Shard::CommandResponse> Shard::runCommand(OperationContext* txn,
                                                     const ReadPreferenceSetting& readPref,
                                                     const std::string& dbName,
                                                     const BSONObj& cmdObj,
                                                     const BSONObj& metadata) {
    return _runCommand(txn, readPref, dbName, cmdObj, metadata);
}

// TODO: the external interface for this is TBD; for now it exactly matches the internal command's.
StatusWith<Shard::QueryResponse> Shard::exhaustiveFindOnConfig(
    OperationContext* txn,
    const ReadPreferenceSetting& readPref,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    const boost::optional<long long> limit) {
    return _exhaustiveFindOnConfig(txn, readPref, nss, query, sort, limit);
}

void Shard::updateReplSetMonitor(const HostAndPort& remoteHost, const Status& remoteCommandStatus) {
    if (remoteCommandStatus.isOK())
        return;

    if (ErrorCodes::isNotMasterError(remoteCommandStatus.code()) ||
        (remoteCommandStatus == ErrorCodes::InterruptedDueToReplStateChange)) {
        _targeter->markHostNotMaster(remoteHost);
    } else if (ErrorCodes::isNetworkError(remoteCommandStatus.code())) {
        _targeter->markHostUnreachable(remoteHost);
    } else if (remoteCommandStatus == ErrorCodes::NotMasterOrSecondary) {
        _targeter->markHostUnreachable(remoteHost);
    } else if (remoteCommandStatus == ErrorCodes::ExceededTimeLimit) {
        _targeter->markHostUnreachable(remoteHost);
    }
}

StatusWith<Shard::CommandResponse> Shard::_runCommand(OperationContext* txn,
                                                      const ReadPreferenceSetting& readPref,
                                                      const string& dbName,
                                                      const BSONObj& cmdObj,
                                                      const BSONObj& metadata) {
    const auto host =
        _targeter->findHost(readPref, RemoteCommandTargeter::selectFindHostMaxWaitTime(txn));
    if (!host.isOK()) {
        return host.getStatus();
    }

    RemoteCommandRequest request(host.getValue(),
                                 dbName,
                                 cmdObj,
                                 metadata,
                                 isConfig() ? kConfigCommandTimeout
                                            : executor::RemoteCommandRequest::kNoTimeout);
    StatusWith<RemoteCommandResponse> swResponse =
        Status(ErrorCodes::InternalError, "Internal error running command");

    TaskExecutor* executor = Grid::get(txn)->getExecutorPool()->getFixedExecutor();
    auto callStatus = executor->scheduleRemoteCommand(
        request,
        [&swResponse](const RemoteCommandCallbackArgs& args) { swResponse = args.response; });
    if (!callStatus.isOK()) {
        return callStatus.getStatus();
    }

    // Block until the command is carried out
    executor->wait(callStatus.getValue());

    updateReplSetMonitor(host.getValue(), swResponse.getStatus());

    if (!swResponse.isOK()) {
        if (swResponse.getStatus().compareCode(ErrorCodes::ExceededTimeLimit)) {
            LOG(0) << "Operation timed out with status " << swResponse.getStatus();
        }
        return swResponse.getStatus();
    }

    CommandResponse cmdResponse;
    cmdResponse.response = swResponse.getValue().data.getOwned();
    cmdResponse.metadata = swResponse.getValue().metadata.getOwned();

    auto commandSpecificStatus = getStatusFromCommandResult(cmdResponse.response);
    updateReplSetMonitor(host.getValue(), commandSpecificStatus);

    return StatusWith<CommandResponse>(std::move(cmdResponse));
}

StatusWith<Shard::QueryResponse> Shard::_exhaustiveFindOnConfig(
    OperationContext* txn,
    const ReadPreferenceSetting& readPref,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {
    // Do not allow exhaustive finds to be run against regular shards.
    invariant(getId() == "config");

    const auto host =
        _targeter->findHost(readPref, RemoteCommandTargeter::selectFindHostMaxWaitTime(txn));
    if (!host.isOK()) {
        return host.getStatus();
    }

    QueryResponse response;

    // If for some reason the callback never gets invoked, we will return this status in response.
    Status status = Status(ErrorCodes::InternalError, "Internal error running find command");

    auto fetcherCallback = [this, &status, &response](
        const Fetcher::QueryResponseStatus& dataStatus, Fetcher::NextAction* nextAction) {

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

            response.opTime = replParseStatus.getValue().getLastOpVisible();

            // We return the config opTime that was returned for this particular request, but as a
            // safeguard we ensure our global configOpTime is at least as large as it.
            invariant(grid.configOpTime() >= response.opTime);
        }

        for (const BSONObj& doc : data.documents) {
            response.docs.push_back(doc.getOwned());
        }

        status = Status::OK();
    };

    BSONObj readConcernObj;
    {
        const repl::ReadConcernArgs readConcern{grid.configOpTime(),
                                                repl::ReadConcernLevel::kMajorityReadConcern};
        BSONObjBuilder bob;
        readConcern.appendInfo(&bob);
        readConcernObj =
            bob.done().getObjectField(repl::ReadConcernArgs::kReadConcernFieldName).getOwned();
    }

    auto lpq = LiteParsedQuery::makeAsFindCmd(nss,
                                              query,
                                              BSONObj(),  // projection
                                              sort,
                                              BSONObj(),  // hint
                                              readConcernObj,
                                              BSONObj(),    // collation
                                              boost::none,  // skip
                                              limit);

    BSONObjBuilder findCmdBuilder;
    lpq->asFindCommand(&findCmdBuilder);

    Seconds maxTime = kConfigCommandTimeout;
    Microseconds remainingTxnMaxTime(txn->getRemainingMaxTimeMicros());
    if (remainingTxnMaxTime != Microseconds::zero()) {
        maxTime = duration_cast<Seconds>(remainingTxnMaxTime);
    }

    findCmdBuilder.append(LiteParsedQuery::cmdOptionMaxTimeMS,
                          durationCount<Milliseconds>(maxTime));

    QueryFetcher fetcher(Grid::get(txn)->getExecutorPool()->getFixedExecutor(),
                         host.getValue(),
                         nss,
                         findCmdBuilder.done(),
                         fetcherCallback,
                         readPref.pref == ReadPreference::PrimaryOnly ? kReplMetadata
                                                                      : kReplSecondaryOkMetadata,
                         maxTime);
    Status scheduleStatus = fetcher.schedule();
    if (!scheduleStatus.isOK()) {
        return scheduleStatus;
    }

    fetcher.wait();

    updateReplSetMonitor(host.getValue(), status);

    if (!status.isOK()) {
        if (status.compareCode(ErrorCodes::ExceededTimeLimit)) {
            LOG(0) << "Operation timed out with status " << status;
        }
        return status;
    }

    return response;
}

}  // namespace mongo
