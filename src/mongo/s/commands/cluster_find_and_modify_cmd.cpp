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

#include "mongo/s/commands/cluster_find_and_modify_cmd.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/update_metrics.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/document_shard_key_update_util.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/shard_key_pattern_query_util.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

using QuerySamplingOptions = OperationContext::QuerySamplingOptions;

const ReadPreferenceSetting kPrimaryOnlyReadPreference(ReadPreference::PrimaryOnly);
const char kLegacyRuntimeConstantsField[] = "runtimeConstants";

BSONObj appendLegacyRuntimeConstantsToCommandObject(OperationContext* opCtx,
                                                    const BSONObj& origCmdObj) {
    uassert(51196,
            "Cannot specify runtime constants option to a mongos",
            !origCmdObj.getField(kLegacyRuntimeConstantsField));
    auto rtcBSON =
        BSON(kLegacyRuntimeConstantsField << Variables::generateRuntimeConstants(opCtx).toBSON());
    return origCmdObj.addField(rtcBSON.getField(kLegacyRuntimeConstantsField));
}

BSONObj stripWriteConcern(const BSONObj& cmdObj) {
    BSONObjBuilder output;
    for (const auto& elem : cmdObj) {
        const auto name = elem.fieldNameStringData();
        if (name == WriteConcernOptions::kWriteConcernField) {
            continue;
        }
        output.append(elem);
    }
    return output.obj();
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

boost::optional<BSONObj> getLet(const BSONObj& cmdObj) {
    if (auto letElem = cmdObj.getField("let"_sd); letElem.type() == BSONType::Object) {
        auto bob = BSONObjBuilder();
        bob.appendElementsUnique(letElem.embeddedObject());
        return bob.obj();
    }
    return boost::none;
}

boost::optional<LegacyRuntimeConstants> getLegacyRuntimeConstants(const BSONObj& cmdObj) {
    if (auto rcElem = cmdObj.getField("runtimeConstants"_sd); rcElem.type() == BSONType::Object) {
        IDLParserContext ctx("internalLegacyRuntimeConstants");
        return LegacyRuntimeConstants::parse(ctx, rcElem.embeddedObject());
    }
    return boost::none;
}

BSONObj getShardKey(OperationContext* opCtx,
                    const ChunkManager& chunkMgr,
                    const NamespaceString& nss,
                    const BSONObj& query,
                    const BSONObj& collation,
                    const boost::optional<ExplainOptions::Verbosity> verbosity,
                    const boost::optional<BSONObj>& let,
                    const boost::optional<LegacyRuntimeConstants>& runtimeConstants) {
    auto expCtx = makeExpressionContextWithDefaultsForTargeter(
        opCtx, nss, collation, verbosity, let, runtimeConstants);

    BSONObj shardKey = uassertStatusOK(
        extractShardKeyFromBasicQueryWithContext(expCtx, chunkMgr.getShardKeyPattern(), query));
    uassert(ErrorCodes::ShardKeyNotFound,
            "Query for sharded findAndModify must contain the shard key",
            !shardKey.isEmpty());
    return shardKey;
}

void handleWouldChangeOwningShardErrorNonTransaction(
    OperationContext* opCtx,
    const ShardId& shardId,
    const NamespaceString& nss,
    const write_ops::FindAndModifyCommandRequest& request,
    BSONObjBuilder* result) {
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor());

    auto txn = txn_api::SyncTransactionWithRetries(
        opCtx, sleepInlineExecutor, nullptr /* resourceYielder */, inlineExecutor);

    // Shared state for the transaction API use below.
    struct SharedBlock {
        SharedBlock(NamespaceString nss_) : nss(nss_) {}

        NamespaceString nss;
        BSONObj response;
    };
    auto sharedBlock = std::make_shared<SharedBlock>(nss);

    auto swCommitResult = txn.runNoThrow(
        opCtx,
        [cmdObj = request.toBSON({}), sharedBlock](const txn_api::TransactionClient& txnClient,
                                                   ExecutorPtr txnExec) {
            return txnClient.runCommand(sharedBlock->nss.dbName(), cmdObj)
                .thenRunOn(txnExec)
                .then([sharedBlock](auto res) {
                    uassertStatusOK(getStatusFromCommandResult(res));

                    sharedBlock->response = CommandHelpers::filterCommandReplyForPassthrough(
                        res.removeField("recoveryToken"));
                })
                .semi();
        });

    result->appendElementsUnique(
        CommandHelpers::filterCommandReplyForPassthrough(sharedBlock->response));

    auto bodyStatus = swCommitResult.getStatus();
    if (bodyStatus != ErrorCodes::DuplicateKey ||
        (bodyStatus == ErrorCodes::DuplicateKey &&
         !bodyStatus.extraInfo<DuplicateKeyErrorInfo>()->getKeyPattern().hasField("_id"))) {
        bodyStatus.addContext(documentShardKeyUpdateUtil::kNonDuplicateKeyErrorContext);
    }
    uassertStatusOK(bodyStatus);

    uassertStatusOK(swCommitResult.getValue().cmdStatus);
    const auto& wcError = swCommitResult.getValue().wcError;
    if (!wcError.toStatus().isOK()) {
        appendWriteConcernErrorDetailToCmdResponse(shardId, wcError, *result);
    }
}

