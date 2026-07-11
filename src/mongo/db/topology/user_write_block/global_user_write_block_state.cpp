// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/topology/user_write_block/global_user_write_block_state.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/user_write_block/user_write_block_bypass.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace {
const auto serviceDecorator = ServiceContext::declareDecoration<GlobalUserWriteBlockState>();
}  // namespace

GlobalUserWriteBlockState* GlobalUserWriteBlockState::get(ServiceContext* serviceContext) {
    return &serviceDecorator(serviceContext);
}

GlobalUserWriteBlockState* GlobalUserWriteBlockState::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void GlobalUserWriteBlockState::enableUserWriteBlocking(OperationContext* opCtx,
                                                        UserWritesBlockReasonEnum reason) {
    _globalUserWritesBlockedReason.store(reason);
    _globalUserWritesBlocked.store(true);
    _globalUserWriteBlockCounters[static_cast<size_t>(reason)].fetchAndAdd(1);
    LOGV2(10296100, "Blocking user writes", "reason"_attr = idl::serialize(reason));
}

void GlobalUserWriteBlockState::disableUserWriteBlocking(OperationContext* opCtx) {
    _globalUserWritesBlocked.store(false);
    auto reason = _globalUserWritesBlockedReason.swap(UserWritesBlockReasonEnum::kUnspecified);
    LOGV2(10296101, "Unblocking user writes", "reason"_attr = idl::serialize(reason));
}

void GlobalUserWriteBlockState::checkUserWritesAllowed(OperationContext* opCtx,
                                                       const NamespaceString& nss) const {
    invariant(shard_role_details::getLocker(opCtx)->isLocked());
    uassert(ErrorCodes::UserWritesBlocked,
            str::stream() << "User writes blocked, reason: "
                          << idl::serialize(_globalUserWritesBlockedReason.load()),
            !_globalUserWritesBlocked.load() ||
                WriteBlockBypass::get(opCtx).isWriteBlockBypassEnabled() || nss.isOnInternalDb() ||
                nss.isTemporaryReshardingCollection() || nss.isSystemDotProfile());
}

bool GlobalUserWriteBlockState::isUserWriteBlockingEnabled(OperationContext* opCtx) const {
    invariant(shard_role_details::getLocker(opCtx)->isLocked());
    return _globalUserWritesBlocked.load();
}

void GlobalUserWriteBlockState::appendUserWriteBlockModeCounters(BSONObjBuilder& bob) const {
    BSONObjBuilder result(bob.subobjStart("userWriteBlockModeCounters"));
    for (size_t reasonIdx = 0; reasonIdx < idlEnumCount<UserWritesBlockReasonEnum>; ++reasonIdx) {
        result.appendNumber(
            idl::serialize(static_cast<UserWritesBlockReasonEnum>(reasonIdx)),
            static_cast<long long>(_globalUserWriteBlockCounters[reasonIdx].load()));
    }
}

void GlobalUserWriteBlockState::enableUserShardedDDLBlocking(OperationContext* opCtx) {
    _userShardedDDLBlocked.store(true);
}

void GlobalUserWriteBlockState::disableUserShardedDDLBlocking(OperationContext* opCtx) {
    _userShardedDDLBlocked.store(false);
}

void GlobalUserWriteBlockState::checkShardedDDLAllowedToStart(OperationContext* opCtx,
                                                              const NamespaceString& nss) const {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));
    uassert(ErrorCodes::UserWritesBlocked,
            "User writes blocked",
            !_userShardedDDLBlocked.load() ||
                WriteBlockBypass::get(opCtx).isWriteBlockBypassEnabled() || nss.isOnInternalDb());
}

void GlobalUserWriteBlockState::enableUserIndexBuildBlocking(OperationContext* opCtx) {
    _userIndexBuildsBlocked.store(true);
}

void GlobalUserWriteBlockState::disableUserIndexBuildBlocking(OperationContext* opCtx) {
    _userIndexBuildsBlocked.store(false);
}

Status GlobalUserWriteBlockState::checkIfIndexBuildAllowedToStart(
    OperationContext* opCtx, const NamespaceString& nss) const {
    if (_userIndexBuildsBlocked.load() &&
        !WriteBlockBypass::get(opCtx).isWriteBlockBypassEnabled() && !nss.isOnInternalDb()) {
        return Status(ErrorCodes::UserWritesBlocked, "User writes blocked");
    }
    return Status::OK();
}

}  // namespace mongo
