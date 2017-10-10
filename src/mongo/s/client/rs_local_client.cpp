/**
 * Copyright (C) 2017 MongoDB Inc.
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

#include <boost/none_t.hpp>

#include "mongo/platform/basic.h"

#include "mongo/s/client/rs_local_client.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/client/shard.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

void RSLocalClient::_updateLastOpTimeFromClient(OperationContext* opCtx,
                                                const repl::OpTime& previousOpTimeOnClient) {
    const auto lastOpTimeFromClient =
        repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

    if (lastOpTimeFromClient.isNull() || lastOpTimeFromClient == previousOpTimeOnClient) {
        return;
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (lastOpTimeFromClient >= _lastOpTime) {
        // It's always possible for lastOpTimeFromClient to be less than _lastOpTime if another
        // thread started and completed a write through this ShardLocal (updating _lastOpTime)
        // after this operation had completed its write but before it got here.
        _lastOpTime = lastOpTimeFromClient;
    }
}

repl::OpTime RSLocalClient::_getLastOpTime() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _lastOpTime;
}

StatusWith<Shard::CommandResponse> RSLocalClient::runCommandOnce(OperationContext* opCtx,
                                                                 const std::string& dbName,
                                                                 const BSONObj& cmdObj) {
    const auto currentOpTimeFromClient =
        repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    ON_BLOCK_EXIT([this, &opCtx, &currentOpTimeFromClient] {
        _updateLastOpTimeFromClient(opCtx, currentOpTimeFromClient);
    });

    try {
        DBDirectClient client(opCtx);

        rpc::UniqueReply commandResponse =
            client.runCommand(OpMsgRequest::fromDBAndBody(dbName, cmdObj));

        auto result = commandResponse->getCommandReply().getOwned();
        return Shard::CommandResponse(boost::none,
                                      result,
                                      commandResponse->getMetadata().getOwned(),
                                      getStatusFromCommandResult(result),
                                      getWriteConcernStatusFromCommandResult(result));
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<Shard::QueryResponse> RSLocalClient::queryOnce(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const repl::ReadConcernLevel& readConcernLevel,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    if (readConcernLevel == repl::ReadConcernLevel::kMajorityReadConcern) {
        // Set up operation context with majority read snapshot so correct optime can be retrieved.
        Status status = opCtx->recoveryUnit()->setReadFromMajorityCommittedSnapshot();

        // Wait for any writes performed by this ShardLocal instance to be committed and visible.
        Status readConcernStatus = replCoord->waitUntilOpTimeForRead(
            opCtx, repl::ReadConcernArgs{_getLastOpTime(), readConcernLevel});
        if (!readConcernStatus.isOK()) {
            return readConcernStatus;
        }

        // Inform the storage engine to read from the committed snapshot for the rest of this
        // operation.
        status = opCtx->recoveryUnit()->setReadFromMajorityCommittedSnapshot();
        if (!status.isOK()) {
            return status;
        }
    } else {
        invariant(readConcernLevel == repl::ReadConcernLevel::kLocalReadConcern);
    }

    DBDirectClient client(opCtx);
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

}  // namespace mongo
