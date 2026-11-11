// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo ::offline_validate {

/**
 * Runs offline validation over all collections in one or all databases, or over a single
 * collection, according to the --validateDbName and --validateCollectionName startup options.
 * Throws OfflineValidationFailedToComplete if validation did not complete for some collections.
 */
void runOfflineValidation(OperationContext* opCtx,
                          boost::optional<Lock::GlobalWrite>&& globalWriteLock);

}  // namespace mongo::offline_validate