void updateReplyOnWouldChangeOwningShardSuccess(bool matchedDocOrUpserted,
                                                const WouldChangeOwningShardInfo& changeInfo,
                                                bool shouldReturnPostImage,
                                                BSONObjBuilder* result) {
    auto upserted = matchedDocOrUpserted && changeInfo.getShouldUpsert();
    auto updatedExistingDocument = matchedDocOrUpserted && !upserted;

    BSONObjBuilder lastErrorObjBuilder(result->subobjStart("lastErrorObject"));
    lastErrorObjBuilder.appendNumber("n", matchedDocOrUpserted ? 1 : 0);
    lastErrorObjBuilder.appendBool("updatedExisting", updatedExistingDocument);
    if (upserted) {
        lastErrorObjBuilder.appendAs(changeInfo.getPostImage()["_id"], "upserted");
    }
    lastErrorObjBuilder.doneFast();

    if (updatedExistingDocument) {
        result->append(
            "value", shouldReturnPostImage ? changeInfo.getPostImage() : changeInfo.getPreImage());
    } else if (upserted && shouldReturnPostImage) {
        result->append("value", changeInfo.getPostImage());
    } else {
        result->appendNull("value");
    }
    result->append("ok", 1.0);
}

void handleWouldChangeOwningShardErrorTransaction(
    OperationContext* opCtx,
    const NamespaceString nss,
    Status responseStatus,
    const write_ops::FindAndModifyCommandRequest& request,
    BSONObjBuilder* result,
    bool fleCrudProcessed) {

    BSONObjBuilder extraInfoBuilder;
    responseStatus.extraInfo()->serialize(&extraInfoBuilder);
    auto extraInfo = extraInfoBuilder.obj();

    // Shared state for the transaction API use below.
    struct SharedBlock {
        SharedBlock(WouldChangeOwningShardInfo changeInfo_, NamespaceString nss_)
            : changeInfo(changeInfo_), nss(nss_) {}

        WouldChangeOwningShardInfo changeInfo;
        NamespaceString nss;
        bool matchedDocOrUpserted{false};
    };
    auto sharedBlock = std::make_shared<SharedBlock>(
        WouldChangeOwningShardInfo::parseFromCommandError(extraInfo), nss);

    try {
        auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
        auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
        auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);

        auto txn = txn_api::SyncTransactionWithRetries(
            opCtx,
            sleepInlineExecutor,
            TransactionRouterResourceYielder::makeForLocalHandoff(),
            inlineExecutor);

        txn.run(opCtx,
                [sharedBlock, fleCrudProcessed](const txn_api::TransactionClient& txnClient,
                                                ExecutorPtr txnExec) -> SemiFuture<void> {
                    return documentShardKeyUpdateUtil::updateShardKeyForDocument(
                               txnClient,
                               txnExec,
                               sharedBlock->nss,
                               sharedBlock->changeInfo,
                               fleCrudProcessed)
                        .thenRunOn(txnExec)
                        .then([sharedBlock](bool matchedDocOrUpserted) {
                            sharedBlock->matchedDocOrUpserted = matchedDocOrUpserted;
                        })
                        .semi();
                });

        auto shouldReturnPostImage = request.getNew() && *request.getNew();
        updateReplyOnWouldChangeOwningShardSuccess(sharedBlock->matchedDocOrUpserted,
                                                   sharedBlock->changeInfo,
                                                   shouldReturnPostImage,
                                                   result);
    } catch (DBException& e) {
        if (e.code() == ErrorCodes::DuplicateKey &&
            e.extraInfo<DuplicateKeyErrorInfo>()->getKeyPattern().hasField("_id")) {
            e.addContext(documentShardKeyUpdateUtil::kDuplicateKeyErrorContext);
        }
        e.addContext("findAndModify");
        throw;
    }
}

