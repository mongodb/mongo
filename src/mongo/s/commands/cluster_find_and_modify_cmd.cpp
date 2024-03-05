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

#include <boost/cstdint.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_extra_info.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/commands/update_metrics.h"
#include "mongo/db/curop.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/resource_yielder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/document_shard_key_update_util.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/shard_key_pattern_query_util.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

constexpr size_t kMaxDatabaseCreationAttempts = 3u;

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

namespace {
BSONObj getQueryForShardKey(boost::intrusive_ptr<ExpressionContext> expCtx,
                            const ChunkManager& cm,
                            const BSONObj& query,
                            bool isTimeseriesViewRequest) {
    if (auto tsFields = cm.getTimeseriesFields();
        cm.isSharded() && tsFields && isTimeseriesViewRequest) {
        // Note: The useTwoPhaseProtocol() uses the shard key extractor to decide whether it should
        // use the two phase protocol and the shard key extractor is only based on the equality
        // query. But we still should be able to route the query to the correct shard from a range
        // query on shard keys (not always) and unfortunately, even an equality query on the time
        // field for timeseries collections would be translated into a range query on
        // control.min.time and control.max.time. So, with this, we can support targeted
        // findAndModify based on the time field.
        //
        // If the collection is a sharded timeseries collection, rewrite the query into a
        // bucket-level query.
        return timeseries::getBucketLevelPredicateForRouting(
            query, expCtx, tsFields->getTimeseriesOptions(), /* allowArbitraryWrites */ true);
    }

    return query;
}
}  // namespace

