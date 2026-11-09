// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Returns whether a collection is in scope for set-wide replicated RecordIds: explicitly
 * replicated and non-clustered.
 *
 * Clustered collections are out of scope because their RecordId is derived from the '_id', so
 * explicit RecordId replication is unnecessary. Implicitly replicated namespaces (e.g.
 * config.transactions, config.image_collection, change stream pre-images) are out of scope
 * because they only replicate a subset of their writes, and those writes carry no 'rid'.
 *
 * This is the collection-scope half of the replicated RecordIds consumption predicate, shared by
 * oplog 'rid' emission, oplog application, initial sync cloning, dbhash, and restore tooling.
 * Node-scope participation is decided separately by each caller.
 */
[[MONGO_MOD_PUBLIC]] bool isReplicatedRecordIdsInScope(const NamespaceString& nss,
                                                       bool isClustered);

}  // namespace mongo
