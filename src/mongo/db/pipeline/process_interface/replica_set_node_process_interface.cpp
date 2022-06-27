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

#include "mongo/db/pipeline/process_interface/replica_set_node_process_interface.h"

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/future.h"

namespace mongo {

namespace {
const char kOperationTimeFieldName[] = "operationTime";

const auto replicaSetNodeExecutor =
    ServiceContext::declareDecoration<std::shared_ptr<executor::TaskExecutor>>();
}  // namespace

std::shared_ptr<executor::TaskExecutor> ReplicaSetNodeProcessInterface::getReplicaSetNodeExecutor(
    ServiceContext* service) {
    return replicaSetNodeExecutor(service);
}

std::shared_ptr<executor::TaskExecutor> ReplicaSetNodeProcessInterface::getReplicaSetNodeExecutor(
    OperationContext* opCtx) {
    return getReplicaSetNodeExecutor(opCtx->getServiceContext());
}

void ReplicaSetNodeProcessInterface::setReplicaSetNodeExecutor(
    ServiceContext* service, std::shared_ptr<executor::TaskExecutor> executor) {
    replicaSetNodeExecutor(service) = std::move(executor);
}

Status ReplicaSetNodeProcessInterface::insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              const NamespaceString& ns,
                                              std::vector<BSONObj>&& objs,
                                              const WriteConcernOptions& wc,
                                              boost::optional<OID> targetEpoch) {
    auto&& opCtx = expCtx->opCtx;
    if (_canWriteLocally(opCtx, ns)) {
        return NonShardServerProcessInterface::insert(expCtx, ns, std::move(objs), wc, targetEpoch);
    }

    BatchedCommandRequest insertCommand(
        buildInsertOp(ns, std::move(objs), expCtx->bypassDocumentValidation));

    return _executeCommandOnPrimary(opCtx, ns, std::move(insertCommand.toBSON())).getStatus();
}

StatusWith<MongoProcessInterface::UpdateResult> ReplicaSetNodeProcessInterface::update(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& ns,
    BatchedObjects&& batch,
    const WriteConcernOptions& wc,
    UpsertType upsert,
    bool multi,
    boost::optional<OID> targetEpoch) {
    auto&& opCtx = expCtx->opCtx;
    if (_canWriteLocally(opCtx, ns)) {
        return NonShardServerProcessInterface::update(
            expCtx, ns, std::move(batch), wc, upsert, multi, targetEpoch);
    }

    BatchedCommandRequest updateCommand(buildUpdateOp(expCtx, ns, std::move(batch), upsert, multi));
    auto result = _executeCommandOnPrimary(opCtx, ns, std::move(updateCommand.toBSON()));
    if (!result.isOK()) {
        return result.getStatus();
    }

    std::string errMsg;
    BatchedCommandResponse response;
    uassert(31450, errMsg, response.parseBSON(result.getValue(), &errMsg));

    return UpdateResult{response.getN(), response.getNModified()};
}

void ReplicaSetNodeProcessInterface::createIndexesOnEmptyCollection(
    OperationContext* opCtx, const NamespaceString& ns, const std::vector<BSONObj>& indexSpecs) {
    if (_canWriteLocally(opCtx, ns)) {
        return NonShardServerProcessInterface::createIndexesOnEmptyCollection(
            opCtx, ns, indexSpecs);
    }
    BSONObjBuilder cmd;
    cmd.append("createIndexes", ns.coll());
    cmd.append("indexes", indexSpecs);
    uassertStatusOK(_executeCommandOnPrimary(opCtx, ns, cmd.obj()));
}

void ReplicaSetNodeProcessInterface::renameIfOptionsAndIndexesHaveNotChanged(
    OperationContext* opCtx,
    const BSONObj& renameCommandObj,
    const NamespaceString& targetNs,
    const BSONObj& originalCollectionOptions,
    const std::list<BSONObj>& originalIndexes) {
    if (_canWriteLocally(opCtx, targetNs)) {
        return NonShardServerProcessInterface::renameIfOptionsAndIndexesHaveNotChanged(
            opCtx, renameCommandObj, targetNs, originalCollectionOptions, originalIndexes);
    }
    // internalRenameIfOptionsAndIndexesMatch can only be run against the admin DB.
    NamespaceString adminNs{NamespaceString::kAdminDb};
    auto cmd = CommonMongodProcessInterface::_convertRenameToInternalRename(
        opCtx, renameCommandObj, originalCollectionOptions, originalIndexes);
    uassertStatusOK(_executeCommandOnPrimary(opCtx, adminNs, cmd));
}

