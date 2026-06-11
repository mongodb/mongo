/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/bson/ordering.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <set>
#include <string>
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
class MONGO_MOD_PUBLIC TxnWildcardMultikeyPaths {
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
    void append(const UUID& collectionUuid, StringData indexName, const std::set<FieldRef>& paths);

    /**
     * Append every cached multikey path for `(collectionUuid, indexName)` into `out`.
     *
     * Safe to call when the cache is empty (no-op).
     */
    void appendMatchingPaths(const UUID& collectionUuid,
                             StringData indexName,
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
