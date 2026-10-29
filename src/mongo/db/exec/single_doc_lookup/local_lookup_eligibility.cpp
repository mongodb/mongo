// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility.h"

namespace mongo::exec::agg {

boost::optional<ScopedSetShardRole> createScopedShardRole(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const LocalLookupEligibility::Local& local) {
    boost::optional<ScopedSetShardRole> versioned;
    if (local.shardVersion || local.dbVersion) {
        versioned.emplace(opCtx, nss, local.shardVersion, local.dbVersion);
    }
    return versioned;
}

}  // namespace mongo::exec::agg