void ReplicaSetNodeProcessInterface::createCollection(OperationContext* opCtx,
                                                      const DatabaseName& dbName,
                                                      const BSONObj& cmdObj) {
    NamespaceString dbNs = NamespaceString(dbName, StringData(""));
    if (_canWriteLocally(opCtx, dbNs)) {
        return NonShardServerProcessInterface::createCollection(opCtx, dbName, cmdObj);
    }
    // TODO SERVER-67519 change CommandHelpers::parseNsCollectionRequired to take in DatabaseName
    auto ns = CommandHelpers::parseNsCollectionRequired(dbName.toStringWithTenantId(), cmdObj);
    uassertStatusOK(_executeCommandOnPrimary(opCtx, ns, cmdObj));
}

void ReplicaSetNodeProcessInterface::dropCollection(OperationContext* opCtx,
                                                    const NamespaceString& ns) {
    if (_canWriteLocally(opCtx, ns)) {
        return NonShardServerProcessInterface::dropCollection(opCtx, ns);
    }
    BSONObjBuilder cmd;
    cmd.append("drop", ns.coll());
    uassertStatusOK(_executeCommandOnPrimary(opCtx, ns, cmd.obj()));
}

StatusWith<BSONObj> ReplicaSetNodeProcessInterface::_executeCommandOnPrimary(
    OperationContext* opCtx, const NamespaceString& ns, const BSONObj& cmdObj) const {
    BSONObjBuilder cmd(cmdObj);
    _attachGenericCommandArgs(opCtx, &cmd);

    // Verify that the ReplicationCoordinator believes that a primary exists before issuing a
    // command to it.
    auto hostAndPort = repl::ReplicationCoordinator::get(opCtx)->getCurrentPrimaryHostAndPort();
    if (hostAndPort.empty()) {
        return StatusWith<BSONObj>{ErrorCodes::PrimarySteppedDown, "No primary exists currently"};
    }

    executor::RemoteCommandRequest request(
        std::move(hostAndPort), ns.db().toString(), cmd.obj(), opCtx);
    auto [promise, future] = makePromiseFuture<executor::TaskExecutor::RemoteCommandCallbackArgs>();
    auto promisePtr = std::make_shared<Promise<executor::TaskExecutor::RemoteCommandCallbackArgs>>(
        std::move(promise));
    auto scheduleResult = taskExecutor->scheduleRemoteCommand(
        std::move(request), [promisePtr](const auto& args) { promisePtr->emplaceValue(args); });
    if (!scheduleResult.isOK()) {
        // Since the command failed to be scheduled, the callback above did not and will not run.
        // Thus, it is safe to fulfill the promise here without worrying about synchronizing access
        // with the executor's thread.
        promisePtr->setError(scheduleResult.getStatus());
    }

    auto response = future.getNoThrow(opCtx);
    if (!response.isOK()) {
        return response.getStatus();
    }

    auto rcr = std::move(response.getValue());

    // Update the OperationTimeTracker associated with 'opCtx' with the operation time from the
    // primary's response.
    auto operationTime = rcr.response.data[kOperationTimeFieldName];
    if (operationTime) {
        invariant(operationTime.type() == BSONType::bsonTimestamp);
        LogicalTime logicalTime(operationTime.timestamp());
        auto operationTimeTracker = OperationTimeTracker::get(opCtx);
        operationTimeTracker->updateOperationTime(logicalTime);
    }

    if (!rcr.response.status.isOK()) {
        return rcr.response.status;
    }

    auto commandStatus = getStatusFromCommandResult(rcr.response.data);
    if (!commandStatus.isOK()) {
        return commandStatus;
    }

    auto writeConcernStatus = getWriteConcernStatusFromCommandResult(rcr.response.data);
    if (!writeConcernStatus.isOK()) {
        return writeConcernStatus;
    }

    auto writeStatus = getFirstWriteErrorStatusFromCommandResult(rcr.response.data);
    if (!writeStatus.isOK()) {
        return writeStatus;
    }

    return rcr.response.data;
}

void ReplicaSetNodeProcessInterface::_attachGenericCommandArgs(OperationContext* opCtx,
                                                               BSONObjBuilder* cmd) const {
    cmd->append(WriteConcernOptions::kWriteConcernField, opCtx->getWriteConcern().toBSON());

    auto maxTimeMS = opCtx->getRemainingMaxTimeMillis();
    if (maxTimeMS != Milliseconds::max()) {
        cmd->append(query_request_helper::cmdOptionMaxTimeMS,
                    durationCount<Milliseconds>(maxTimeMS));
    }

    logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, cmd);
}

bool ReplicaSetNodeProcessInterface::_canWriteLocally(OperationContext* opCtx,
                                                      const NamespaceString& ns) const {
    Lock::ResourceLock rstl(opCtx->lockState(), resourceIdReplicationStateTransitionLock, MODE_IX);
    return repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, ns);
}

}  // namespace mongo