void handleWouldChangeOwningShardErrorTransactionLegacy(OperationContext* opCtx,
                                                        const NamespaceString nss,
                                                        Status responseStatus,
                                                        const BSONObj& cmdObj,
                                                        BSONObjBuilder* result,
                                                        bool fleCrudProcessed) {
    BSONObjBuilder extraInfoBuilder;
    responseStatus.extraInfo()->serialize(&extraInfoBuilder);
    auto extraInfo = extraInfoBuilder.obj();
    auto wouldChangeOwningShardExtraInfo =
        WouldChangeOwningShardInfo::parseFromCommandError(extraInfo);

    try {
        auto matchedDocOrUpserted = documentShardKeyUpdateUtil::updateShardKeyForDocumentLegacy(
            opCtx, nss, wouldChangeOwningShardExtraInfo, fleCrudProcessed);

        auto shouldReturnPostImage = cmdObj.getBoolField("new");
        updateReplyOnWouldChangeOwningShardSuccess(
            matchedDocOrUpserted, wouldChangeOwningShardExtraInfo, shouldReturnPostImage, result);
    } catch (DBException& e) {
        if (e.code() == ErrorCodes::DuplicateKey &&
            e.extraInfo<DuplicateKeyErrorInfo>()->getKeyPattern().hasField("_id")) {
            e.addContext(documentShardKeyUpdateUtil::kDuplicateKeyErrorContext);
        }
        e.addContext("findAndModify");
        throw;
    }
}

BSONObj prepareCmdObjForPassthrough(
    OperationContext* opCtx,
    const BSONObj& cmdObj,
    const NamespaceString& nss,
    bool isExplain,
    const boost::optional<DatabaseVersion>& dbVersion,
    const boost::optional<ShardVersion>& shardVersion,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery) {
    BSONObj filteredCmdObj = CommandHelpers::filterCommandRequestForPassthrough(cmdObj);
    if (!isExplain) {
        if (auto sampleId = analyze_shard_key::tryGenerateSampleId(
                opCtx, nss, cmdObj.firstElementFieldNameStringData())) {
            filteredCmdObj =
                analyze_shard_key::appendSampleId(std::move(filteredCmdObj), std::move(*sampleId));
        }
    }

    BSONObj newCmdObj(std::move(filteredCmdObj));
    if (dbVersion) {
        newCmdObj = appendDbVersionIfPresent(newCmdObj, *dbVersion);
    }
    if (shardVersion) {
        newCmdObj = appendShardVersion(newCmdObj, *shardVersion);
    }

    if (opCtx->isRetryableWrite()) {
        if (!newCmdObj.hasField(write_ops::WriteCommandRequestBase::kStmtIdFieldName)) {
            BSONObjBuilder bob(newCmdObj);
            bob.append(write_ops::WriteCommandRequestBase::kStmtIdFieldName, 0);
            newCmdObj = bob.obj();
        }
    }

    uassert(ErrorCodes::InvalidOptions,
            "$_allowShardKeyUpdatesWithoutFullShardKeyInQuery is an internal parameter",
            !newCmdObj.hasField(write_ops::FindAndModifyCommandRequest::
                                    kAllowShardKeyUpdatesWithoutFullShardKeyInQueryFieldName));
    if (allowShardKeyUpdatesWithoutFullShardKeyInQuery.has_value()) {
        BSONObjBuilder bob(newCmdObj);
        bob.appendBool(write_ops::FindAndModifyCommandRequest::
                           kAllowShardKeyUpdatesWithoutFullShardKeyInQueryFieldName,
                       *allowShardKeyUpdatesWithoutFullShardKeyInQuery);
        newCmdObj = bob.obj();
    }
    return newCmdObj;
}

