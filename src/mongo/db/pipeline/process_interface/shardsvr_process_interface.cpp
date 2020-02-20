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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/process_interface/shardsvr_process_interface.h"

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/cluster_write.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using write_ops::Insert;
using write_ops::Update;
using write_ops::UpdateOpEntry;

bool ShardServerProcessInterface::isSharded(OperationContext* opCtx, const NamespaceString& nss) {
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_IS);
    Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
    return CollectionShardingState::get(opCtx, nss)->getCollectionDescription().isSharded();
}

void ShardServerProcessInterface::checkRoutingInfoEpochOrThrow(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    ChunkVersion targetCollectionVersion) const {
    auto catalogCache = Grid::get(expCtx->opCtx)->catalogCache();
    auto const shardId = ShardingState::get(expCtx->opCtx)->shardId();

    return catalogCache->checkEpochOrThrow(nss, targetCollectionVersion, shardId);
}

std::pair<std::vector<FieldPath>, bool>
ShardServerProcessInterface::collectDocumentKeyFieldsForHostedCollection(OperationContext* opCtx,
                                                                         const NamespaceString& nss,
                                                                         UUID uuid) const {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);

    const auto collDesc = [opCtx, &nss]() -> ScopedCollectionDescription {
        Lock::DBLock dbLock(opCtx, nss.db(), MODE_IS);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
        return CollectionShardingState::get(opCtx, nss)->getCollectionDescription();
    }();

    if (!collDesc.isSharded() || !collDesc.uuidMatches(uuid)) {
        // An unsharded collection can still become sharded so is not final. If the uuid doesn't
        // match the one stored in the ScopedCollectionDescription, this implies that the collection
        // has been dropped and recreated as sharded. We don't know what the old document key fields
        // might have been in this case so we return just _id.
        return {{"_id"}, false};
    }

    // Unpack the shard key. Collection is now sharded so the document key fields will never change,
    // mark as final.
    return {_shardKeyToDocumentKeyFields(collDesc.getKeyPatternFields()), true};
}

Status ShardServerProcessInterface::insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           const NamespaceString& ns,
                                           std::vector<BSONObj>&& objs,
                                           const WriteConcernOptions& wc,
                                           boost::optional<OID> targetEpoch) {
    BatchedCommandResponse response;
    BatchWriteExecStats stats;

    BatchedCommandRequest insertCommand(
        buildInsertOp(ns, std::move(objs), expCtx->bypassDocumentValidation));

    insertCommand.setWriteConcern(wc.toBSON());

    ClusterWriter::write(expCtx->opCtx, insertCommand, &stats, &response, targetEpoch);

    return response.toStatus();
}

StatusWith<MongoProcessInterface::UpdateResult> ShardServerProcessInterface::update(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& ns,
    BatchedObjects&& batch,
    const WriteConcernOptions& wc,
    UpsertType upsert,
    bool multi,
    boost::optional<OID> targetEpoch) {
    BatchedCommandResponse response;
    BatchWriteExecStats stats;

    BatchedCommandRequest updateCommand(buildUpdateOp(expCtx, ns, std::move(batch), upsert, multi));

    updateCommand.setWriteConcern(wc.toBSON());

    ClusterWriter::write(expCtx->opCtx, updateCommand, &stats, &response, targetEpoch);

    if (auto status = response.toStatus(); status != Status::OK()) {
        return status;
    }
    return {{response.getN(), response.getNModified()}};
}

BSONObj ShardServerProcessInterface::attachCursorSourceAndExplain(
    Pipeline* ownedPipeline, ExplainOptions::Verbosity verbosity) {
    return sharded_agg_helpers::targetShardsForExplain(ownedPipeline);
}

std::unique_ptr<ShardFilterer> ShardServerProcessInterface::getShardFilterer(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    auto collectionFilter =
        CollectionShardingState::get(expCtx->opCtx, expCtx->ns)->getOwnershipFilter(expCtx->opCtx);
    return std::make_unique<ShardFiltererImpl>(std::move(collectionFilter));
}

void ShardServerProcessInterface::renameIfOptionsAndIndexesHaveNotChanged(
    OperationContext* opCtx,
    const BSONObj& renameCommandObj,
    const NamespaceString& destinationNs,
    const BSONObj& originalCollectionOptions,
    const std::list<BSONObj>& originalIndexes) {
    auto newCmdObj = CommonMongodProcessInterface::_convertRenameToInternalRename(
        opCtx, renameCommandObj, originalCollectionOptions, originalIndexes);
    BSONObjBuilder newCmdWithWriteConcernBuilder(std::move(newCmdObj));
    newCmdWithWriteConcernBuilder.append(WriteConcernOptions::kWriteConcernField,
                                         opCtx->getWriteConcern().toBSON());
    newCmdObj = newCmdWithWriteConcernBuilder.done();
    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, destinationNs.db()));
    auto response =
        executeCommandAgainstDatabasePrimary(opCtx,
                                             // internalRenameIfOptionsAndIndexesMatch is adminOnly.
                                             NamespaceString::kAdminDb,
                                             std::move(cachedDbInfo),
                                             newCmdObj,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                             Shard::RetryPolicy::kNoRetry);
    uassertStatusOKWithContext(response.swResponse,
                               str::stream() << "failed while running command " << newCmdObj);
    auto result = response.swResponse.getValue().data;
    uassertStatusOKWithContext(getStatusFromCommandResult(result),
                               str::stream() << "failed while running command " << newCmdObj);
    uassertStatusOKWithContext(getWriteConcernStatusFromCommandResult(result),
                               str::stream() << "failed while running command " << newCmdObj);
}

