// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/versioning_protocol/chunk_version.h"

#include <boost/optional.hpp>

namespace mongo {
namespace shard_catalog_recoverer {

/**
 * Result of one authoritative metadata recovery attempt. Callers loop on kRetry.
 */
enum class AttemptResult {
    kDone,
    kRetry,
};

/**
 * Handles an authoritative collection shard-version mismatch for 'nss', recovering collection
 * metadata and reconciling it with 'receivedShardVersion' when provided.
 *
 * Returns kRetry when the caller should re-enter after a transient error (critical section,
 * FCV transition, etc.).
 */
AttemptResult onShardVersionMismatchAuthoritative(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<ChunkVersion> receivedShardVersion);

}  // namespace shard_catalog_recoverer
}  // namespace mongo
