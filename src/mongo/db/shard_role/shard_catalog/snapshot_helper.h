/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/modules.h"

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

StringData toString(ReadSourceReason reason);

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