std::list<BSONObj> ShardServerProcessInterface::getIndexSpecs(OperationContext* opCtx,
                                                              const NamespaceString& ns,
                                                              bool includeBuildUUIDs) {
    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, ns.db()));
    auto shard = uassertStatusOK(
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, cachedDbInfo.primaryId()));
    auto cmdObj = BSON("listIndexes" << ns.coll());
    Shard::QueryResponse indexes;
    try {
        indexes = uassertStatusOK(
            shard->runExhaustiveCursorCommand(opCtx,
                                              ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                              ns.db().toString(),
                                              appendDbVersionIfPresent(cmdObj, cachedDbInfo),
                                              Milliseconds(-1)));
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return std::list<BSONObj>();
    }
    return std::list<BSONObj>(indexes.docs.begin(), indexes.docs.end());
}
void ShardServerProcessInterface::createCollection(OperationContext* opCtx,
                                                   const std::string& dbName,
                                                   const BSONObj& cmdObj) {
    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName));
    BSONObjBuilder finalCmdBuilder(cmdObj);
    finalCmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                           opCtx->getWriteConcern().toBSON());
    BSONObj finalCmdObj = finalCmdBuilder.obj();
    auto response =
        executeCommandAgainstDatabasePrimary(opCtx,
                                             dbName,
                                             std::move(cachedDbInfo),
                                             finalCmdObj,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                             Shard::RetryPolicy::kIdempotent);
    uassertStatusOKWithContext(response.swResponse,
                               str::stream() << "failed while running command " << finalCmdObj);
    auto result = response.swResponse.getValue().data;
    uassertStatusOKWithContext(getStatusFromCommandResult(result),
                               str::stream() << "failed while running command " << finalCmdObj);
    uassertStatusOKWithContext(getWriteConcernStatusFromCommandResult(result),
                               str::stream()
                                   << "write concern failed while running command " << finalCmdObj);
}

void ShardServerProcessInterface::createIndexesOnEmptyCollection(
    OperationContext* opCtx, const NamespaceString& ns, const std::vector<BSONObj>& indexSpecs) {
    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, ns.db()));
    BSONObjBuilder newCmdBuilder;
    newCmdBuilder.append("createIndexes", ns.coll());
    newCmdBuilder.append("indexes", indexSpecs);
    newCmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                         opCtx->getWriteConcern().toBSON());
    auto cmdObj = newCmdBuilder.done();
    auto response =
        executeCommandAgainstDatabasePrimary(opCtx,
                                             ns.db(),
                                             std::move(cachedDbInfo),
                                             cmdObj,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                             Shard::RetryPolicy::kIdempotent);
    uassertStatusOKWithContext(response.swResponse,
                               str::stream() << "failed while running command " << cmdObj);
    auto result = response.swResponse.getValue().data;
    uassertStatusOKWithContext(getStatusFromCommandResult(result),
                               str::stream() << "failed while running command " << cmdObj);
    uassertStatusOKWithContext(getWriteConcernStatusFromCommandResult(result),
                               str::stream()
                                   << "write concern failed while running command " << cmdObj);
}
void ShardServerProcessInterface::dropCollection(OperationContext* opCtx,
                                                 const NamespaceString& ns) {
    // Build and execute the dropCollection command against the primary shard of the given
    // database.
    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, ns.db()));
    BSONObjBuilder newCmdBuilder;
    newCmdBuilder.append("drop", ns.coll());
    newCmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                         opCtx->getWriteConcern().toBSON());
    auto cmdObj = newCmdBuilder.done();
    auto response =
        executeCommandAgainstDatabasePrimary(opCtx,
                                             ns.db(),
                                             std::move(cachedDbInfo),
                                             cmdObj,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                             Shard::RetryPolicy::kIdempotent);
    uassertStatusOKWithContext(response.swResponse,
                               str::stream() << "failed while running command " << cmdObj);
    auto result = response.swResponse.getValue().data;
    uassertStatusOKWithContext(getStatusFromCommandResult(result),
                               str::stream() << "failed while running command " << cmdObj);
    uassertStatusOKWithContext(getWriteConcernStatusFromCommandResult(result),
                               str::stream()
                                   << "write concern failed while running command " << cmdObj);
}

std::unique_ptr<Pipeline, PipelineDeleter>
ShardServerProcessInterface::attachCursorSourceToPipeline(Pipeline* ownedPipeline,
                                                          bool allowTargetingShards) {
    return sharded_agg_helpers::attachCursorToPipeline(ownedPipeline, allowTargetingShards);
}

}  // namespace mongo
