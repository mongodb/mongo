// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/txn_wildcard_multikey_paths.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/multikey_state.h"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

TxnWildcardMultikeyPaths& TxnWildcardMultikeyPaths::get(OperationContext* opCtx) {
    auto& slot = getMultikeyState(opCtx).wildcardPaths;
    if (!slot) {
        slot = std::make_unique<TxnWildcardMultikeyPaths>();
    }
    return *slot;
}

const TxnWildcardMultikeyPaths* TxnWildcardMultikeyPaths::tryGet(OperationContext* opCtx) {
    return getMultikeyState(opCtx).wildcardPaths.get();
}

void TxnWildcardMultikeyPaths::append(const UUID& collectionUuid,
                                      std::string_view indexName,
                                      const std::set<FieldRef>& paths) {
    if (paths.empty()) {
        return;
    }
    auto& bucket = _byIndex[std::make_pair(collectionUuid, std::string{indexName})];
    bucket.insert(paths.begin(), paths.end());
}

void TxnWildcardMultikeyPaths::appendMatchingPaths(const UUID& collectionUuid,
                                                   std::string_view indexName,
                                                   std::set<FieldRef>* out) const {
    auto it = _byIndex.find(std::make_pair(collectionUuid, std::string{indexName}));
    if (it == _byIndex.end()) {
        return;
    }
    // Return every cached path for the index. Over-reports vs the index scan with bounds used in
    // `scanWildcardMetadataKeys`. Safe because multikey is monotonic: extra paths can only push
    // the planner to be more conservative, never toward wrong results.
    //
    // TODO(SERVER-128059): mirror `getMultikeyPathIndexIntervalsForField` (point-prefix probe +
    // numeric-component range expansion).
    out->insert(it->second.begin(), it->second.end());
}

}  // namespace mongo
