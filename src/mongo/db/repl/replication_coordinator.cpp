// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/replication_coordinator.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {


namespace {
const auto getReplicationCoordinator =
    ServiceContext::declareDecoration<std::unique_ptr<ReplicationCoordinator>>();
}

ReplicationCoordinator::ReplicationCoordinator() {}
ReplicationCoordinator::~ReplicationCoordinator() {}

ReplicationCoordinator* ReplicationCoordinator::get(ServiceContext* service) {
    return getReplicationCoordinator(service).get();
}

ReplicationCoordinator* ReplicationCoordinator::get(ServiceContext& service) {
    return getReplicationCoordinator(service).get();
}

ReplicationCoordinator* ReplicationCoordinator::get(OperationContext* ctx) {
    return get(ctx->getClient()->getServiceContext());
}


void ReplicationCoordinator::set(ServiceContext* service,
                                 std::unique_ptr<ReplicationCoordinator> replCoord) {
    auto& coordinator = getReplicationCoordinator(service);
    coordinator = std::move(replCoord);
}

bool ReplicationCoordinator::isOplogDisabledFor(OperationContext* opCtx,
                                                const NamespaceString& nss) const {
    if (!getSettings().isReplSet()) {
        return true;
    }

    if (!opCtx->writesAreReplicated()) {
        return true;
    }

    if (ReplicationCoordinator::isOplogDisabledForNS(nss)) {
        return true;
    }

    // Magic restore performs writes to replicated collections (e.g in the config DB) that we don't
    // want replicated via the oplog.
    if (storageGlobalParams.magicRestore) {
        return true;
    }

    fassert(28626, shard_role_details::getRecoveryUnit(opCtx));

    return false;
}

void ReplicationCoordinator::setOldestTimestamp(const Timestamp& timestamp) {
    getServiceContext()->getStorageEngine()->setOldestTimestamp(timestamp, false /*force*/);
}

bool ReplicationCoordinator::isOplogDisabledForNS(const NamespaceString& nss) {
    if (!nss.isReplicated()) {
        return true;
    }

    return false;
}

bool ReplicationCoordinator::isInInitialSyncOrRollback() const {
    if (!getSettings().isReplSet()) {
        return false;
    }

    const auto memberState = getMemberState();
    return memberState.startup2() || memberState.rollback();
}

bool ReplicationCoordinator::shouldUseEmptyOplogBatchToPropagateCommitPoint(
    OpTime clientOpTime) const {
    if (!repl::allowEmptyOplogBatchesToPropagateCommitPoint) {
        return false;
    }

    // For getMore operations with a last committed opTime, we should not wait if our
    // lastCommittedOpTime has progressed past the client's lastCommittedOpTime. In that case,
    // we will return early so that we can inform the client of the new lastCommittedOpTime
    // immediately.
    return clientOpTime < getLastCommittedOpTime();
}

bool ReplicationCoordinator::shouldParkHelloAwaitingTopologyChange(
    const TopologyVersion& currentTopologyVersion,
    const boost::optional<TopologyVersion>& clientTopologyVersion,
    std::int64_t lastHorizonTopologyChange) {
    if (!clientTopologyVersion) {
        // The client is not using awaitable hello so we respond immediately.
        return false;
    }
    if (clientTopologyVersion->getProcessId() != currentTopologyVersion.getProcessId()) {
        // Getting a different process id indicates that the server has restarted so we return
        // immediately with the updated process id.
        return false;
    }
    auto clientCounter = clientTopologyVersion->getCounter();
    auto serverCounter = currentTopologyVersion.getCounter();
    uassert(31382,
            str::stream() << "Received a topology version with counter: " << clientCounter
                          << " which is greater than the server topology version counter: "
                          << serverCounter,
            clientCounter <= serverCounter);
    if (clientCounter < serverCounter) {
        uassert(ErrorCodes::SplitHorizonChange,
                "Stale horizon detected, we have since received a reconfig that changed the "
                "horizon mappings.",
                clientCounter >= lastHorizonTopologyChange);
        // The received hello command contains a stale topology version so we respond
        // immediately with a more current topology version.
        return false;
    }
    return true;
}

}  // namespace repl
}  // namespace mongo
