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

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/find_and_modify_common.h"
#include "mongo/db/commands/update_metrics.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/document_shard_key_update_util.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/s/write_ops/cluster_write.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference(ReadPreference::PrimaryOnly);
const char kRuntimeConstantsField[] = "runtimeConstants";

BSONObj appendRuntimeConstantsToCommandObject(OperationContext* opCtx, const BSONObj& origCmdObj) {
    uassert(51196,
            "Cannot specify runtime constants option to a mongos",
            !origCmdObj.getField(kRuntimeConstantsField));
    auto rtcBSON =
        BSON(kRuntimeConstantsField << Variables::generateRuntimeConstants(opCtx).toBSON());
    return origCmdObj.addField(rtcBSON.getField(kRuntimeConstantsField));
}

BSONObj getCollation(const BSONObj& cmdObj) {
    BSONElement collationElement;
    auto status = bsonExtractTypedField(cmdObj, "collation", BSONType::Object, &collationElement);
    if (status.isOK()) {
        return collationElement.Obj();
    } else if (status != ErrorCodes::NoSuchKey) {
        uassertStatusOK(status);
    }

    return BSONObj();
}

BSONObj getShardKey(OperationContext* opCtx, const ChunkManager& chunkMgr, const BSONObj& query) {
    BSONObj shardKey =
        uassertStatusOK(chunkMgr.getShardKeyPattern().extractShardKeyFromQuery(opCtx, query));
    uassert(ErrorCodes::ShardKeyNotFound,
            "Query for sharded findAndModify must contain the shard key",
            !shardKey.isEmpty());
    return shardKey;
}

void updateShardKeyValueOnWouldChangeOwningShardError(OperationContext* opCtx,
                                                      const NamespaceString nss,
                                                      Status responseStatus,
                                                      const BSONObj& cmdObj,
                                                      BSONObjBuilder* result) {
    BSONObjBuilder extraInfoBuilder;
    responseStatus.extraInfo()->serialize(&extraInfoBuilder);
    auto extraInfo = extraInfoBuilder.obj();
    auto wouldChangeOwningShardExtraInfo =
        WouldChangeOwningShardInfo::parseFromCommandError(extraInfo);

    try {
        auto matchedDocOrUpserted = documentShardKeyUpdateUtil::updateShardKeyForDocument(
            opCtx, nss, wouldChangeOwningShardExtraInfo, cmdObj.getIntField("stmtId"));
        auto upserted = matchedDocOrUpserted && wouldChangeOwningShardExtraInfo.getShouldUpsert();
        auto updatedExistingDocument = matchedDocOrUpserted && !upserted;

        BSONObjBuilder lastErrorObjBuilder(result->subobjStart("lastErrorObject"));
        lastErrorObjBuilder.appendNumber("n", matchedDocOrUpserted ? 1 : 0);
        lastErrorObjBuilder.appendBool("updatedExisting", updatedExistingDocument);
        if (upserted) {
            lastErrorObjBuilder.appendAs(wouldChangeOwningShardExtraInfo.getPostImage()["_id"],
                                         "upserted");
        }
        lastErrorObjBuilder.doneFast();

        auto shouldReturnPostImage = cmdObj.getBoolField("new");
        if (updatedExistingDocument) {
            result->append("value",
                           shouldReturnPostImage ? wouldChangeOwningShardExtraInfo.getPostImage()
                                                 : wouldChangeOwningShardExtraInfo.getPreImage());
        } else if (upserted && shouldReturnPostImage) {
            result->append("value", wouldChangeOwningShardExtraInfo.getPostImage());
        } else {
            result->appendNull("value");
        }
        result->append("ok", 1.0);
    } catch (DBException& e) {
        if (e.code() == ErrorCodes::DuplicateKey &&
            e.extraInfo<DuplicateKeyErrorInfo>()->getKeyPattern().hasField("_id")) {
            e.addContext(documentShardKeyUpdateUtil::kDuplicateKeyErrorContext);
        }
        e.addContext("findAndModify");
        throw;
    }
}