FindAndModifyCmd findAndModifyCmd;

}  // namespace

Status FindAndModifyCmd::checkAuthForOperation(OperationContext* opCtx,
                                               const DatabaseName& dbName,
                                               const BSONObj& cmdObj) const {
    const bool update = cmdObj["update"].trueValue();
    const bool upsert = cmdObj["upsert"].trueValue();
    const bool remove = cmdObj["remove"].trueValue();

    ActionSet actions;
    actions.addAction(ActionType::find);
    if (update) {
        actions.addAction(ActionType::update);
    }
    if (upsert) {
        actions.addAction(ActionType::insert);
    }
    if (remove) {
        actions.addAction(ActionType::remove);
    }
    if (shouldBypassDocumentValidationForCommand(cmdObj)) {
        actions.addAction(ActionType::bypassDocumentValidation);
    }

    auto nss = CommandHelpers::parseNsFromCommand(dbName, cmdObj);
    ResourcePattern resource(CommandHelpers::resourcePatternForNamespace(nss.ns()));
    uassert(17137,
            "Invalid target namespace " + resource.toString(),
            resource.isExactNamespacePattern());

    auto* as = AuthorizationSession::get(opCtx->getClient());
    if (!as->isAuthorizedForActionsOnResource(resource, actions)) {
        return {ErrorCodes::Unauthorized, "unauthorized"};
    }

    return Status::OK();
}

