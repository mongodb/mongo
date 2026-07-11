// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"

namespace mongo {

/**
 * Returns whether the replicated fast count collection is enabled on the provided operation
 * context.
 */
// TODO(SERVER-119896): Delete this function and rename library.
[[MONGO_MOD_PUBLIC]] bool isReplicatedFastCountEnabled(OperationContext* opCtx);

/**
 * Returns whether the provided namespace string can be tracked by the replicated fast count
 * collection.
 */
[[MONGO_MOD_PUBLIC]] bool isReplicatedFastCountEligible(const NamespaceString& nss);

/**
 * Returns true if we should get the size and count for the specified 'nss' from the replicated fast
 * count system.
 *
 * Returns false for local collections and implicitly replicated collections.
 *
 * If the persistence provider uses replicated fast count, returns true for collections that are
 * eligible to be tracked by replicated fast count.
 *
 * If the persistence provider does not use replicated fast count but the feature flag for
 * replicated fast count is enabled, returns true for collections that are eligible to be tracked by
 * replicated fast count, excluding the oplog collection, as long as the collection is a replica
 * set.
 */
[[MONGO_MOD_PUBLIC]] bool shouldReadFromReplicatedFastCount(OperationContext* opCtx,
                                                            const NamespaceString& nss);

/**
 * Returns true if size metadata and timestamps are persisted in containers instead of collections.
 */
[[MONGO_MOD_PUBLIC]] bool shouldUseReplicatedFastCountContainers(OperationContext* opCtx);

/**
 * Returns true if replicated fast count metadata should be emitted in listCollections output.
 */
[[MONGO_MOD_PUBLIC]] bool isReplicatedFastCountListCollectionsEnabled(OperationContext* opCtx);

/**
 * Returns true if initial sync should fetch the replicated fast count timestamp store timestamp.
 */
[[MONGO_MOD_PUBLIC]] bool isReplicatedFastCountInitialSyncEnabled(OperationContext* opCtx);

}  // namespace mongo
