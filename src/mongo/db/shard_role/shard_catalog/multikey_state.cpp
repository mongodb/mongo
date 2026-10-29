// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/multikey_state.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/txn_wildcard_multikey_paths.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/decorable.h"

namespace mongo {
namespace {

const auto getMultikeyStateDecoration = RecoveryUnit::Snapshot::declareDecoration<MultikeyState>();

}  // namespace

MultikeyState& getMultikeyState(OperationContext* opCtx) {
    return getMultikeyStateDecoration(shard_role_details::getRecoveryUnit(opCtx)->getSnapshot());
}

}  // namespace mongo
