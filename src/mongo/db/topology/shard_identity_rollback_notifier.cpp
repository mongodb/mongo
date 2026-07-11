// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/shard_identity_rollback_notifier.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {

namespace {
const auto getRollbackNotifier = ServiceContext::declareDecoration<ShardIdentityRollbackNotifier>();
}  // namespace

ShardIdentityRollbackNotifier::ShardIdentityRollbackNotifier() = default;

ShardIdentityRollbackNotifier* ShardIdentityRollbackNotifier::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

ShardIdentityRollbackNotifier* ShardIdentityRollbackNotifier::get(ServiceContext* opCtx) {
    return &getRollbackNotifier(opCtx);
}


}  // namespace mongo
