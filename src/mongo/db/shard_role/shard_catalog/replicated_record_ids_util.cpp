// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/replicated_record_ids_util.h"

namespace mongo {

bool isReplicatedRecordIdsInScope(const NamespaceString& nss, bool isClustered) {
    return nss.isReplicated() && !nss.isImplicitlyReplicated() && !isClustered;
}

}  // namespace mongo
