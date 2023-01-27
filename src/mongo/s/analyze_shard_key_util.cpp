/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/s/analyze_shard_key_util.h"

#include "mongo/client/connpool.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_aggregate.h"
#include "mongo/s/stale_shard_version_helpers.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

namespace {

/*
 * Rounds 'val' to 'n' decimal places.
 */
double round(double val, int n) {
    const double multiplier = std::pow(10.0, n);
    return std::ceil(val * multiplier) / multiplier;
}

/**
 * Runs the aggregate command 'aggRequest' locally and applies 'callbackFn' to each returned
 * document.
 */
void runAggregateTargetLocal(OperationContext* opCtx,
                             AggregateCommandRequest aggRequest,
                             std::function<void(const BSONObj&)> callbackFn) {
    DBDirectClient client(opCtx);
    auto cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
        &client, aggRequest, true /* secondaryOk */, false /* useExhaust*/));

    while (cursor->more()) {
        auto doc = cursor->next();
        callbackFn(doc);
    }
}

/**
 * Runs the aggregate command 'aggRequest' and applies 'callbackFn' to each returned document.
 * Targets the shards that the query targets and automatically retries on shard versioning errors.
 * Does not support runnning getMore commands for the aggregation.
 */
void runAggregateTargetByQuery(OperationContext* opCtx,
                               AggregateCommandRequest aggRequest,
                               std::function<void(const BSONObj&)> callbackFn) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);

    auto nss = aggRequest.getNamespace();
    bool succeeded = false;

    while (true) {
        try {
            shardVersionRetry(
                opCtx, Grid::get(opCtx)->catalogCache(), nss, "AnalyzeShardKey"_sd, [&] {
                    BSONObjBuilder responseBuilder;
                    uassertStatusOK(
                        ClusterAggregate::runAggregate(opCtx,
                                                       ClusterAggregate::Namespaces{nss, nss},
                                                       aggRequest,
                                                       LiteParsedPipeline{aggRequest},
                                                       PrivilegeVector(),
                                                       &responseBuilder));
                    succeeded = true;
                    auto response = responseBuilder.obj();
                    auto firstBatch = response.firstElement()["firstBatch"].Obj();
                    BSONObjIterator it(firstBatch);

                    while (it.more()) {
                        auto doc = it.next().Obj();
                        callbackFn(doc);
                    }
                });
            return;
        } catch (const DBException& ex) {
            if (ex.toStatus() == ErrorCodes::ShardNotFound) {
                // 'callbackFn' should never trigger a ShardNotFound error. It is also incorrect
                // to retry the aggregate command after some documents have already been
                // processed.
                invariant(!succeeded);

                LOGV2(6875200,
                      "Failed to run aggregate command to analyze shard key",
                      "error"_attr = ex.toStatus());
                continue;
            }
            throw;
        }
    }
}

/**
 * Runs the aggregate command 'aggRequest' and applies 'callbackFn' to each returned document.
 * Targets all shards that own data for the collection 'nss'. Does not support runnning getMore
 * commands for the aggregation.
 */
void runAggregateTargetByNs(OperationContext* opCtx,
                            const NamespaceString& nss,
                            AggregateCommandRequest aggRequest,
                            std::function<void(const BSONObj&)> callbackFn) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);

    std::set<ShardId> shardIds;
    auto cm =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfo(opCtx, nss));
    if (cm.isSharded()) {
        cm.getAllShardIds(&shardIds);
    } else {
        shardIds.insert(cm.dbPrimary());
    }

    std::vector<AsyncRequestsSender::Request> requests;
    auto cmdObj = applyReadWriteConcern(
        opCtx, true /* appendRC */, true /* appendWC */, aggRequest.toBSON({}));
    for (const auto& shardId : shardIds) {
        requests.emplace_back(shardId, cmdObj);
    }

    auto shardResults = gatherResponses(opCtx,
                                        aggRequest.getNamespace().db(),
                                        ReadPreferenceSetting(ReadPreference::SecondaryPreferred),
                                        Shard::RetryPolicy::kIdempotent,
                                        requests);

    for (const auto& shardResult : shardResults) {
        const auto shardResponse = uassertStatusOK(std::move(shardResult.swResponse));
        uassertStatusOK(shardResponse.status);
        const auto cmdResult = shardResponse.data;
        uassertStatusOK(getStatusFromCommandResult(cmdResult));

        auto firstBatch = cmdResult.firstElement()["firstBatch"].Obj();
        BSONObjIterator it(firstBatch);

        if (!it.more()) {
            continue;
        }

        auto doc = it.next().Obj();
        callbackFn(doc);
    }
}

