// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/uncommitted_multikey.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/multikey_state.h"

namespace mongo {

UncommittedMultikey& UncommittedMultikey::get(OperationContext* opCtx) {
    return getMultikeyState(opCtx).uncommittedMultikey;
}

}  // namespace mongo
