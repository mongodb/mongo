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

#include "mongo/db/service_entry_point_mongod.h"

#include <memory>
#include <mutex>
#include <string>

#include <boost/move/utility_core.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/replica_set_endpoint_util.h"
#include "mongo/db/s/resharding/resharding_metrics_helpers.h"
#include "mongo/db/s/scoped_operation_completion_sharding_actions.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_entry_point_common.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/write_concern.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/rpc/check_allowed_op_query_cmd.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/gossiped_routing_cache_gen.h"
#include "mongo/s/grid.h"
#include "mongo/s/service_entry_point_mongos.h"
#include "mongo/s/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/polymorphic_scoped.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

class ServiceEntryPointMongod::Hooks final : public ServiceEntryPointCommon::Hooks {
public:
    bool lockedForWriting() const override {
        return mongo::lockedForWriting();
    }

    void setPrepareConflictBehaviorForReadConcern(
        OperationContext* opCtx, const CommandInvocation* invocation) const override {
        // Some read commands can safely ignore prepare conflicts by default because they do not
        // require snapshot isolation and do not conflict with concurrent writes. We also give these
        // operations permission to write, as this may be required for queries that spill using the
        // storage engine. The kIgnoreConflictsAllowWrites setting suppresses an assertion in the
        // storage engine that prevents operations that ignore prepare conflicts from also writing.
        const auto prepareConflictBehavior = invocation->canIgnorePrepareConflicts()
            ? PrepareConflictBehavior::kIgnoreConflictsAllowWrites
            : PrepareConflictBehavior::kEnforce;
        mongo::setPrepareConflictBehaviorForReadConcern(
            opCtx, repl::ReadConcernArgs::get(opCtx), prepareConflictBehavior);
    }

