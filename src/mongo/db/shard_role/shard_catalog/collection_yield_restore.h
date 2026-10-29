// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * Default implementation for restoring a CollectionPtr after yield. Requires at least the necessary
 * corresponding MODE_IS lock.
 */
class [[MONGO_MOD_USE_REPLACEMENT(TransactionResourcesStasher)]] LockedCollectionYieldRestore {
public:
    explicit LockedCollectionYieldRestore(OperationContext* opCtx, const CollectionPtr& coll);
    ConsistentCollection operator()(OperationContext* opCtx, boost::optional<UUID> optUuid) const;

private:
    const NamespaceString _nss;
};
}  // namespace mongo
