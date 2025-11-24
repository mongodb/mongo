/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/service_entry_point_shard_role_helpers.h"

#include "mongo/db/router_role/gossiped_routing_cache_gen.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/transaction/transaction_participant_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/namespace_string_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace service_entry_point_shard_role_helpers {

BSONObj getRedactedCopyForLogging(const Command* command, const BSONObj& cmdObj) {
    mutablebson::Document cmdToLog(cmdObj, mutablebson::Document::kInPlaceDisabled);
    command->snipForLogging(&cmdToLog);
    BSONObjBuilder bob;
    cmdToLog.writeTo(&bob);
    return bob.obj();
}

void waitForReadConcern(OperationContext* opCtx,
                        const CommandInvocation* invocation,
                        const OpMsgRequest& request) {
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
                        "db"_attr = invocation->db(),
                        "command"_attr = redact(
                            getRedactedCopyForLogging(invocation->definition(), request.body)),
                        "error"_attr = redact(rcStatus));
        }

        uassertStatusOK(rcStatus);
    }
}

void waitForWriteConcern(OperationContext* opCtx,
                         const CommandInvocation* invocation,
                         const repl::OpTime& lastOpBeforeRun,
                         BSONObjBuilder& commandResponseBuilder) {

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
    if (!opCtx->inMultiDocumentTransaction() &&
        shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite()) {
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        waitForWriteConcernAndAppendStatus();
        return;
    }

    // Aggregate and getMore requests can be read ops or write ops. We only want to wait for write
    // concern if the op could have done a write (i.e. had any write stages in its pipeline).
    // Aggregate::Invocation::isReadOperation will indicate whether the original agg request had
    // any write stages in its pipeline, but GetMore::Invocation::isReadOperation will not, so we
    // fall back to checking whether it took the global write lock for getMore.
    // Also, aggregate requests with write stages can be processed on secondaries if the read
    // concern specifies such. The secondariy will forward writes to the primary, and the primary
    // will wait for write concern. The secondary should not wait for write concern.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto skipSettingLastOpToSystemOp = (replCoord && replCoord->getSettings().isReplSet() &&
                                        !replCoord->getMemberState().primary()) ||
        (invocation->isReadOperation() &&
         invocation->definition()->getLogicalOp() != LogicalOp::opGetMore) ||
        (invocation->definition()->getLogicalOp() == LogicalOp::opGetMore &&
         !shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite());

    if (!skipSettingLastOpToSystemOp && !opCtx->inMultiDocumentTransaction()) {
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

void appendReplyMetadata(OperationContext* opCtx,
                         const GenericArguments& requestArgs,
                         BSONObjBuilder* metadataBob) {
    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet = replCoord->getSettings().isReplSet();

    if (isReplSet) {
        // Attach our own last opTime.
        repl::OpTime lastOpTimeFromClient =
            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        replCoord->prepareReplMetadata(requestArgs, lastOpTimeFromClient, metadataBob);
    }

    // Gossip back requested routing table cache versions.
    if (requestArgs.getRequestGossipRoutingCache()) {
        const auto collectionsToGossip = *requestArgs.getRequestGossipRoutingCache();

        const auto catalogCache = Grid::get(opCtx)->catalogCache();

        BSONArrayBuilder arrayBuilder;
        for (const auto& collectionToGossip : collectionsToGossip) {
            const auto nss =
                NamespaceStringUtil::deserialize(boost::none,
                                                 collectionToGossip.getElement().String(),
                                                 SerializationContext::stateDefault());
            const auto cachedCollectionVersion = catalogCache->peekCollectionCacheVersion(nss);
            if (cachedCollectionVersion) {
                GossipedRoutingCache gossipedRoutingCache(nss, *cachedCollectionVersion);
                arrayBuilder.append(gossipedRoutingCache.toBSON());
            }
        }

        if (arrayBuilder.arrSize() > 0) {
            metadataBob->appendArray(GenericReplyFields::kRoutingCacheGossipFieldName,
                                     arrayBuilder.obj());
        }
    }
}

void createTransactionCoordinator(OperationContext* opCtx,
                                  TxnNumber clientTxnNumber,
                                  boost::optional<TxnRetryCounter> clientTxnRetryCounter) {
    auto clientLsid = opCtx->getLogicalSessionId().value();
    auto& clockSource = opCtx->fastClockSource();

    // If this shard has been selected as the coordinator, set up the coordinator state
    // to be ready to receive votes.
    TransactionCoordinatorService::get(opCtx)->createCoordinator(
        opCtx,
        clientLsid,
        {clientTxnNumber, clientTxnRetryCounter ? *clientTxnRetryCounter : 0},
        clockSource.now() + Seconds(gTransactionLifetimeLimitSeconds.load()));
}

}  // namespace service_entry_point_shard_role_helpers
}  // namespace mongo