MONGO_FAIL_POINT_DEFINE(analyzeShardKeyHangBeforeWritingLocally);
MONGO_FAIL_POINT_DEFINE(analyzeShardKeyHangBeforeWritingRemotely);

const int kMaxRetriesOnRetryableErrors = 5;

// The write concern for writes done as part of query sampling or analyzing a shard key.
const Seconds writeConcernTimeout{60};
const WriteConcernOptions kMajorityWriteConcern{
    WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, writeConcernTimeout};

/*
 * Returns true if this mongod can accept writes to the collection 'nss'. Unless the collection is
 * in the "local" database, this will only return true if this mongod is a primary (or a
 * standalone).
 */
bool canAcceptWrites(OperationContext* opCtx, const NamespaceString& nss) {
    ShouldNotConflictWithSecondaryBatchApplicationBlock noPBWMBlock(opCtx->lockState());
    Lock::DBLock lk(opCtx, nss.dbName(), MODE_IS);
    Lock::CollectionLock lock(opCtx, nss, MODE_IS);
    return mongo::repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx,
                                                                                       nss.db());
}

/*
 * Runs the write command 'cmdObj' against the database 'dbName' locally, asserts that the
 * top-level command is OK, then asserts the write status using the 'uassertWriteStatusFn' callback.
 * Returns the command response.
 */
BSONObj executeWriteCommandLocal(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj,
                                 const std::function<void(const BSONObj&)>& uassertWriteStatusFn) {
    DBDirectClient client(opCtx);
    BSONObj resObj;

    if (!client.runCommand(dbName, cmdObj, resObj)) {
        uassertStatusOK(getStatusFromCommandResult(resObj));
    }
    uassertWriteStatusFn(resObj);

    return resObj;
}

/*
 * Runs the write command 'cmdObj' against the database 'dbName' on the (remote) primary, asserts
 * that the top-level command is OK, then asserts the write status using the given
 * 'uassertWriteStatusFn' callback. Throws a PrimarySteppedDown error if no primary is found.
 * Returns the command response.
 */
BSONObj executeWriteCommandRemote(OperationContext* opCtx,
                                  const DatabaseName& dbName,
                                  const BSONObj& cmdObj,
                                  const std::function<void(const BSONObj&)>& uassertWriteStatusFn) {
    auto hostAndPort = repl::ReplicationCoordinator::get(opCtx)->getCurrentPrimaryHostAndPort();

    if (hostAndPort.empty()) {
        uasserted(ErrorCodes::PrimarySteppedDown, "No primary exists currently");
    }

    auto conn = std::make_unique<ScopedDbConnection>(hostAndPort.toString());

    if (auth::isInternalAuthSet()) {
        uassertStatusOK(conn->get()->authenticateInternalUser());
    }

    DBClientBase* client = conn->get();
    ScopeGuard guard([&] { conn->done(); });
    try {
        BSONObj resObj;

        if (!client->runCommand(dbName, cmdObj, resObj)) {
            uassertStatusOK(getStatusFromCommandResult(resObj));
        }
        uassertWriteStatusFn(resObj);

        return resObj;
    } catch (...) {
        guard.dismiss();
        conn->kill();
        throw;
    }
}