Status FindAndModifyCmd::explain(OperationContext* opCtx,
                                 const OpMsgRequest& request,
                                 ExplainOptions::Verbosity verbosity,
                                 rpc::ReplyBuilderInterface* result) const {
    const DatabaseName dbName(request.getValidatedTenantId(), request.getDatabase());
    const BSONObj& cmdObj = [&]() {
        // Check whether the query portion needs to be rewritten for FLE.
        auto findAndModifyRequest = write_ops::FindAndModifyCommandRequest::parse(
            IDLParserContext("ClusterFindAndModify"), request.body);
        if (shouldDoFLERewrite(findAndModifyRequest)) {
            CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation = true;

            auto newRequest = processFLEFindAndModifyExplainMongos(opCtx, findAndModifyRequest);
            return newRequest.first.toBSON(request.body);
        } else {
            return request.body;
        }
    }();
    const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));

    const auto cri =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
    const auto& cm = cri.cm;

    std::shared_ptr<Shard> shard;
    if (cm.isSharded()) {
        const BSONObj query = cmdObj.getObjectField("query");
        const BSONObj collation = getCollation(cmdObj);
        const auto let = getLet(cmdObj);
        const auto rc = getLegacyRuntimeConstants(cmdObj);
        const BSONObj shardKey = getShardKey(opCtx, cm, nss, query, collation, verbosity, let, rc);
        const auto chunk = cm.findIntersectingChunk(shardKey, collation);

        shard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, chunk.getShardId()));
    } else {
        shard = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, cm.dbPrimary()));
    }

    const auto explainCmd = ClusterExplain::wrapAsExplain(
        appendLegacyRuntimeConstantsToCommandObject(opCtx, cmdObj), verbosity);

    // Time how long it takes to run the explain command on the shard.
    Timer timer;
    BSONObjBuilder bob;

    if (cm.isSharded()) {
        _runCommand(opCtx,
                    shard->getId(),
                    cri.getShardVersion(shard->getId()),
                    boost::none,
                    nss,
                    applyReadWriteConcern(opCtx, false, false, explainCmd),
                    true /* isExplain */,
                    &bob);
    } else {
        _runCommand(opCtx,
                    shard->getId(),
                    boost::make_optional(!cm.dbVersion().isFixed(), ShardVersion::UNSHARDED()),
                    cm.dbVersion(),
                    nss,
                    applyReadWriteConcern(opCtx, false, false, explainCmd),
                    true /* isExplain */,
                    &bob);
    }

    const auto millisElapsed = timer.millis();

    executor::RemoteCommandResponse response(bob.obj(), Milliseconds(millisElapsed));

    // We fetch an arbitrary host from the ConnectionString, since
    // ClusterExplain::buildExplainResult() doesn't use the given HostAndPort.
    AsyncRequestsSender::Response arsResponse{
        shard->getId(), response, shard->getConnString().getServers().front()};

    auto bodyBuilder = result->getBodyBuilder();
    return ClusterExplain::buildExplainResult(
        opCtx, {arsResponse}, ClusterExplain::kSingleShard, millisElapsed, cmdObj, &bodyBuilder);
}

bool FindAndModifyCmd::run(OperationContext* opCtx,
                           const DatabaseName& dbName,
                           const BSONObj& cmdObj,
                           BSONObjBuilder& result) {
    const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));

    if (processFLEFindAndModify(opCtx, cmdObj, result) == FLEBatchResult::kProcessed) {
        return true;
    }

    // Collect metrics.
    _updateMetrics.collectMetrics(cmdObj);

    // Technically, findAndModify should only be creating database if upsert is true, but this
    // would require that the parsing be pulled into this function.
    cluster::createDatabase(opCtx, nss.db());

    // Append mongoS' runtime constants to the command object before forwarding it to the shard.
    auto cmdObjForShard = appendLegacyRuntimeConstantsToCommandObject(opCtx, cmdObj);

    const auto cri = uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));
    const auto& cm = cri.cm;
    if (cm.isSharded()) {
        const BSONObj query = cmdObjForShard.getObjectField("query");
        const bool isUpsert = cmdObjForShard.getBoolField("upsert");
        if (write_without_shard_key::useTwoPhaseProtocol(opCtx,
                                                         nss,
                                                         false /* isUpdateOrDelete */,
                                                         isUpsert,
                                                         query,
                                                         getCollation(cmdObjForShard))) {
            _runCommandWithoutShardKey(opCtx,
                                       nss,
                                       applyReadWriteConcern(opCtx, this, cmdObjForShard),
                                       false /* isExplain */,
                                       &result);
        } else {
            const BSONObj collation = getCollation(cmdObjForShard);
            const auto let = getLet(cmdObjForShard);
            const auto rc = getLegacyRuntimeConstants(cmdObjForShard);
            const BSONObj shardKey =
                getShardKey(opCtx, cm, nss, query, collation, boost::none, let, rc);

            // For now, set bypassIsFieldHashedCheck to be true in order to skip the
            // isFieldHashedCheck in the special case where _id is hashed and used as the shard
            // key. This means that we always assume that a findAndModify request using _id is
            // targetable to a single shard.
            auto chunk = cm.findIntersectingChunk(shardKey, collation, true);
            _runCommand(opCtx,
                        chunk.getShardId(),
                        cri.getShardVersion(chunk.getShardId()),
                        boost::none,
                        nss,
                        applyReadWriteConcern(opCtx, this, cmdObjForShard),
                        false /* isExplain */,
                        &result);
        }
    } else {
        _runCommand(opCtx,
                    cm.dbPrimary(),
                    boost::make_optional(!cm.dbVersion().isFixed(), ShardVersion::UNSHARDED()),
                    cm.dbVersion(),
                    nss,
                    applyReadWriteConcern(opCtx, this, cmdObjForShard),
                    false /* isExplain */,
                    &result);
    }

    return true;
}

