// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/router_role/routing_cache/shard_cannot_refresh_due_to_locks_held_exception.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string_util.h"

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(ShardCannotRefreshDueToLocksHeldInfo);

}  // namespace

void ShardCannotRefreshDueToLocksHeldInfo::serialize(BSONObjBuilder* bob) const {
    bob->append(kNssFieldName,
                NamespaceStringUtil::serialize(_nss, SerializationContext::stateDefault()));
}

std::shared_ptr<const ErrorExtraInfo> ShardCannotRefreshDueToLocksHeldInfo::parse(
    const BSONObj& obj) {
    return std::make_shared<ShardCannotRefreshDueToLocksHeldInfo>(parseFromCommandError(obj));
}

ShardCannotRefreshDueToLocksHeldInfo ShardCannotRefreshDueToLocksHeldInfo::parseFromCommandError(
    const BSONObj& obj) {
    return ShardCannotRefreshDueToLocksHeldInfo(NamespaceStringUtil::deserialize(
        boost::none, obj[kNssFieldName].String(), SerializationContext::stateDefault()));
}

}  // namespace mongo
