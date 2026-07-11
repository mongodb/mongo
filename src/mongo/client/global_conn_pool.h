// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/connpool.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Global connection pool (used by all references to the internal DB client).
 */
extern DBConnectionPool globalConnPool;

}  // namespace mongo
