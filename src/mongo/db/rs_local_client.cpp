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

#include "mongo/platform/basic.h"

#include "mongo/db/rs_local_client.h"

#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

void RSLocalClient::_updateLastOpTimeFromClient(OperationContext* opCtx,
                                                const repl::OpTime& previousOpTimeOnClient) {
    const auto lastOpTimeFromClient =
        repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

    if (lastOpTimeFromClient.isNull() || lastOpTimeFromClient == previousOpTimeOnClient) {
        return;
    }

    stdx::lock_guard<Latch> lk(_mutex);
    if (lastOpTimeFromClient >= _lastOpTime) {
        // It's always possible for lastOpTimeFromClient to be less than _lastOpTime if another
        // thread started and completed a write through this ShardLocal (updating _lastOpTime)
        // after this operation had completed its write but before it got here.
        _lastOpTime = lastOpTimeFromClient;
    }
}

repl::OpTime RSLocalClient::_getLastOpTime() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _lastOpTime;
}

StatusWith<Shard::CommandResponse> RSLocalClient::runCommandOnce(OperationContext* opCtx,
                                                                 StringData dbName,
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
    boost::optional<long long> limit,
    const boost::optional<BSONObj>& hint) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    boost::optional<ScopeGuard<std::function<void()>>> readSourceGuard;

    if (readConcernLevel == repl::ReadConcernLevel::kMajorityReadConcern) {
        // Resets to the original read source at the end of this operation.
        auto originalReadSource = opCtx->recoveryUnit()->getTimestampReadSource();
        boost::optional<Timestamp> originalReadTimestamp;
        if (originalReadSource == RecoveryUnit::ReadSource::kProvided) {
            originalReadTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);
        }
        readSourceGuard.emplace([opCtx, originalReadSource, originalReadTimestamp] {
            if (originalReadSource == RecoveryUnit::ReadSource::kProvided) {
                opCtx->recoveryUnit()->setTimestampReadSource(originalReadSource,
                                                              originalReadTimestamp);
            } else {
                opCtx->recoveryUnit()->setTimestampReadSource(originalReadSource);
            }
        });
        // Sets up operation context with majority read snapshot so correct optime can be retrieved.
        opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kMajorityCommitted);
        Status status = opCtx->recoveryUnit()->majorityCommittedSnapshotAvailable();
        if (!status.isOK()) {
            return status;
        }

        // Waits for any writes performed by this ShardLocal instance to be committed and visible.
        Status readConcernStatus = replCoord->waitUntilOpTimeForRead(
            opCtx, repl::ReadConcernArgs{_getLastOpTime(), readConcernLevel});
        if (!readConcernStatus.isOK()) {
            return readConcernStatus;
        }

        // Informs the storage engine to read from the committed snapshot for the rest of this
        // operation.
        status = opCtx->recoveryUnit()->majorityCommittedSnapshotAvailable();
        if (!status.isOK()) {
            return status;
        }
    } else {
        invariant(readConcernLevel == repl::ReadConcernLevel::kLocalReadConcern);
    }

    DBDirectClient client(opCtx);
    FindCommandRequest findRequest{nss};
    findRequest.setFilter(query);
    if (!sort.isEmpty()) {
        findRequest.setSort(sort);
    }
    if (hint) {
        findRequest.setHint(*hint);
    }
    if (limit) {
        findRequest.setLimit(*limit);
    }

    try {
        std::unique_ptr<DBClientCursor> cursor = client.find(std::move(findRequest), readPref);

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

Status RSLocalClient::runAggregation(
    OperationContext* opCtx,
    const AggregateCommandRequest& aggRequest,
    std::function<bool(const std::vector<BSONObj>& batch,
                       const boost::optional<BSONObj>& postBatchResumeToken)> callback) {
    DBDirectClient client(opCtx);
    auto cursor = uassertStatusOKWithContext(
        DBClientCursor::fromAggregationRequest(
            &client, aggRequest, true /* secondaryOk */, true /* useExhaust */),
        "Failed to establish a cursor for aggregation");

    while (cursor->more()) {
        std::vector<BSONObj> batchDocs;
        batchDocs.reserve(cursor->objsLeftInBatch());
        while (cursor->moreInCurrentBatch()) {
            batchDocs.emplace_back(cursor->nextSafe().getOwned());
        }

        try {
            if (!callback(batchDocs, boost::none)) {
                break;
            }
        } catch (const DBException& ex) {
            return ex
                .toStatus(str::stream()
                          << "Exception while running aggregation retrieval of results callback");
        }
    }

    return Status::OK();
}

}  // namespace mongo