class FindAndModifyCmd : public BasicCommand {
public:
    FindAndModifyCmd()
        : BasicCommand("findAndModify", "findandmodify"), _updateMetrics{"findAndModify"} {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool supportsReadConcern(const std::string& dbName,
                             const BSONObj& cmdObj,
                             repl::ReadConcernLevel level) const final {
        return true;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        find_and_modify::addPrivilegesRequiredForFindAndModify(this, dbname, cmdObj, out);
    }

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& request,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) const override {
        std::string dbName = request.getDatabase().toString();
        const BSONObj& cmdObj = request.body;
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

        std::shared_ptr<ChunkManager> chunkMgr;
        std::shared_ptr<Shard> shard;

        if (!routingInfo.cm()) {
            shard = routingInfo.db().primary();
        } else {
            chunkMgr = routingInfo.cm();

            const BSONObj query = cmdObj.getObjectField("query");
            const BSONObj collation = getCollation(cmdObj);
            const BSONObj shardKey = getShardKey(opCtx, *chunkMgr, query);
            const auto chunk = chunkMgr->findIntersectingChunk(shardKey, collation);

            shard = uassertStatusOK(
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, chunk.getShardId()));
        }

        const auto explainCmd = ClusterExplain::wrapAsExplain(
            appendRuntimeConstantsToCommandObject(opCtx, cmdObj), verbosity);

        // Time how long it takes to run the explain command on the shard.
        Timer timer;
        BSONObjBuilder bob;
        _runCommand(opCtx,
                    shard->getId(),
                    (chunkMgr ? chunkMgr->getVersion(shard->getId()) : ChunkVersion::UNSHARDED()),
                    nss,
                    explainCmd,
                    &bob);
        const auto millisElapsed = timer.millis();

        Strategy::CommandResult cmdResult;
        cmdResult.shardTargetId = shard->getId();
        cmdResult.target = shard->getConnString();
        cmdResult.result = bob.obj();

        std::vector<Strategy::CommandResult> shardResults;
        shardResults.push_back(cmdResult);

        auto bodyBuilder = result->getBodyBuilder();
        return ClusterExplain::buildExplainResult(
            opCtx, shardResults, ClusterExplain::kSingleShard, millisElapsed, &bodyBuilder);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));

        // Collect metrics.
        _updateMetrics.collectMetrics(cmdObj);

        // findAndModify should only be creating database if upsert is true, but this would require
        // that the parsing be pulled into this function.
        createShardDatabase(opCtx, nss.db());

        // Append mongoS' runtime constants to the command object before forwarding it to the shard.
        auto cmdObjForShard = appendRuntimeConstantsToCommandObject(opCtx, cmdObj);

        const auto routingInfo = uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));
        if (!routingInfo.cm()) {
            _runCommand(opCtx,
                        routingInfo.db().primaryId(),
                        ChunkVersion::UNSHARDED(),
                        nss,
                        cmdObjForShard,
                        &result);
            return true;
        }

        const auto chunkMgr = routingInfo.cm();

        const BSONObj query = cmdObjForShard.getObjectField("query");
        const BSONObj collation = getCollation(cmdObjForShard);
        const BSONObj shardKey = getShardKey(opCtx, *chunkMgr, query);

        // For now, set bypassIsFieldHashedCheck to be true in order to skip the
        // isFieldHashedCheck in the special case where _id is hashed and used as the shard key.
        // This means that we always assume that a findAndModify request using _id is targetable
        // to a single shard.
        auto chunk = chunkMgr->findIntersectingChunk(shardKey, collation, true);

        _runCommand(opCtx,
                    chunk.getShardId(),
                    chunkMgr->getVersion(chunk.getShardId()),
                    nss,
                    cmdObjForShard,
                    &result);

        return true;
    }

