// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

namespace mongo {
class DatabaseName;
class OperationContext;

/**
 * Drops the database "dbName". Aborts in-progress index builds on each collection in the database
 * if two-phase index builds are enabled.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status dropDatabase(OperationContext* opCtx,
                                                    const DatabaseName& dbName,
                                                    bool markFromMigrate = false);

/**
 * Drops the database "dbName". Does not abort in-progress index builds.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status dropDatabaseForApplyOps(OperationContext* opCtx,
                                                               const DatabaseName& dbName,
                                                               bool markFromMigrate = false);
}  // namespace mongo
