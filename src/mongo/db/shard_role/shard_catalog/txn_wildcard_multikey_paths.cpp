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