    void waitForReadConcern(OperationContext* opCtx,
                            const CommandInvocation* invocation,
                            const OpMsgRequest& request) const override {
        Status rcStatus = mongo::waitForReadConcern(opCtx,
                                                    repl::ReadConcernArgs::get(opCtx),
                                                    invocation->ns().dbName(),
                                                    invocation->allowsAfterClusterTime());

        if (!rcStatus.isOK()) {
            if (ErrorCodes::isExceededTimeLimitError(rcStatus.code())) {
                const int debugLevel =
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) ? 0 : 2;
                LOGV2_DEBUG(21975,
                            debugLevel,
                            "Command timed out waiting for read concern to be satisfied",
                            "db"_attr = request.getDatabase(),
                            "command"_attr =
                                redact(ServiceEntryPointCommon::getRedactedCopyForLogging(
                                    invocation->definition(), request.body)),
                            "error"_attr = redact(rcStatus));
            }

            uassertStatusOK(rcStatus);
        }
    }

    void waitForSpeculativeMajorityReadConcern(OperationContext* opCtx) const override {
        auto speculativeReadInfo = repl::SpeculativeMajorityReadInfo::get(opCtx);
        if (!speculativeReadInfo.isSpeculativeRead()) {
            return;
        }
        uassertStatusOK(mongo::waitForSpeculativeMajorityReadConcern(opCtx, speculativeReadInfo));
    }


    void waitForWriteConcern(OperationContext* opCtx,
                             const CommandInvocation* invocation,
                             const repl::OpTime& lastOpBeforeRun,
                             BSONObjBuilder& commandResponseBuilder) const override {

        // Prevent waiting for writeConcern if the command is changing only unreplicated namespaces.
        invariant(invocation);
        bool anyReplicatedNamespace = false;
        for (auto& ns : invocation->allNamespaces()) {
            if (ns.isReplicated()) {
                anyReplicatedNamespace = true;
                break;
            }
        }
        if (!anyReplicatedNamespace) {
            return;
        }

        // Do not increase consumption metrics during wait for write concern, as in serverless this
        // might cause a tenant to be billed for reading the oplog entry (which might be of
        // considerable size) of another tenant.
        ResourceConsumption::PauseMetricsCollectorBlock pauseMetricsCollection(opCtx);

        auto lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

        auto waitForWriteConcernAndAppendStatus = [&]() {
            WriteConcernResult res;
            auto waitForWCStatus =
                mongo::waitForWriteConcern(opCtx, lastOpAfterRun, opCtx->getWriteConcern(), &res);

            CommandHelpers::appendCommandWCStatus(commandResponseBuilder, waitForWCStatus, res);
        };

        // If lastOp has changed, then a write has been done by this client. This timestamp is
        // sufficient for waiting for write concern.
        if (lastOpAfterRun != lastOpBeforeRun) {
            invariant(lastOpAfterRun > lastOpBeforeRun);
            waitForWriteConcernAndAppendStatus();
            return;
        }

        // If an error occurs after performing a write but before waiting for write concern and
        // returning to the client, the driver may retry an operation that has already been
        // completed, resulting in a no-op. The no-op has to wait for the write concern nonetheless,
        // because acknowledgement from secondaries might still be pending. Given that the timestamp
        // of the original operation that performed the write is not available, the best
        // approximation is to use the system’s last op time, which is guaranteed to be >= than the
        // original op time.

        // Ensures that if we tried to do a write, we wait for write concern, even if that write was
        // a noop. We do not need to update this for multi-document transactions as read-only/noop
        // transactions will do a noop write at commit time, which should have incremented the
        // lastOp. And speculative majority semantics dictate that "abortTransaction" should not
        // wait for write concern on operations the transaction observed.
        if (shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite() &&
            !opCtx->inMultiDocumentTransaction()) {
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
            lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            waitForWriteConcernAndAppendStatus();
            return;
        }

        // Waits for write concern if we tried to explicitly set the lastOp forward but lastOp was
        // already up to date. We still want to wait for write concern on the lastOp. This is
        // primarily to make sure back to back retryable write retries still wait for write concern.
        //
        // WARNING: Retryable writes that expect to wait for write concern on retries must ensure
        // this is entered by calling setLastOp() or setLastOpToSystemLastOpTime().
        if (repl::ReplClientInfo::forClient(opCtx->getClient())
                .lastOpWasSetExplicitlyByClientForCurrentOperation(opCtx)) {
            waitForWriteConcernAndAppendStatus();
            return;
        }

        // If no write was attempted and the client's lastOp was not changed by the current network
        // operation then we skip waiting for writeConcern.
    }

    void waitForLinearizableReadConcern(OperationContext* opCtx) const override {
        // When a linearizable read command is passed in, check to make sure we're reading from the
        // primary.
        if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
            repl::ReadConcernLevel::kLinearizableReadConcern) {
            uassertStatusOK(mongo::waitForLinearizableReadConcern(opCtx, Milliseconds::zero()));
        }
    }

    void uassertCommandDoesNotSpecifyWriteConcern(
        const CommonRequestArgs& requestArgs) const override {
        uassert(ErrorCodes::InvalidOptions,
                "Command does not support writeConcern",
                !commandSpecifiesWriteConcern(requestArgs));
    }

    void attachCurOpErrInfo(OperationContext* opCtx, const BSONObj& replyObj) const override {
        CurOp::get(opCtx)->debug().errInfo = getStatusFromCommandResult(replyObj);
    }

    void appendReplyMetadata(OperationContext* opCtx,
                             const OpMsgRequest& request,
                             BSONObjBuilder* metadataBob) const override {
        auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
        const bool isReplSet = replCoord->getSettings().isReplSet();

        if (isReplSet) {
            // Attach our own last opTime.
            repl::OpTime lastOpTimeFromClient =
                repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            replCoord->prepareReplMetadata(request.body, lastOpTimeFromClient, metadataBob);
        }

        // Gossip back requested routing table cache versions.
        if (request.body.hasField(Generic_args_unstable_v1::kRequestGossipRoutingCacheFieldName)) {
            const auto collectionsToGossip =
                request.body.getField(Generic_args_unstable_v1::kRequestGossipRoutingCacheFieldName)
                    .Array();

            const auto catalogCache = Grid::get(opCtx)->catalogCache();

            BSONArrayBuilder arrayBuilder;
            for (const auto& collectionToGossip : collectionsToGossip) {
                const auto nss = NamespaceStringUtil::deserialize(
                    boost::none, collectionToGossip.String(), SerializationContext::stateDefault());
                const auto cachedCollectionVersion = catalogCache->peekCollectionCacheVersion(nss);
                if (cachedCollectionVersion) {
                    GossipedRoutingCache gossipedRoutingCache(nss, *cachedCollectionVersion);
                    arrayBuilder.append(gossipedRoutingCache.toBSON());
                }
            }

            if (arrayBuilder.arrSize() > 0) {
                metadataBob->appendArray(
                    Generic_reply_fields_unstable_v1::kRoutingCacheGossipFieldName,
                    arrayBuilder.obj());
            }
        }
    }

    bool refreshDatabase(OperationContext* opCtx,
                         const StaleDbRoutingVersion& se) const noexcept override {
        return onDbVersionMismatchNoExcept(opCtx, se.getDb(), se.getVersionReceived()).isOK();
    }

    bool refreshCollection(OperationContext* opCtx,
                           const StaleConfigInfo& se) const noexcept override {
        return onCollectionPlacementVersionMismatchNoExcept(
                   opCtx, se.getNss(), se.getVersionReceived().placementVersion())
            .isOK();
    }

    bool refreshCatalogCache(
        OperationContext* opCtx,
        const ShardCannotRefreshDueToLocksHeldInfo& refreshInfo) const noexcept override {
        return Grid::get(opCtx)
            ->catalogCache()
            ->getCollectionRoutingInfo(opCtx, refreshInfo.getNss())
            .isOK();
    }

    void handleReshardingCriticalSectionMetrics(OperationContext* opCtx,
                                                const StaleConfigInfo& se) const noexcept override {
        resharding_metrics::onCriticalSectionError(opCtx, se);
    }

    // The refreshDatabase, refreshCollection, and refreshCatalogCache methods may have modified the
    // locker state, in particular the flags which say if the operation took a write lock or shared
    // lock.  This will cause mongod to perhaps erroneously check for write concern when no writes
    // were done, or unnecessarily kill a read operation.  If we re-use the opCtx to retry command
    // execution, we must reset the locker state.
    void resetLockerState(OperationContext* opCtx) const noexcept override {
        // It is necessary to lock the client to change the Locker on the OperationContext.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        invariant(!shard_role_details::getLocker(opCtx)->isLocked());
        shard_role_details::swapLocker(
            opCtx, std::make_unique<Locker>(opCtx->getServiceContext()), lk);
    }

    std::unique_ptr<PolymorphicScoped> scopedOperationCompletionShardingActions(
        OperationContext* opCtx) const override {
        return std::make_unique<ScopedOperationCompletionShardingActions>(opCtx);
    }
};

