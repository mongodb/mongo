// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/lock_manager/dump_lock_manager.h"

#include "mongo/base/shim.h"

namespace mongo {

void dumpLockManager() {
    static auto w = MONGO_WEAK_FUNCTION_DEFINITION(dumpLockManager);
    return w();
}

}  // namespace mongo