/*
 * Runs the write command 'cmdObj' against the collection 'nss'. If this mongod is currently the
 * primary, runs the write command locally. Otherwise, runs the command on the remote primary.
 * Internally asserts that the top-level command is OK, then asserts the write status using the
 * given 'uassertWriteStatusFn' callback. Internally retries the write command on retryable
 * errors (for kMaxRetriesOnRetryableErrors times) so the writes must be idempotent. Returns the
 * command response.
 */
BSONObj executeWriteCommand(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const BSONObj& cmdObj,
                            const std::function<void(const BSONObj&)>& uassertWriteStatusFn) {
    const auto dbName = nss.dbName();
    auto numRetries = 0;

    while (true) {
        try {
            if (canAcceptWrites(opCtx, nss)) {
                // There is a window here where this mongod may step down after check above. In this
                // case, a NotWritablePrimary error would be thrown. However, this is preferable to
                // running the command while holding locks.
                analyzeShardKeyHangBeforeWritingLocally.pauseWhileSet(opCtx);
                return executeWriteCommandLocal(opCtx, dbName, cmdObj, uassertWriteStatusFn);
            }

            analyzeShardKeyHangBeforeWritingRemotely.pauseWhileSet(opCtx);
            return executeWriteCommandRemote(opCtx, dbName, cmdObj, uassertWriteStatusFn);
        } catch (DBException& ex) {
            if (ErrorCodes::isRetriableError(ex) && numRetries < kMaxRetriesOnRetryableErrors) {
                numRetries++;
                continue;
            }
            throw;
        }
    }

    return {};
}

}  // namespace

double calculatePercentage(double part, double whole) {
    invariant(part >= 0);
    invariant(whole > 0);
    invariant(part <= whole);
    return round(part / whole * 100, 2);
}

void runAggregate(OperationContext* opCtx,
                  AggregateCommandRequest aggRequest,
                  std::function<void(const BSONObj&)> callbackFn) {
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        return runAggregateTargetByQuery(opCtx, aggRequest, callbackFn);
    }
    return runAggregateTargetLocal(opCtx, aggRequest, callbackFn);
}

void runAggregate(OperationContext* opCtx,
                  const NamespaceString& nss,
                  AggregateCommandRequest aggRequest,
                  std::function<void(const BSONObj&)> callbackFn) {
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        return runAggregateTargetByNs(opCtx, nss, aggRequest, callbackFn);
    }
    return runAggregateTargetLocal(opCtx, aggRequest, callbackFn);
}

void insertDocuments(OperationContext* opCtx,
                     const NamespaceString& nss,
                     const std::vector<BSONObj>& docs,
                     const std::function<void(const BSONObj&)>& uassertWriteStatusFn) {
    write_ops::InsertCommandRequest insertCmd(nss);
    insertCmd.setDocuments(docs);
    insertCmd.setWriteCommandRequestBase([&] {
        write_ops::WriteCommandRequestBase wcb;
        wcb.setOrdered(false);
        wcb.setBypassDocumentValidation(false);
        return wcb;
    }());
    auto insertCmdObj = insertCmd.toBSON(
        {BSON(WriteConcernOptions::kWriteConcernField << kMajorityWriteConcern.toBSON())});

    executeWriteCommand(opCtx, nss, std::move(insertCmdObj), [&](const BSONObj& resObj) {
        uassertWriteStatusFn(resObj);
    });
}

void dropCollection(OperationContext* opCtx, const NamespaceString& nss) {
    auto dropCollectionCmdObj =
        BSON("drop" << nss.coll().toString() << WriteConcernOptions::kWriteConcernField
                    << kMajorityWriteConcern.toBSON());
    executeWriteCommand(opCtx, nss, std::move(dropCollectionCmdObj), [&](const BSONObj& resObj) {
        BatchedCommandResponse res;
        std::string errMsg;

        if (!res.parseBSON(resObj, &errMsg)) {
            uasserted(ErrorCodes::FailedToParse, errMsg);
        }

        auto status = res.toStatus();
        if (status == ErrorCodes::NamespaceNotFound) {
            return;
        }
        uassertStatusOK(status);
    });
}

}  // namespace analyze_shard_key
}  // namespace mongo
