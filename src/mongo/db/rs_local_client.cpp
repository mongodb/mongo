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

#include <boost/cstdint.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/rs_local_client.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

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
                                                                 const DatabaseName& dbName,
                                                                 const BSONObj& cmdObj) {
    const auto currentOpTimeFromClient =
        repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    ON_BLOCK_EXIT([this, &opCtx, &currentOpTimeFromClient] {
        _updateLastOpTimeFromClient(opCtx, currentOpTimeFromClient);
    });

    try {
        DBDirectClient client(opCtx);

        rpc::UniqueReply commandResponse = client.runCommand(
            OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::get(opCtx), dbName, cmdObj));

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

    if (readConcernLevel == repl::ReadConcernLevel::kMajorityReadConcern ||
        readConcernLevel == repl::ReadConcernLevel::kSnapshotReadConcern) {
        invariant(!shard_role_details::getLocker(opCtx)->isLocked());
        invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

        // Resets to the original read source at the end of this operation.
        auto originalReadSource =
            shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource();
        boost::optional<Timestamp> originalReadTimestamp;
        if (originalReadSource == RecoveryUnit::ReadSource::kProvided) {
            originalReadTimestamp =
                shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp(opCtx);
        }
        readSourceGuard.emplace([opCtx, originalReadSource, originalReadTimestamp] {
            if (originalReadSource == RecoveryUnit::ReadSource::kProvided) {
                shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
                    originalReadSource, originalReadTimestamp);
            } else {
                shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
                    originalReadSource);
            }
        });
        // Sets up operation context with majority read snapshot so correct optime can be
        // retrieved.
        shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
            RecoveryUnit::ReadSource::kMajorityCommitted);
        Status status =
            shard_role_details::getRecoveryUnit(opCtx)->majorityCommittedSnapshotAvailable();
        if (!status.isOK()) {
            return status;
        }

        // Waits for any writes performed by this ShardLocal instance to be committed and
        // visible. We hardcode majority here even if using snaphsot as both operations will do the
        // initial snapshot at the majority timestamp.
        Status readConcernStatus = replCoord->waitUntilOpTimeForRead(
            opCtx,
            repl::ReadConcernArgs{_getLastOpTime(), repl::ReadConcernLevel::kMajorityReadConcern});
        if (!readConcernStatus.isOK()) {
            return readConcernStatus;
        }

        status = shard_role_details::getRecoveryUnit(opCtx)->majorityCommittedSnapshotAvailable();
        if (!status.isOK()) {
            return status;
        }
        if (readConcernLevel == repl::ReadConcernLevel::kSnapshotReadConcern) {
            // Snapshot readConcern starts a snapshot at the majority timestamp, acquire the
            // timestamp now and overwrite the majority readConcern used above.
            auto opTime = replCoord->getCurrentCommittedSnapshotOpTime();
            shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kProvided, opTime.getTimestamp());
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
                    str::stream() << "Failed to establish a cursor for reading "
                                  << nss.toStringForErrorMsg() << " from local storage"};
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
    /* We use DBDirectClient to read locally, which uses the readSource/readTimestamp set on the
     * opCtx rather than applying the readConcern speficied in the command. This is not
     * consistent with any remote client. We extract the readConcern from the request and apply
     * it to the opCtx's readSource/readTimestamp. Leave as it was originally before returning*/

    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    // extracting readConcern
    repl::ReadConcernArgs requestReadConcernArgs;
    if (!aggRequest.getReadConcern()) {
        requestReadConcernArgs =
            repl::ReadConcernArgs{_getLastOpTime(), repl::ReadConcernLevel::kLocalReadConcern};
    } else {
        // initialize read concern args
        auto readConcernParseStatus = requestReadConcernArgs.parse(*aggRequest.getReadConcern());
        if (!readConcernParseStatus.isOK()) {
            return readConcernParseStatus;
        }

        // if after cluster time is set, change it with lastOp time if this comes later
        if (requestReadConcernArgs.getArgsAfterClusterTime()) {
            auto afterClusterTime = *requestReadConcernArgs.getArgsAfterClusterTime();
            if (afterClusterTime.asTimestamp() < _getLastOpTime().getTimestamp())
                requestReadConcernArgs =
                    repl::ReadConcernArgs{_getLastOpTime(), requestReadConcernArgs.getLevel()};
        }
    }
    // saving original read source and read concern
    auto originalRCA = repl::ReadConcernArgs::get(opCtx);
    auto originalReadSource = shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource();
    boost::optional<Timestamp> originalReadTimestamp;
    if (originalReadSource == RecoveryUnit::ReadSource::kProvided)
        originalReadTimestamp =
            shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp(opCtx);

    ON_BLOCK_EXIT([&]() {
        repl::ReadConcernArgs::get(opCtx) = originalRCA;
        if (originalReadSource == RecoveryUnit::ReadSource::kProvided) {
            shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
                originalReadSource, originalReadTimestamp);
        } else {
            shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(originalReadSource);
        }
    });

    // Waits for any writes performed by this ShardLocal instance to be committed and
    // visible. This will set the correct ReadSource as well
    repl::ReadConcernArgs::get(opCtx) = requestReadConcernArgs;
    Status rcStatus = mongo::waitForReadConcern(
        opCtx, requestReadConcernArgs, aggRequest.getNamespace().dbName(), true);

    if (!rcStatus.isOK())
        return rcStatus;

    // run aggregation
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
