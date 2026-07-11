// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/ordering.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>

namespace mongo {

/**
 * Per-transaction cache of wildcard multikey paths written. Transactions eagerly replicate
 * multikeyness changes in side transactions, including wildcard multikey metadata index entries.
 * The parent transaction must be able to see those changes in subsequent statements, so we cache
 * them here.
 *
 * The cache lives inside the parent transaction's RecoveryUnit::Snapshot MultikeyState decoration,
 * so it:
 *   - Survives statement boundaries via TxnResources RU stash/unstash.
 *   - Is cleaned up when the parent RU is destroyed on transaction commit or abort.
 */
class [[MONGO_MOD_PUBLIC]] TxnWildcardMultikeyPaths {
public:
    /**
     * Returns the cache attached to the transaction's RecoveryUnit Snapshot for the given
     * OperationContext. Lazily constructs the cache on first call — never null.
     *
     * Prefer `tryGet()` on hot read paths where the cache may not exist yet, to avoid paying the
     * construction cost when there is nothing to read.
     */
    static TxnWildcardMultikeyPaths& get(OperationContext* opCtx);

    /**
     * Returns a pointer to the cache attached to the transaction's RecoveryUnit Snapshot, or
     * nullptr if no caller has populated it yet. Does NOT construct the cache. Use on read paths
     * that should be free when no side-committed wildcard multikey writes have happened.
     */
    static const TxnWildcardMultikeyPaths* tryGet(OperationContext* opCtx);

    /**
     * Record paths from a side-committed wildcard multikey write so subsequent reads on the
     * parent transaction can see them (RYOW).
     */
    void append(const UUID& collectionUuid,
                std::string_view indexName,
                const std::set<FieldRef>& paths);

    /**
     * Append every cached multikey path for `(collectionUuid, indexName)` into `out`.
     *
     * Safe to call when the cache is empty (no-op).
     */
    void appendMatchingPaths(const UUID& collectionUuid,
                             std::string_view indexName,
                             std::set<FieldRef>* out) const;

    /**
     * Returns true if no side-committed wildcard multikey paths have been recorded against this
     * snapshot.
     */
    bool empty() const {
        return _byIndex.empty();
    }

private:
    absl::flat_hash_map<std::pair<UUID, std::string>, std::set<FieldRef>> _byIndex;
};

}  // namespace mongo