bool FindAndModifyCmd::getCrudProcessedFromCmd(const BSONObj& cmdObj) {
    // We could have wrapped the FindAndModify command in an explain object
    const BSONObj& realCmdObj =
        cmdObj.getField("explain").ok() ? cmdObj.getObjectField("explain") : cmdObj;
    auto req = write_ops::FindAndModifyCommandRequest::parse(
        IDLParserContext("ClusterFindAndModify"), realCmdObj);

    return req.getEncryptionInformation().has_value() &&
        req.getEncryptionInformation()->getCrudProcessed().get_value_or(false);
}

// Catches errors in the given response, and reruns the command if necessary. Uses the given
// response to construct the findAndModify command result passed to the client.
void FindAndModifyCmd::_constructResult(OperationContext* opCtx,
                                        const ShardId& shardId,
                                        const boost::optional<ShardVersion>& shardVersion,
                                        const boost::optional<DatabaseVersion>& dbVersion,
                                        const NamespaceString& nss,
                                        const BSONObj& cmdObj,
                                        const Status& responseStatus,
                                        const BSONObj& response,
                                        BSONObjBuilder* result) {
    auto txnRouter = TransactionRouter::get(opCtx);
    bool isRetryableWrite = opCtx->getTxnNumber() && !txnRouter;

    if (ErrorCodes::isNeedRetargettingError(responseStatus.code()) ||
        ErrorCodes::isSnapshotError(responseStatus.code()) ||
        responseStatus.code() == ErrorCodes::StaleDbVersion) {
        // Command code traps this exception and re-runs
        uassertStatusOK(responseStatus.withContext("findAndModify"));
    }

    if (responseStatus.code() == ErrorCodes::TenantMigrationAborted) {
        uassertStatusOK(responseStatus.withContext("findAndModify"));
    }

    if (responseStatus.code() == ErrorCodes::WouldChangeOwningShard) {
        if (feature_flags::gFeatureFlagUpdateDocumentShardKeyUsingTransactionApi.isEnabled(
                serverGlobalParams.featureCompatibility)) {
            handleWouldChangeOwningShardError(opCtx, shardId, nss, cmdObj, responseStatus, result);
        } else {
            // TODO SERVER-67429: Remove this branch.
            opCtx->setQuerySamplingOptions(QuerySamplingOptions::kOptOut);

            if (isRetryableWrite) {
                _handleWouldChangeOwningShardErrorRetryableWriteLegacy(
                    opCtx, shardId, shardVersion, dbVersion, nss, cmdObj, result);
            } else {
                handleWouldChangeOwningShardErrorTransactionLegacy(
                    opCtx, nss, responseStatus, cmdObj, result, getCrudProcessedFromCmd(cmdObj));
            }
        }

        return;
    }

    // Throw if a non-OK status is not because of any of the above errors.
    uassertStatusOK(responseStatus);

    // First append the properly constructed writeConcernError. It will then be skipped in
    // appendElementsUnique.
    if (auto wcErrorElem = response["writeConcernError"]) {
        appendWriteConcernErrorToCmdResponse(shardId, wcErrorElem, *result);
    }

    result->appendElementsUnique(CommandHelpers::filterCommandReplyForPassthrough(response));
}

