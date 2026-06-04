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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_bypass.h"
#include "mongo/idl/idl_parser.h"
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
    _replicaSetWritesBlockCounters[static_cast<size_t>(reason)].fetchAndAdd(1);
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

void ReplicaSetWriteBlockState::checkReplicaSetWritesAllowed(
    OperationContext* opCtx,
    const NamespaceString& nss,
    ReplicaSetWriteBlockRejectedWriteOp opType) const {
    const auto info = _writeBlockInfo.load();
    const bool writesAllowed = !info.blocked ||
        ReplicaSetWriteBlockBypass::get(opCtx).isEnabled() || nss.isOnInternalDb() ||
        nss.isTemporaryReshardingCollection() || nss.isSystemDotProfile();
    if (!writesAllowed) {
        switch (opType) {
            case ReplicaSetWriteBlockRejectedWriteOp::kInsert:
                _replicaSetWriteBlockRejectedInserts.fetchAndAdd(1);
                break;
            case ReplicaSetWriteBlockRejectedWriteOp::kUpdate:
                _replicaSetWriteBlockRejectedUpdates.fetchAndAdd(1);
                break;
        }
        uasserted(
            ErrorCodes::ReplicaSetWritesBlocked,
            fmt::format("Replica set write blocked, reason: {}", idl::serialize(info.reason)));
    }
}

bool ReplicaSetWriteBlockState::isReplicaSetWriteBlockingEnabled() const {
    return _writeBlockInfo.load().blocked;
}

Status ReplicaSetWriteBlockState::checkIfCompactAllowedToStart(OperationContext* opCtx) const {
    // Compact is gated by the allowDeletions flag: it is blocked only when replica set deletions
    // are blocked, and is permitted when deletions are allowed.
    if (_deletionsBlocked.load() && !ReplicaSetWriteBlockBypass::get(opCtx).isEnabled()) {
        const auto info = _writeBlockInfo.load();
        return Status(ErrorCodes::ReplicaSetWritesBlocked,
                      fmt::format("Compact blocked because replica set deletions are blocked, "
                                  "reason: {}",
                                  idl::serialize(info.reason)));
    }
    return Status::OK();
}

void ReplicaSetWriteBlockState::enableReplicaSetDeletionsBlocking() {
    _deletionsBlocked.store(true);
}

void ReplicaSetWriteBlockState::disableReplicaSetDeletionsBlocking() {
    _deletionsBlocked.store(false);
}

void ReplicaSetWriteBlockState::checkReplicaSetDeletionsAllowed(OperationContext* opCtx,
                                                                const NamespaceString& nss) const {
    const bool deletesAllowed = !_deletionsBlocked.load() ||
        ReplicaSetWriteBlockBypass::get(opCtx).isEnabled() || nss.isOnInternalDb() ||
        nss.isSystemDotProfile();
    if (!deletesAllowed) {
        const auto info = _writeBlockInfo.load();
        _replicaSetWriteBlockRejectedDeletes.fetchAndAdd(1);
        uasserted(
            ErrorCodes::ReplicaSetWritesBlocked,
            fmt::format("Replica set writes blocked, reason: {}", idl::serialize(info.reason)));
    }
}

bool ReplicaSetWriteBlockState::isReplicaSetDeletionsBlockingEnabled_forTest() const {
    return _deletionsBlocked.load();
}

void ReplicaSetWriteBlockState::appendReplicaSetWritesBlockCounters(BSONObjBuilder& bob) const {
    BSONObjBuilder result(bob.subobjStart("replicaSetWritesBlockCounters"));
    for (size_t reasonIdx = 0; reasonIdx < idlEnumCount<ReplicaSetWritesBlockReasonEnum>;
         ++reasonIdx) {
        result.appendNumber(
            idl::serialize(static_cast<ReplicaSetWritesBlockReasonEnum>(reasonIdx)),
            static_cast<long long>(_replicaSetWritesBlockCounters[reasonIdx].load()));
    }
}

void ReplicaSetWriteBlockState::appendReplicaSetWriteBlockRejectionMetrics(
    BSONObjBuilder& bob) const {
    BSONObjBuilder sub(bob.subobjStart("replicaSetWritesBlockRejected"));
    sub.appendNumber("inserts",
                     static_cast<long long>(_replicaSetWriteBlockRejectedInserts.load()));
    sub.appendNumber("updates",
                     static_cast<long long>(_replicaSetWriteBlockRejectedUpdates.load()));
    sub.appendNumber("deletes",
                     static_cast<long long>(_replicaSetWriteBlockRejectedDeletes.load()));
}

}  // namespace mongo
