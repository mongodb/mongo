// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata_recoverer.h"
#include "mongo/db/versioning_protocol/database_version.h"

namespace mongo {
namespace shard_catalog_recoverer {

/**
 * Handles an authoritative database version mismatch for 'dbName', reconciling with
 * 'receivedDbVersion'.
 *
 * Returns kRetry when the caller should re-enter after a transient error (critical section, FCV
 * transition, etc.).
 */
AttemptResult onDbVersionMismatchAuthoritative(OperationContext* opCtx,
                                               const DatabaseName& dbName,
                                               const DatabaseVersion& receivedDbVersion);

}  // namespace shard_catalog_recoverer
}  // namespace mongo
