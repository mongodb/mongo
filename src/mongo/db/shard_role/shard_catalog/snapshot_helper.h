// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace SnapshotHelper {

/**
 * Whether the node was primary or not when the read source was determined.
 * kNotPrimary covers every non-primary replication state (SECONDARY, ROLLBACK, RECOVERING,
 * STARTUP, STARTUP2, REMOVED, ARBITER, UNKNOWN).
 */
enum class NodeRole { kPrimary, kNotPrimary };

/**
 * Diagnostic label for why a particular read source was chosen. Intended for logging / debugging;
 * does not drive control flow.
 */
enum class ReadSourceReason {
    kUnspecified,
    kSecondaryReadingReplicatedCollection,
    kUnreplicatedCollection,
    kPrimary,
    kNotPrimaryOrSecondary,
    kPinned,
    kSecondaryReadChangeNotNeeded,
    kAllowReadFromLatest,
};

std::string_view toString(ReadSourceReason reason);

/**
 * Captured state of the replication-driven read-source decision: the chosen ReadSource, the
 * write-acceptance role of the node at decision time, and a diagnostic reason. All three fields are
 * produced by the same probe of replication state and must be consumed together — never re-derive
 * any field independently.
 */
struct ReadSourceInfo {
    RecoveryUnit::ReadSource readSource;
    NodeRole nodeRole;
    ReadSourceReason reason = ReadSourceReason::kUnspecified;
};

/**
 * Returns the node's current role as kPrimary or kNotPrimary based on replication state.
 * Note that this function is not thread safe, and should only be called from either
 * - a pessimistic path where a db lock is held (and as consequence the RSTL is held)
 * - an optimistic path where a the replication term is re-checked after this call.
 * Both would ensure serialization with a concurrent stepdown/up.
 */
NodeRole getNodeRole(OperationContext* opCtx);

/**
 * Determines the read source that should be active for this operation based on replication
 * state and namespace. Pure query — does not modify the recovery unit.
 *
 * Returns kLastApplied if on a secondary (unless reading from latest on secondaries is explicitly
 * allowed, in which case it returns kNoTimestamp), kNoTimestamp if on a primary / unreplicated, or
 * boost::none if the override doesn't apply (read concern is not local/available).
 *
 */
boost::optional<ReadSourceInfo> getReadSourceForSecondaryReadsIfNeeded(
    OperationContext* opCtx, boost::optional<const NamespaceString&> nss);

/**
 * Attempts to apply the requested read source to the recovery unit and returns the ReadSourceInfo
 * actually in effect afterwards. The returned tuple may differ from `requested` when a
 * concurrent state change forces a downgrade (kLastApplied → kNoTimestamp) or when the
 * read source is pinned.
 */
ReadSourceInfo updateReadSourceTimestampForSecondaryReadsIfPossible(
    OperationContext* opCtx,
    boost::optional<const NamespaceString&> nss,
    const ReadSourceInfo& requested);

}  // namespace SnapshotHelper
}  // namespace mongo