ServiceEntryPointMongod::ServiceEntryPointMongod() : _hooks(std::make_unique<Hooks>()) {}

ServiceEntryPointMongod::~ServiceEntryPointMongod() = default;

Future<DbResponse> ServiceEntryPointMongod::_replicaSetEndpointHandleRequest(
    OperationContext* opCtx, const Message& m) noexcept try {
    // TODO (SERVER-81551): Move the OpMsgRequest parsing above ServiceEntryPoint::handleRequest().
    auto opMsgReq = rpc::opMsgRequestFromAnyProtocol(m, opCtx->getClient());
    if (m.operation() == dbQuery) {
        checkAllowedOpQueryCommand(*opCtx->getClient(), opMsgReq.getCommandName());
    }

    auto shouldRoute = replica_set_endpoint::shouldRouteRequest(opCtx, opMsgReq);
    LOGV2_DEBUG(8555601,
                3,
                "Using replica set endpoint",
                "opId"_attr = opCtx->getOpID(),
                "cmdName"_attr = opMsgReq.getCommandName(),
                "dbName"_attr = opMsgReq.getDatabaseNoThrow(),
                "cmdObj"_attr = redact(opMsgReq.body.toString()),
                "shouldRoute"_attr = shouldRoute);
    if (shouldRoute) {
        replica_set_endpoint::ScopedSetRouterService service(opCtx);
        return ServiceEntryPointMongos::handleRequestImpl(opCtx, m);
    }
    return ServiceEntryPointCommon::handleRequest(opCtx, m, *_hooks);
} catch (const DBException& ex) {
    // Try to generate a response based on the status. If encounter another error (e.g.
    // UnsupportedFormat) while trying to generate the response, just return the status.
    try {
        auto replyBuilder = rpc::makeReplyBuilder(rpc::protocolForMessage(m));
        replyBuilder->setCommandReply(ex.toStatus(), {});
        DbResponse dbResponse;
        dbResponse.response = replyBuilder->done();
        return dbResponse;
    } catch (...) {
    }
    return ex.toStatus();
}

Future<DbResponse> ServiceEntryPointMongod::handleRequest(OperationContext* opCtx,
                                                          const Message& m) noexcept {
    // TODO (SERVER-77921): Support for different ServiceEntryPoints based on role.
    if (replica_set_endpoint::isReplicaSetEndpointClient(opCtx->getClient())) {
        return _replicaSetEndpointHandleRequest(opCtx, m);
    }
    return ServiceEntryPointCommon::handleRequest(opCtx, m, *_hooks);
}

}  // namespace mongo