// Two-phase protocol to run a findAndModify command without a shard key or _id.
void FindAndModifyCmd::_runCommandWithoutShardKey(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const BSONObj& cmdObj,
                                                  bool isExplain,
                                                  BSONObjBuilder* result) {
    auto allowShardKeyUpdatesWithoutFullShardKeyInQuery =
        opCtx->isRetryableWrite() || opCtx->inMultiDocumentTransaction();

    auto cmdObjForPassthrough =
        prepareCmdObjForPassthrough(opCtx,
                                    cmdObj,
                                    nss,
                                    isExplain,
                                    boost::none /* dbVersion */,
                                    boost::none /* shardVersion */,
                                    allowShardKeyUpdatesWithoutFullShardKeyInQuery);

    auto swRes =
        write_without_shard_key::runTwoPhaseWriteProtocol(opCtx, nss, cmdObjForPassthrough);

    // runTwoPhaseWriteProtocol returns an empty response when there are no matching documents
    // and {upsert: false}.
    BSONObj cmdResponse;
    // If runTwoPhaseWriteProtocol has a non-OK status, shardId will not be set, since we did not
    // successfully apply the operation on a shard.
    ShardId shardId;

    if (swRes.isOK()) {
        if (swRes.getValue().getResponse().isEmpty()) {
            write_ops::FindAndModifyLastError lastError(0 /* n */);
            lastError.setUpdatedExisting(false);

            write_ops::FindAndModifyCommandReply findAndModifyResponse;
            findAndModifyResponse.setLastErrorObject(std::move(lastError));
            findAndModifyResponse.setValue(boost::none);
            cmdResponse = findAndModifyResponse.toBSON();
        } else {
            cmdResponse = swRes.getValue().getResponse();
        }
        shardId = ShardId(swRes.getValue().getShardId().toString());
    }

    // Extract findAndModify command result from the result of the two phase write protocol.
    _constructResult(opCtx,
                     shardId,
                     boost::none /* shardVersion */,
                     boost::none /* dbVersion */,
                     nss,
                     cmdObj,
                     swRes.getStatus(),
                     cmdResponse,
                     result);
}

// Command invocation to be used if a shard key is specified or the collection is unsharded.
void FindAndModifyCmd::_runCommand(OperationContext* opCtx,
                                   const ShardId& shardId,
                                   const boost::optional<ShardVersion>& shardVersion,
                                   const boost::optional<DatabaseVersion>& dbVersion,
                                   const NamespaceString& nss,
                                   const BSONObj& cmdObj,
                                   bool isExplain,
                                   BSONObjBuilder* result) {
    auto txnRouter = TransactionRouter::get(opCtx);
    bool isRetryableWrite = opCtx->getTxnNumber() && !txnRouter;

    const auto response = [&] {
        std::vector<AsyncRequestsSender::Request> requests;
        auto cmdObjForPassthrough = prepareCmdObjForPassthrough(
            opCtx,
            cmdObj,
            nss,
            isExplain,
            dbVersion,
            shardVersion,
            boost::none /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */);
        requests.emplace_back(shardId, cmdObjForPassthrough);

        MultiStatementTransactionRequestsSender ars(
            opCtx,
            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
            nss.dbName(),
            requests,
            kPrimaryOnlyReadPreference,
            isRetryableWrite ? Shard::RetryPolicy::kIdempotent : Shard::RetryPolicy::kNoRetry);

        auto response = ars.next();
        invariant(ars.done());

        return uassertStatusOK(std::move(response.swResponse));
    }();

    _constructResult(opCtx,
                     shardId,
                     shardVersion,
                     dbVersion,
                     nss,
                     cmdObj,
                     getStatusFromCommandResult(response.data),
                     response.data,
                     result);
}