private:
    static void _runCommand(OperationContext* opCtx,
                            const ShardId& shardId,
                            const ChunkVersion& shardVersion,
                            const NamespaceString& nss,
                            const BSONObj& cmdObj,
                            BSONObjBuilder* result) {
        bool isRetryableWrite = opCtx->getTxnNumber() && !TransactionRouter::get(opCtx);
        const auto response = [&] {
            std::vector<AsyncRequestsSender::Request> requests;
            requests.emplace_back(
                shardId,
                appendShardVersion(CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
                                   shardVersion));

            MultiStatementTransactionRequestsSender ars(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                nss.db().toString(),
                requests,
                kPrimaryOnlyReadPreference,
                isRetryableWrite ? Shard::RetryPolicy::kIdempotent : Shard::RetryPolicy::kNoRetry);

            auto response = ars.next();
            invariant(ars.done());

            return uassertStatusOK(std::move(response.swResponse));
        }();

        uassertStatusOK(response.status);

        const auto responseStatus = getStatusFromCommandResult(response.data);
        if (ErrorCodes::isNeedRetargettingError(responseStatus.code()) ||
            ErrorCodes::isSnapshotError(responseStatus.code())) {
            // Command code traps this exception and re-runs
            uassertStatusOK(responseStatus.withContext("findAndModify"));
        }

        if (responseStatus.code() == ErrorCodes::WouldChangeOwningShard) {
            if (isRetryableWrite) {
                RouterOperationContextSession routerSession(opCtx);
                try {
                    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
                    readConcernArgs =
                        repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

                    // Re-run the findAndModify command that will change the shard key value in a
                    // transaction. We call _runCommand recursively, and this second time through
                    // since it will be run as a transaction it will take the other code path to
                    // updateShardKeyValueOnWouldChangeOwningShardError.  We ensure the retried
                    // operation does not include WC inside the transaction by stripping it from the
                    // cmdObj.  The transaction commit will still use the WC, because it uses the WC
                    // from the opCtx (which has been set previously in Strategy).
                    documentShardKeyUpdateUtil::startTransactionForShardKeyUpdate(opCtx);
                    _runCommand(
                        opCtx, shardId, shardVersion, nss, stripWriteConcern(cmdObj), result);
                    uassertStatusOK(getStatusFromCommandResult(result->asTempObj()));

                    auto commitResponse =
                        documentShardKeyUpdateUtil::commitShardKeyUpdateTransaction(opCtx);

                    uassertStatusOK(getStatusFromCommandResult(commitResponse));
                    if (auto wcErrorElem = commitResponse["writeConcernError"]) {
                        appendWriteConcernErrorToCmdResponse(shardId, wcErrorElem, *result);
                    }
                } catch (DBException& e) {
                    if (e.code() != ErrorCodes::DuplicateKey ||
                        (e.code() == ErrorCodes::DuplicateKey &&
                         !e.extraInfo<DuplicateKeyErrorInfo>()->getKeyPattern().hasField("_id"))) {
                        e.addContext(documentShardKeyUpdateUtil::kNonDuplicateKeyErrorContext);
                    };

                    auto txnRouterForAbort = TransactionRouter::get(opCtx);
                    if (txnRouterForAbort)
                        txnRouterForAbort.implicitlyAbortTransaction(opCtx, e.toStatus());

                    throw;
                }
            } else {
                updateShardKeyValueOnWouldChangeOwningShardError(
                    opCtx, nss, responseStatus, cmdObj, result);
            }

            return;
        }

        // First append the properly constructed writeConcernError. It will then be skipped in
        // appendElementsUnique.
        if (auto wcErrorElem = response.data["writeConcernError"]) {
            appendWriteConcernErrorToCmdResponse(shardId, wcErrorElem, *result);
        }

        result->appendElementsUnique(
            CommandHelpers::filterCommandReplyForPassthrough(response.data));
    }

    // Update related command execution metrics.
    UpdateMetrics _updateMetrics;
} findAndModifyCmd;

}  // namespace
}  // namespace mongo
