/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/topology/user_write_block/replica_set_write_block_state.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_bypass.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {

namespace {
const auto serviceDecorator = ServiceContext::declareDecoration<ReplicaSetWriteBlockState>();
}  // namespace

ReplicaSetWriteBlockState* ReplicaSetWriteBlockState::get(ServiceContext* serviceContext) {
    return &serviceDecorator(serviceContext);
}

ReplicaSetWriteBlockState* ReplicaSetWriteBlockState::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void ReplicaSetWriteBlockState::enableReplicaSetWriteBlocking(
    ReplicaSetWritesBlockReasonEnum reason) {
    _writeBlockInfo.store(WriteBlockInfo{.blocked = true, .reason = reason});
    LOGV2(12097000, "Blocking replica set writes", "reason"_attr = idl::serialize(reason));
}

void ReplicaSetWriteBlockState::disableReplicaSetWriteBlocking() {
    auto previousInfo = _writeBlockInfo.swap(WriteBlockInfo{});
    if (previousInfo.blocked) {
        LOGV2(12097001,
              "Unblocking replica set writes",
              "reason"_attr = idl::serialize(previousInfo.reason));
    } else {
        LOGV2(12097003,
              "disableReplicaSetWriteBlocking called but replica set writes were not blocked");
    }
}

void ReplicaSetWriteBlockState::checkReplicaSetWritesAllowed(OperationContext* opCtx,
                                                             const NamespaceString& nss) const {
    const auto info = _writeBlockInfo.load();
    uassert(ErrorCodes::UserWritesBlocked,
            fmt::format("Replica set write blocked, reason: {}", idl::serialize(info.reason)),
            !info.blocked || ReplicaSetWriteBlockBypass::get(opCtx).isEnabled() ||
                nss.isOnInternalDb() || nss.isTemporaryReshardingCollection() ||
                nss.isSystemDotProfile());
}

bool ReplicaSetWriteBlockState::isReplicaSetWriteBlockingEnabled() const {
    return _writeBlockInfo.load().blocked;
}

void ReplicaSetWriteBlockState::enableReplicaSetDeletionsBlocking() {
    _deletionsBlocked.store(true);
}

void ReplicaSetWriteBlockState::disableReplicaSetDeletionsBlocking() {
    _deletionsBlocked.store(false);
}

void ReplicaSetWriteBlockState::checkReplicaSetDeletionsAllowed(OperationContext* opCtx,
                                                                const NamespaceString& nss) const {
    uassert(ErrorCodes::UserWritesBlocked,
            "User writes blocked",
            !_deletionsBlocked.load() || ReplicaSetWriteBlockBypass::get(opCtx).isEnabled() ||
                nss.isOnInternalDb() || nss.isSystemDotProfile());
}

bool ReplicaSetWriteBlockState::isReplicaSetDeletionsBlockingEnabled_forTest() const {
    return _deletionsBlocked.load();
}

}  // namespace mongo