void handleWouldChangeOwningShardErrorNonTransaction(
    OperationContext* opCtx,
    const ShardId& shardId,
    const NamespaceString& nss,
    const write_ops::FindAndModifyCommandRequest& request,
    BSONObjBuilder* result) {
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

    auto txn = txn_api::SyncTransactionWithRetries(
        opCtx, executor, nullptr /* resourceYielder */, inlineExecutor);

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

    // For timeseries collections, the 'postMeasurementImage' is returned back through
    // WouldChangeOwningShardInfo from the old shard as well and it should be returned to the user
    // instead of the post-image.
    auto postImage = [&] {
        return changeInfo.getUserPostImage() ? *changeInfo.getUserPostImage()
                                             : changeInfo.getPostImage();
    }();

    if (updatedExistingDocument) {
        result->append("value", shouldReturnPostImage ? postImage : changeInfo.getPreImage());
    } else if (upserted && shouldReturnPostImage) {
        result->append("value", postImage);
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

        auto txn = txn_api::SyncTransactionWithRetries(
            opCtx,
            executor,
            TransactionRouterResourceYielder::makeForLocalHandoff(),
            inlineExecutor);

        txn.run(opCtx,
                [opCtx, sharedBlock, fleCrudProcessed](const txn_api::TransactionClient& txnClient,
                                                       ExecutorPtr txnExec) -> SemiFuture<void> {
                    return documentShardKeyUpdateUtil::updateShardKeyForDocument(
                               txnClient,
                               opCtx,
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
                                                        bool isTimeseriesViewRequest,
                                                        bool fleCrudProcessed) {
    BSONObjBuilder extraInfoBuilder;
    responseStatus.extraInfo()->serialize(&extraInfoBuilder);
    auto extraInfo = extraInfoBuilder.obj();
    auto wouldChangeOwningShardExtraInfo =
        WouldChangeOwningShardInfo::parseFromCommandError(extraInfo);

    try {
        auto matchedDocOrUpserted = documentShardKeyUpdateUtil::updateShardKeyForDocumentLegacy(
            opCtx, nss, wouldChangeOwningShardExtraInfo, isTimeseriesViewRequest, fleCrudProcessed);

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

MONGO_REGISTER_COMMAND(FindAndModifyCmd).forRouter();

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
    ResourcePattern resource(CommandHelpers::resourcePatternForNamespace(nss));
    uassert(17137,
            "Invalid target namespace " + resource.toString(),
            resource.isExactNamespacePattern());

    auto* as = AuthorizationSession::get(opCtx->getClient());
    if (!as->isAuthorizedForActionsOnResource(resource, actions)) {
        return {ErrorCodes::Unauthorized, "unauthorized"};
    }

    return Status::OK();
}

namespace {
/**
 * Replaces the target namespace in the 'cmdObj' by 'bucketNss'. Also sets the
 * 'isTimeseriesNamespace' flag.
 */
BSONObj replaceNamespaceByBucketNss(const BSONObj& cmdObj, const NamespaceString& bucketNss) {
    BSONObjBuilder bob;
    for (const auto& elem : cmdObj) {
        const auto name = elem.fieldNameStringData();
        if (name == write_ops::FindAndModifyCommandRequest::kCommandName) {
            bob.append(write_ops::FindAndModifyCommandRequest::kCommandName, bucketNss.coll());
        } else {
            bob.append(elem);
        }
    }
    // Set this flag so that shards can differentiate a request on a time-series view from a request
    // on a time-series buckets collection since we replace the target namespace in the command with
    // the buckets namespace.
    bob.append(write_ops::FindAndModifyCommandRequest::kIsTimeseriesNamespaceFieldName, true);

    return bob.obj();
}

/**
 * Returns CollectionRoutingInfo for 'maybeTsNss' namespace. If 'maybeTsNss' is a timeseries
 * collection, returns CollectionRoutingInfo for the corresponding timeseries buckets collection.
 */
CollectionRoutingInfo getCollectionRoutingInfo(OperationContext* opCtx,
                                               const BSONObj& cmdObj,
                                               const NamespaceString& maybeTsNss) {
    // Apparently, we should return the CollectionRoutingInfo for the original namespace if we're
    // not writing to a timeseries collection.
    auto cri = uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, maybeTsNss));

    // Note: We try to get CollectionRoutingInfo for the timeseries buckets collection only when the
    // timeseries deletes or updates feature flag is enabled.
    const bool arbitraryTimeseriesWritesEnabled =
        feature_flags::gTimeseriesDeletesSupport.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
        feature_flags::gTimeseriesUpdatesSupport.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    if (!arbitraryTimeseriesWritesEnabled || cri.cm.hasRoutingTable() ||
        maybeTsNss.isTimeseriesBucketsCollection()) {
        return cri;
    }

    // If the 'maybeTsNss' namespace is not a timeseries buckets collection and is not tracked on
    // the configsvr, try to get the CollectionRoutingInfo for the corresponding timeseries buckets
    // collection to see if it's tracked and it really is a timeseries buckets collection. We should
    // do this to figure out whether we need to use the two phase write protocol or not on
    // timeseries buckets collections.
    auto bucketCollNss = maybeTsNss.makeTimeseriesBucketsNamespace();
    auto bucketCollCri = uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, bucketCollNss));
    if (!bucketCollCri.cm.hasRoutingTable() || !bucketCollCri.cm.getTimeseriesFields()) {
        return cri;
    }

    uassert(ErrorCodes::InvalidOptions,
            "Cannot perform findAndModify with sort on a sharded timeseries collection",
            !cmdObj.hasField("sort"));

    return bucketCollCri;
}

/**
 * Returns the shard id if the 'query' can be targeted to a single shard. Otherwise, returns
 * boost::none.
 */
boost::optional<ShardId> targetPotentiallySingleShard(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const ChunkManager& cm,
    const BSONObj& query,
    const BSONObj& collation,
    bool isTimeseriesViewRequest) {
    // Special case: there's only one shard owning all the chunks.
    if (cm.getNShardsOwningChunks() == 1) {
        std::set<ShardId> shardIds;
        cm.getAllShardIds(&shardIds);
        return *shardIds.begin();
    }

    std::set<ShardId> shardIds;
    getShardIdsForQuery(expCtx,
                        getQueryForShardKey(expCtx, cm, query, isTimeseriesViewRequest),
                        collation,
                        cm,
                        &shardIds);

    if (shardIds.size() == 1) {
        // If we can find a single shard to target, we can skip the two phase write protocol.
        return *shardIds.begin();
    }

    return boost::none;
}

ShardId targetSingleShard(boost::intrusive_ptr<ExpressionContext> expCtx,
                          const ChunkManager& cm,
                          const BSONObj& query,
                          const BSONObj& collation,
                          bool isTimeseriesViewRequest) {
    std::set<ShardId> shardIds;

    // For now, set bypassIsFieldHashedCheck to be true in order to skip the
    // isFieldHashedCheck in the special case where _id is hashed and used as the shard
    // key. This means that we always assume that a findAndModify request using _id is
    // targetable to a single shard.
    getShardIdsForQuery(expCtx,
                        getQueryForShardKey(expCtx, cm, query, isTimeseriesViewRequest),
                        collation,
                        cm,
                        &shardIds,
                        nullptr,
                        true);

    uassert(ErrorCodes::ShardKeyNotFound,
            str::stream() << "Query with sharded findAndModify expected to only target one "
                             "shard, but the query targeted "
                          << shardIds.size() << " shard(s)",
            shardIds.size() == 1);

    return *shardIds.begin();
}

BSONObj makeExplainCmd(OperationContext* opCtx,
                       const BSONObj& cmdObj,
                       ExplainOptions::Verbosity verbosity) {
    return ClusterExplain::wrapAsExplain(appendLegacyRuntimeConstantsToCommandObject(opCtx, cmdObj),
                                         verbosity);
}
}  // namespace

Status FindAndModifyCmd::explain(OperationContext* opCtx,
                                 const OpMsgRequest& request,
                                 ExplainOptions::Verbosity verbosity,
                                 rpc::ReplyBuilderInterface* result) const {
    const DatabaseName dbName = request.getDbName();
    auto bodyBuilder = result->getBodyBuilder();
    BSONObj cmdObj = [&]() {
        // Check whether the query portion needs to be rewritten for FLE.
        auto findAndModifyRequest = write_ops::FindAndModifyCommandRequest::parse(
            IDLParserContext("ClusterFindAndModify"), request.body);
        if (shouldDoFLERewrite(findAndModifyRequest)) {
            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
            }

            auto newRequest = processFLEFindAndModifyExplainMongos(opCtx, findAndModifyRequest);
            return newRequest.first.toBSON(request.body);
        } else {
            return request.body;
        }
    }();
    NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));

    const auto cri = getCollectionRoutingInfo(opCtx, cmdObj, nss);
    const auto& cm = cri.cm;
    auto isTrackedTimeseries = cm.hasRoutingTable() && cm.getTimeseriesFields();
    auto isTimeseriesViewRequest = false;
    if (isTrackedTimeseries && !nss.isTimeseriesBucketsCollection()) {
        nss = std::move(cm.getNss());
        isTimeseriesViewRequest = true;
    }
    // Note: at this point, 'nss' should be the timeseries buckets collection namespace if we're
    // writing to a tracked timeseries collection.

    boost::optional<ShardId> shardId;
    const BSONObj query = cmdObj.getObjectField("query");
    const BSONObj collation = getCollation(cmdObj);
    const auto isUpsert = cmdObj.getBoolField("upsert");
    const auto let = getLet(cmdObj);
    const auto rc = getLegacyRuntimeConstants(cmdObj);
    if (cm.hasRoutingTable()) {
        // If the request is for a view on a sharded timeseries buckets collection, we need to
        // replace the namespace by buckets collection namespace in the command object.
        if (isTimeseriesViewRequest) {
            cmdObj = replaceNamespaceByBucketNss(cmdObj, nss);
        }
        auto expCtx = makeExpressionContextWithDefaultsForTargeter(
            opCtx, nss, cri, collation, boost::none /* verbosity */, let, rc);
        if (write_without_shard_key::useTwoPhaseProtocol(opCtx,
                                                         nss,
                                                         false /* isUpdateOrDelete */,
                                                         isUpsert,
                                                         query,
                                                         collation,
                                                         let,
                                                         rc,
                                                         isTimeseriesViewRequest)) {
            shardId =
                targetPotentiallySingleShard(expCtx, cm, query, collation, isTimeseriesViewRequest);
        } else {
            shardId = targetSingleShard(expCtx, cm, query, collation, isTimeseriesViewRequest);
        }
    } else {
        shardId = cm.dbPrimary();
    }

    // Time how long it takes to run the explain command on the shard.
    Timer timer;
    BSONObjBuilder bob;
    if (!shardId) {
        _runExplainWithoutShardKey(
            opCtx, nss, makeExplainCmd(opCtx, cmdObj, verbosity), verbosity, &bob);
        bodyBuilder.appendElementsUnique(bob.obj());
        return Status::OK();
    }

    auto shardVersion = cm.hasRoutingTable()
        ? boost::make_optional(cri.getShardVersion(*shardId))
        : boost::make_optional(!cm.dbVersion().isFixed(), ShardVersion::UNSHARDED());

    _runCommand(
        opCtx,
        *shardId,
        shardVersion,
        cm.dbVersion(),
        nss,
        applyReadWriteConcern(opCtx, false, false, makeExplainCmd(opCtx, cmdObj, verbosity)),
        true /* isExplain */,
        boost::none /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */,
        isTimeseriesViewRequest,
        &bob);

    const auto millisElapsed = timer.millis();

    executor::RemoteCommandResponse response(bob.obj(), Milliseconds(millisElapsed));

    // We fetch an arbitrary host from the ConnectionString, since
    // ClusterExplain::buildExplainResult() doesn't use the given HostAndPort.
    auto shard = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, *shardId));
    AsyncRequestsSender::Response arsResponse{
        *shardId, response, shard->getConnString().getServers().front()};

    return ClusterExplain::buildExplainResult(
        opCtx, {arsResponse}, ClusterExplain::kSingleShard, millisElapsed, cmdObj, &bodyBuilder);
}

