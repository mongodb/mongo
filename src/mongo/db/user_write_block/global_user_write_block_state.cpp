/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/db/user_write_block/global_user_write_block_state.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/server_options.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/user_write_block/write_block_bypass.h"
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
    LOGV2(
        10296100, "Blocking user writes", "reason"_attr = UserWritesBlockReason_serializer(reason));
}

void GlobalUserWriteBlockState::disableUserWriteBlocking(OperationContext* opCtx) {
    _globalUserWritesBlocked.store(false);
    auto reason = _globalUserWritesBlockedReason.swap(UserWritesBlockReasonEnum::kUnspecified);
    LOGV2(10296101,
          "Unblocking user writes",
          "reason"_attr = UserWritesBlockReason_serializer(reason));
}

void GlobalUserWriteBlockState::checkUserWritesAllowed(OperationContext* opCtx,
                                                       const NamespaceString& nss) const {
    invariant(shard_role_details::getLocker(opCtx)->isLocked());
    uassert(
        ErrorCodes::UserWritesBlocked,
        str::stream() << "User writes blocked, reason: "
                      << UserWritesBlockReason_serializer(_globalUserWritesBlockedReason.load()),
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
            UserWritesBlockReason_serializer(static_cast<UserWritesBlockReasonEnum>(reasonIdx)),
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