// TODO SERVER-67429: Remove this function.
void FindAndModifyCmd::_handleWouldChangeOwningShardErrorRetryableWriteLegacy(
    OperationContext* opCtx,
    const ShardId& shardId,
    const boost::optional<ShardVersion>& shardVersion,
    const boost::optional<DatabaseVersion>& dbVersion,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    BSONObjBuilder* result) {
    RouterOperationContextSession routerSession(opCtx);
    try {
        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        // Re-run the findAndModify command that will change the shard key value in a
        // transaction. We call _runCommand or _runCommandWithoutShardKey recursively, and this
        // second time through since it will be run as a transaction it will take the other code
        // path to handleWouldChangeOwningShardErrorTransactionLegacy.  We ensure the retried
        // operation does not include WC inside the transaction by stripping it from the
        // cmdObj.  The transaction commit will still use the WC, because it uses the WC
        // from the opCtx (which has been set previously in Strategy).
        documentShardKeyUpdateUtil::startTransactionForShardKeyUpdate(opCtx);

        if (const auto query = cmdObj.getObjectField("query");
            write_without_shard_key::useTwoPhaseProtocol(
                opCtx,
                nss,
                false /* isUpdateOrDelete */,
                cmdObj.getBoolField("upsert") /* isUpsert */,
                query,
                getCollation(cmdObj))) {
            _runCommandWithoutShardKey(
                opCtx, nss, stripWriteConcern(cmdObj), false /* isExplain */, result);
        } else {
            _runCommand(opCtx,
                        shardId,
                        shardVersion,
                        dbVersion,
                        nss,
                        stripWriteConcern(cmdObj),
                        false /* isExplain */,
                        result);
        }

        uassertStatusOK(getStatusFromCommandResult(result->asTempObj()));
        auto commitResponse = documentShardKeyUpdateUtil::commitShardKeyUpdateTransaction(opCtx);

        uassertStatusOK(getStatusFromCommandResult(commitResponse));
        if (auto wcErrorElem = commitResponse["writeConcernError"]) {
            appendWriteConcernErrorToCmdResponse(shardId, wcErrorElem, *result);
        }
    } catch (DBException& e) {
        if (e.code() != ErrorCodes::DuplicateKey ||
            (e.code() == ErrorCodes::DuplicateKey &&
             !e.extraInfo<DuplicateKeyErrorInfo>()->getKeyPattern().hasField("_id"))) {
            e.addContext(documentShardKeyUpdateUtil::kNonDuplicateKeyErrorContext);
        }

        auto txnRouterForAbort = TransactionRouter::get(opCtx);
        if (txnRouterForAbort)
            txnRouterForAbort.implicitlyAbortTransaction(opCtx, e.toStatus());

        throw;
    }
}

void FindAndModifyCmd::handleWouldChangeOwningShardError(OperationContext* opCtx,
                                                         const ShardId& shardId,
                                                         const NamespaceString& nss,
                                                         const BSONObj& cmdObj,
                                                         Status responseStatus,
                                                         BSONObjBuilder* result) {
    auto txnRouter = TransactionRouter::get(opCtx);
    bool isRetryableWrite = opCtx->getTxnNumber() && !txnRouter;

    auto parsedRequest = write_ops::FindAndModifyCommandRequest::parse(
        IDLParserContext("ClusterFindAndModify"), cmdObj);

    // Strip write concern because this command will be sent as part of a
    // transaction and the write concern has already been loaded onto the opCtx and
    // will be picked up by the transaction API.
    parsedRequest.setWriteConcern(boost::none);

    // Strip runtime constants because they will be added again when this command is
    // recursively sent through the service entry point.
    parsedRequest.setLegacyRuntimeConstants(boost::none);
    if (txnRouter) {
        handleWouldChangeOwningShardErrorTransaction(
            opCtx, nss, responseStatus, parsedRequest, result, getCrudProcessedFromCmd(cmdObj));
    } else {
        if (isRetryableWrite) {
            parsedRequest.setStmtId(0);
        }
        handleWouldChangeOwningShardErrorNonTransaction(opCtx, shardId, nss, parsedRequest, result);
    }
}

}  // namespace mongo
