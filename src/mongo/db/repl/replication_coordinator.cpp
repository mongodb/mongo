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


#include "mongo/db/repl/replication_coordinator.h"

#include "mongo/db/client.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

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

}  // namespace repl
}  // namespace mongo