bool FindAndModifyCmd::run(OperationContext* opCtx,
                           const DatabaseName& dbName,
                           const BSONObj& cmdObj,
                           BSONObjBuilder& result) {
    NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));

    if (processFLEFindAndModify(opCtx, cmdObj, result) == FLEBatchResult::kProcessed) {
        return true;
    }

    // Collect metrics.
    _updateMetrics->collectMetrics(cmdObj);


    auto cri = [&]() {
        size_t attempts = 1u;
        while (true) {
            try {
                // Technically, findAndModify should only be creating database if upsert is true,
                // but this would require that the parsing be pulled into this function.
                cluster::createDatabase(opCtx, nss.dbName());
                return getCollectionRoutingInfo(opCtx, cmdObj, nss);
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                LOGV2_INFO(8584300,
                           "Failed initialization of routing info because the database has been "
                           "concurrently dropped",
                           logAttrs(nss.dbName()),
                           "attemptNumber"_attr = attempts,
                           "maxAttempts"_attr = kMaxDatabaseCreationAttempts);

                if (attempts++ >= kMaxDatabaseCreationAttempts) {
                    // The maximum number of attempts has been reached, so the procedure fails as it
                    // could be a logical error. At this point, it is unlikely that the error is
                    // caused by concurrent drop database operations.
                    throw;
                }
            }
        }
    }();

    const auto& cm = cri.cm;
    auto isTrackedTimeseries = cm.hasRoutingTable() && cm.getTimeseriesFields();
    auto isTimeseriesViewRequest = false;
    if (isTrackedTimeseries && !nss.isTimeseriesBucketsCollection()) {
        nss = std::move(cm.getNss());
        isTimeseriesViewRequest = true;
    }
    // Note: at this point, 'nss' should be the timeseries buckets collection namespace if we're
    // writing to a sharded timeseries collection.

    // Append mongoS' runtime constants to the command object before forwarding it to the shard.
    auto cmdObjForShard = appendLegacyRuntimeConstantsToCommandObject(opCtx, cmdObj);
    if (cm.hasRoutingTable()) {
        // If the request is for a view on a sharded timeseries buckets collection, we need to
        // replace the namespace by buckets collection namespace in the command object.
        if (isTimeseriesViewRequest) {
            cmdObjForShard = replaceNamespaceByBucketNss(cmdObjForShard, nss);
        }

        auto letParams = getLet(cmdObjForShard);
        auto runtimeConstants = getLegacyRuntimeConstants(cmdObjForShard);
        BSONObj collation = getCollation(cmdObjForShard);
        auto expCtx = makeExpressionContextWithDefaultsForTargeter(
            opCtx, nss, cri, collation, boost::none /* verbosity */, letParams, runtimeConstants);

        // If this command has 'let' parameters, then evaluate them once and stash them back on the
        // original command object. Note that this isn't necessary outside of the case where we have
        // a routing table because this is intended to prevent evaluating let parameters multiple
        // times (which can only happen when executing against a sharded cluster).
        if (letParams) {
            // Serialize variables before moving 'cmdObjForShard' to avoid invalid access.
            expCtx->variables.seedVariablesWithLetParameters(expCtx.get(), *letParams);
            auto letVars = Value(expCtx->variables.toBSON(expCtx->variablesParseState, *letParams));

            MutableDocument cmdDoc(Document(std::move(cmdObjForShard)));
            cmdDoc[write_ops::FindAndModifyCommandRequest::kLetFieldName] = letVars;
            cmdObjForShard = cmdDoc.freeze().toBson();

            // Reset the objects set up above as they are now invalid given that 'cmdObjForShard'
            // has been changed.
            letParams = getLet(cmdObjForShard);
            runtimeConstants = getLegacyRuntimeConstants(cmdObjForShard);
            collation = getCollation(cmdObjForShard);
        }

        BSONObj query = cmdObjForShard.getObjectField("query");
        const bool isUpsert = cmdObjForShard.getBoolField("upsert");

        if (write_without_shard_key::useTwoPhaseProtocol(opCtx,
                                                         nss,
                                                         false /* isUpdateOrDelete */,
                                                         isUpsert,
                                                         query,
                                                         collation,
                                                         letParams,
                                                         runtimeConstants,
                                                         isTimeseriesViewRequest)) {
            findAndModifyNonTargetedShardedCount.increment(1);
            auto allowShardKeyUpdatesWithoutFullShardKeyInQuery =
                opCtx->isRetryableWrite() || opCtx->inMultiDocumentTransaction();

            if (auto shardId = targetPotentiallySingleShard(
                    expCtx, cm, query, collation, isTimeseriesViewRequest)) {
                // If we can find a single shard to target, we can skip the two phase write
                // protocol.
                _runCommand(opCtx,
                            *shardId,
                            cri.getShardVersion(*shardId),
                            boost::none,
                            nss,
                            applyReadWriteConcern(opCtx, this, cmdObjForShard),
                            false /* isExplain */,
                            allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                            isTimeseriesViewRequest,
                            &result);
            } else {
                _runCommandWithoutShardKey(opCtx,
                                           nss,
                                           applyReadWriteConcern(opCtx, this, cmdObjForShard),
                                           isTimeseriesViewRequest,
                                           &result);
            }
        } else {
            if (cm.isSharded()) {
                findAndModifyTargetedShardedCount.increment(1);
            } else {
                findAndModifyUnshardedCount.increment(1);
            }

            ShardId shardId =
                targetSingleShard(expCtx, cm, query, collation, isTimeseriesViewRequest);

            _runCommand(opCtx,
                        shardId,
                        cri.getShardVersion(shardId),
                        boost::none,
                        nss,
                        applyReadWriteConcern(opCtx, this, cmdObjForShard),
                        false /* isExplain */,
                        boost::none /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */,
                        isTimeseriesViewRequest,
                        &result);
        }
    } else {
        findAndModifyUnshardedCount.increment(1);
        _runCommand(opCtx,
                    cm.dbPrimary(),
                    boost::make_optional(!cm.dbVersion().isFixed(), ShardVersion::UNSHARDED()),
                    cm.dbVersion(),
                    nss,
                    applyReadWriteConcern(opCtx, this, cmdObjForShard),
                    false /* isExplain */,
                    boost::none /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */,
                    isTimeseriesViewRequest,
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
                                        bool isTimeseriesViewRequest,
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
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            handleWouldChangeOwningShardError(opCtx, shardId, nss, cmdObj, responseStatus, result);
        } else {
            // TODO SERVER-67429: Remove this branch.
            opCtx->setQuerySamplingOptions(QuerySamplingOptions::kOptOut);

            if (isRetryableWrite) {
                _handleWouldChangeOwningShardErrorRetryableWriteLegacy(opCtx,
                                                                       shardId,
                                                                       shardVersion,
                                                                       dbVersion,
                                                                       nss,
                                                                       cmdObj,
                                                                       isTimeseriesViewRequest,
                                                                       result);
            } else {
                handleWouldChangeOwningShardErrorTransactionLegacy(opCtx,
                                                                   nss,
                                                                   responseStatus,
                                                                   cmdObj,
                                                                   result,
                                                                   isTimeseriesViewRequest,
                                                                   getCrudProcessedFromCmd(cmdObj));
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
                                                  bool isTimeseriesViewRequest,
                                                  BSONObjBuilder* result) {
    auto allowShardKeyUpdatesWithoutFullShardKeyInQuery =
        opCtx->isRetryableWrite() || opCtx->inMultiDocumentTransaction();

    auto cmdObjForPassthrough =
        prepareCmdObjForPassthrough(opCtx,
                                    cmdObj,
                                    nss,
                                    false /* isExplain */,
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
                     isTimeseriesViewRequest,
                     result);
}

// Two-phase protocol to run an explain for a findAndModify command without a shard key or _id.
void FindAndModifyCmd::_runExplainWithoutShardKey(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const BSONObj& cmdObj,
                                                  ExplainOptions::Verbosity verbosity,
                                                  BSONObjBuilder* result) {
    auto cmdObjForPassthrough = prepareCmdObjForPassthrough(
        opCtx,
        cmdObj,
        nss,
        true /* isExplain */,
        boost::none /* dbVersion */,
        boost::none /* shardVersion */,
        boost::none /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */);

    // Explain currently cannot be run within a transaction, so each command is instead run
    // separately outside of a transaction, and we compose the results at the end.
    auto clusterQueryWithoutShardKeyExplainRes = [&] {
        ClusterQueryWithoutShardKey clusterQueryWithoutShardKeyCommand(cmdObjForPassthrough);
        const auto explainClusterQueryWithoutShardKeyCmd =
            ClusterExplain::wrapAsExplain(clusterQueryWithoutShardKeyCommand.toBSON({}), verbosity);
        auto opMsg = OpMsgRequestBuilder::createWithValidatedTenancyScope(
            nss.dbName(),
            auth::ValidatedTenancyScope::get(opCtx),
            explainClusterQueryWithoutShardKeyCmd);
        return CommandHelpers::runCommandDirectly(opCtx, opMsg).getOwned();
    }();

    auto clusterWriteWithoutShardKeyExplainRes = [&] {
        // Since 'explain' does not return the results of the query, we do not have an _id
        // document to target by from the 'Read Phase'. We instead will use a dummy _id
        // target document for the 'Write Phase'.
        ClusterWriteWithoutShardKey clusterWriteWithoutShardKeyCommand(
            cmdObjForPassthrough,
            clusterQueryWithoutShardKeyExplainRes.getStringField("targetShardId").toString(),
            write_without_shard_key::targetDocForExplain);
        const auto explainClusterWriteWithoutShardKeyCmd =
            ClusterExplain::wrapAsExplain(clusterWriteWithoutShardKeyCommand.toBSON({}), verbosity);
        auto opMsg = OpMsgRequestBuilder::createWithValidatedTenancyScope(
            nss.dbName(),
            auth::ValidatedTenancyScope::get(opCtx),
            explainClusterWriteWithoutShardKeyCmd);
        return CommandHelpers::runCommandDirectly(opCtx, opMsg).getOwned();
    }();

    auto output = write_without_shard_key::generateExplainResponseForTwoPhaseWriteProtocol(
        clusterQueryWithoutShardKeyExplainRes, clusterWriteWithoutShardKeyExplainRes);
    result->appendElementsUnique(output);
}

// Command invocation to be used if a shard key is specified or the collection is unsharded.
void FindAndModifyCmd::_runCommand(
    OperationContext* opCtx,
    const ShardId& shardId,
    const boost::optional<ShardVersion>& shardVersion,
    const boost::optional<DatabaseVersion>& dbVersion,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    bool isExplain,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
    bool isTimeseriesViewRequest,
    BSONObjBuilder* result) {
    auto txnRouter = TransactionRouter::get(opCtx);
    bool isRetryableWrite = opCtx->getTxnNumber() && !txnRouter;

    const auto response = [&] {
        std::vector<AsyncRequestsSender::Request> requests;
        auto cmdObjForPassthrough =
            prepareCmdObjForPassthrough(opCtx,
                                        cmdObj,
                                        nss,
                                        isExplain,
                                        dbVersion,
                                        shardVersion,
                                        allowShardKeyUpdatesWithoutFullShardKeyInQuery);
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
                     isTimeseriesViewRequest,
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
    bool isTimeseriesViewRequest,
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

        if (write_without_shard_key::useTwoPhaseProtocol(opCtx,
                                                         nss,
                                                         false /* isUpdateOrDelete */,
                                                         cmdObj.getBoolField("upsert"),
                                                         cmdObj.getObjectField("query"),
                                                         getCollation(cmdObj),
                                                         getLet(cmdObj),
                                                         getLegacyRuntimeConstants(cmdObj),
                                                         isTimeseriesViewRequest)) {
            findAndModifyNonTargetedShardedCount.increment(1);
            _runCommandWithoutShardKey(
                opCtx, nss, stripWriteConcern(cmdObj), isTimeseriesViewRequest, result);

        } else {
            findAndModifyTargetedShardedCount.increment(1);
            _runCommand(opCtx,
                        shardId,
                        shardVersion,
                        dbVersion,
                        nss,
                        stripWriteConcern(cmdObj),
                        false /* isExplain */,
                        boost::none /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */,
                        isTimeseriesViewRequest,
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
