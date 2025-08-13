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

#include "mongo/db/pipeline/process_interface/replica_set_node_process_interface.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/db/commands.h"
#include "mongo/db/local_catalog/ddl/list_collections_gen.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/pipeline/process_interface/common_mongod_process_interface.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/repl/intent_guard.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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

Status ReplicaSetNodeProcessInterface::insert(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& ns,
    std::unique_ptr<write_ops::InsertCommandRequest> insertCommand,
    const WriteConcernOptions& wc,
    boost::optional<OID> targetEpoch) {
    auto&& opCtx = expCtx->getOperationContext();
    if (_canWriteLocally(opCtx, ns)) {
        return NonShardServerProcessInterface::insert(
            expCtx, ns, std::move(insertCommand), wc, targetEpoch);
    }

    BatchedCommandRequest batchInsertCommand(std::move(insertCommand));

    return _executeCommandOnPrimary(opCtx, ns, batchInsertCommand.toBSON()).getStatus();
}

StatusWith<MongoProcessInterface::UpdateResult> ReplicaSetNodeProcessInterface::update(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& ns,
    std::unique_ptr<write_ops::UpdateCommandRequest> updateCommand,
    const WriteConcernOptions& wc,
    UpsertType upsert,
    bool multi,
    boost::optional<OID> targetEpoch) {
    auto&& opCtx = expCtx->getOperationContext();
    if (_canWriteLocally(opCtx, ns)) {
        return NonShardServerProcessInterface::update(
            expCtx, ns, std::move(updateCommand), wc, upsert, multi, targetEpoch);
    }
    BatchedCommandRequest batchUpdateCommand(std::move(updateCommand));

    auto result = _executeCommandOnPrimary(opCtx, ns, batchUpdateCommand.toBSON());
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

void ReplicaSetNodeProcessInterface::createTimeseriesView(OperationContext* opCtx,
                                                          const NamespaceString& ns,
                                                          const BSONObj& cmdObj,
                                                          const TimeseriesOptions& userOpts) {
    if (_canWriteLocally(opCtx, ns)) {
        return NonShardServerProcessInterface::createTimeseriesView(opCtx, ns, cmdObj, userOpts);
    }

    try {
        uassertStatusOK(_executeCommandOnPrimary(opCtx, ns, cmdObj));
    } catch (const DBException& ex) {
        _handleTimeseriesCreateError(ex, opCtx, ns, userOpts);
    }
}

Status ReplicaSetNodeProcessInterface::insertTimeseries(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& ns,
    std::unique_ptr<write_ops::InsertCommandRequest> insertCommand,
    const WriteConcernOptions& wc,
    boost::optional<OID> targetEpoch) {
    if (_canWriteLocally(expCtx->getOperationContext(), ns)) {
        return NonShardServerProcessInterface::insertTimeseries(
            expCtx, ns, std::move(insertCommand), wc, targetEpoch);
    } else {
        return ReplicaSetNodeProcessInterface::insert(
            expCtx, ns, std::move(insertCommand), wc, targetEpoch);
    }
}

void ReplicaSetNodeProcessInterface::renameIfOptionsAndIndexesHaveNotChanged(
    OperationContext* opCtx,
    const NamespaceString& sourceNs,
    const NamespaceString& targetNs,
    bool dropTarget,
    bool stayTemp,
    const BSONObj& originalCollectionOptions,
    const std::list<BSONObj>& originalIndexes) {
    if (_canWriteLocally(opCtx, targetNs)) {
        return NonShardServerProcessInterface::renameIfOptionsAndIndexesHaveNotChanged(
            opCtx,
            sourceNs,
            targetNs,
            dropTarget,
            stayTemp,
            originalCollectionOptions,
            originalIndexes);
    }
    // internalRenameIfOptionsAndIndexesMatch can only be run against the admin DB.
    NamespaceString adminNs{DatabaseName::kAdmin};
    auto cmd = CommonMongodProcessInterface::_convertRenameToInternalRename(
        opCtx, sourceNs, targetNs, originalCollectionOptions, originalIndexes);
    uassertStatusOK(_executeCommandOnPrimary(opCtx, adminNs, cmd));
}

void ReplicaSetNodeProcessInterface::createCollection(OperationContext* opCtx,
                                                      const DatabaseName& dbName,
                                                      const BSONObj& cmdObj) {
    NamespaceString dbNs = NamespaceString(dbName);
    if (_canWriteLocally(opCtx, dbNs)) {
        return NonShardServerProcessInterface::createCollection(opCtx, dbName, cmdObj);
    }
    auto ns = CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);
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

UUID ReplicaSetNodeProcessInterface::fetchCollectionUUIDFromPrimary(OperationContext* opCtx,
                                                                    const NamespaceString& nss) {
    // Create a listCollections command and run it against the primary to get the UUID.
    ListCollections listCollectionsCmd;
    listCollectionsCmd.setDbName(nss.dbName());
    listCollectionsCmd.setFilter(BSON("name" << nss.coll()));

    auto result = _executeCommandOnPrimary(
        opCtx, nss, listCollectionsCmd.toBSON(), /* attachWriteConcern */ false);
    uassertStatusOK(result);

    auto uuid =
        bson::extractElementAtDottedPath(result.getValue(), "cursor.firstBatch.0.info.uuid");
    return uassertStatusOK(UUID::parse(uuid));
}

StatusWith<BSONObj> ReplicaSetNodeProcessInterface::_executeCommandOnPrimary(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const BSONObj& cmdObj,
    bool attachWriteConcern) const {
    BSONObjBuilder cmd(cmdObj);

    _attachGenericCommandArgs(opCtx, &cmd, attachWriteConcern);

    // Verify that the ReplicationCoordinator believes that a primary exists before issuing a
    // command to it.
    auto hostAndPort = repl::ReplicationCoordinator::get(opCtx)->getCurrentPrimaryHostAndPort();
    if (hostAndPort.empty()) {
        return StatusWith<BSONObj>{ErrorCodes::PrimarySteppedDown, "No primary exists currently"};
    }

    executor::RemoteCommandRequest request(std::move(hostAndPort), ns.dbName(), cmd.obj(), opCtx);
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
        invariant(operationTime.type() == BSONType::timestamp);
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
                                                               BSONObjBuilder* cmd,
                                                               bool attachWriteConcern) const {
    if (attachWriteConcern) {
        cmd->append(WriteConcernOptions::kWriteConcernField, opCtx->getWriteConcern().toBSON());
    }

    auto maxTimeMS = opCtx->getRemainingMaxTimeMillis();
    if (maxTimeMS != Milliseconds::max()) {
        cmd->append(query_request_helper::cmdOptionMaxTimeMS,
                    durationCount<Milliseconds>(maxTimeMS));
    }

    logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, cmd);
}

bool ReplicaSetNodeProcessInterface::_canWriteLocally(OperationContext* opCtx,
                                                      const NamespaceString& ns) const {
    if (gFeatureFlagIntentRegistration.isEnabled()) {
        return rss::consensus::IntentRegistry::get(opCtx->getServiceContext())
            .canDeclareIntent(rss::consensus::IntentRegistry::Intent::Write, opCtx);
    } else {
        Lock::ResourceLock rstl(opCtx, resourceIdReplicationStateTransitionLock, MODE_IX);
        return repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, ns);
    }
}

}  // namespace mongo
