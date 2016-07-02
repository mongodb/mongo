/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/client/shard_local.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
const Status kInternalErrorStatus{ErrorCodes::InternalError,
                                  "Invalid to check for write concern error if command failed"};
}  // namespace

const ConnectionString ShardLocal::getConnString() const {
    auto replCoord = repl::getGlobalReplicationCoordinator();

    // Currently ShardLocal only works for config servers, which must be replica sets.  If we
    // ever start using ShardLocal on shards we'll need to consider how to handle shards that are
    // not replica sets.
    invariant(replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet);
    return replCoord->getConfig().getConnectionString();
}

std::shared_ptr<RemoteCommandTargeter> ShardLocal::getTargeter() const {
    MONGO_UNREACHABLE;
};

const ConnectionString ShardLocal::originalConnString() const {
    // Return the local connection string here as this method is only used for updating the
    // ShardRegistry and we don't need a mapping from hosts in the replica set config to the shard
    // for local shards.
    return ConnectionString::forLocal();
}

void ShardLocal::updateReplSetMonitor(const HostAndPort& remoteHost,
                                      const Status& remoteCommandStatus) {
    MONGO_UNREACHABLE;
}

std::string ShardLocal::toString() const {
    return getId().toString() + ":<local>";
}

bool ShardLocal::isRetriableError(ErrorCodes::Error code, RetryPolicy options) {
    if (options == RetryPolicy::kNoRetry) {
        return false;
    }

    if (options == RetryPolicy::kIdempotent) {
        return code == ErrorCodes::WriteConcernFailed;
    } else {
        invariant(options == RetryPolicy::kNotIdempotent);
        return false;
    }
}

StatusWith<Shard::CommandResponse> ShardLocal::_runCommand(OperationContext* txn,
                                                           const ReadPreferenceSetting& unused,
                                                           const std::string& dbName,
                                                           const BSONObj& cmdObj) {
    try {
        DBDirectClient client(txn);
        rpc::UniqueReply commandResponse = client.runCommandWithMetadata(
            dbName, cmdObj.firstElementFieldName(), rpc::makeEmptyMetadata(), cmdObj);
        BSONObj responseReply = commandResponse->getCommandReply().getOwned();
        BSONObj responseMetadata = commandResponse->getMetadata().getOwned();

        Status commandStatus = getStatusFromCommandResult(responseReply);
        Status writeConcernStatus = kInternalErrorStatus;
        if (commandStatus.isOK()) {
            writeConcernStatus = getWriteConcernStatusFromCommandResult(responseReply);
        }

        return Shard::CommandResponse{std::move(responseReply),
                                      std::move(responseMetadata),
                                      std::move(commandStatus),
                                      std::move(writeConcernStatus)};
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<Shard::QueryResponse> ShardLocal::_exhaustiveFindOnConfig(
    OperationContext* txn,
    const ReadPreferenceSetting& readPref,
    const repl::ReadConcernLevel& readConcernLevel,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {
    auto replCoord = repl::ReplicationCoordinator::get(txn);

    if (readConcernLevel == repl::ReadConcernLevel::kMajorityReadConcern) {
        // Set up operation context with majority read snapshot so correct optime can be retrieved.
        Status status = txn->recoveryUnit()->setReadFromMajorityCommittedSnapshot();

        // Wait until a snapshot is available.
        while (status == ErrorCodes::ReadConcernMajorityNotAvailableYet) {
            LOG(1) << "Waiting for ReadFromMajorityCommittedSnapshot to become available";
            replCoord->waitUntilSnapshotCommitted(txn, SnapshotName::min());
            status = txn->recoveryUnit()->setReadFromMajorityCommittedSnapshot();
        }

        if (!status.isOK()) {
            return status;
        }
    } else {
        invariant(readConcernLevel == repl::ReadConcernLevel::kLocalReadConcern);
    }

    DBDirectClient client(txn);
    Query fullQuery(query);
    if (!sort.isEmpty()) {
        fullQuery.sort(sort);
    }
    fullQuery.readPref(readPref.pref, BSONArray());

    try {
        std::unique_ptr<DBClientCursor> cursor =
            client.query(nss.ns().c_str(), fullQuery, limit.get_value_or(0));

        if (!cursor) {
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to establish a cursor for reading " << nss.ns()
                                  << " from local storage"};
        }

        std::vector<BSONObj> documentVector;
        while (cursor->more()) {
            BSONObj document = cursor->nextSafe().getOwned();
            documentVector.push_back(std::move(document));
        }

        return Shard::QueryResponse{std::move(documentVector),
                                    replCoord->getCurrentCommittedSnapshotOpTime()};
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

Status ShardLocal::createIndexOnConfig(OperationContext* txn,
                                       const NamespaceString& ns,
                                       const BSONObj& keys,
                                       bool unique) {
    invariant(ns.db() == "config" || ns.db() == "admin");

    try {
        DBDirectClient client(txn);
        IndexSpec index;
        index.addKeys(keys);
        index.unique(unique);
        client.createIndex(ns.toString(), index);
    } catch (const DBException& e) {
        return e.toStatus();
    }

    return Status::OK();
}

}  // namespace mongo
