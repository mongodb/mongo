// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/storage/key_format.h"

#include <string_view>

namespace mongo {
/**
 * Sets up the internal collections that are used to persist replicated fast count metadata and
 * starts up the replicated fast count manager thread. This function is idempotent if the fast count
 * stores already exist. Throws an exception if the internal collections or containers cannot be
 * created.
 *
 * When trying to create containers, this will throw if one container does not exist and the other
 * exists and is non-empty. Since the metadata and timestamp containers are always written to in the
 * same transaction, such a state is indicative of some form of data corruption.
 */
[[MONGO_MOD_PUBLIC]] void setUpReplicatedFastCount(OperationContext* opCtx);

[[MONGO_MOD_PUBLIC]] Status createInternalFastCountContainers(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              std::string_view metadataIdent,
                                                              KeyFormat metadataKeyFormat,
                                                              std::string_view timestampsIdent,
                                                              KeyFormat timestampsKeyFormat,
                                                              bool writeToOplog);

/**
 * Handles an ident with the provided `existingIdentFormat` existing in the storage engine.
 *
 * This function is used to handle an ident create operation that results in
 * `ObjectAlreadyExists`. If the ident is non-empty, `handeExistingFastCountIdent()` returns an
 * error code 12309402.
 *
 * Until layered table drops are implemented properly, it is possible for an ident and its contents
 * to get dropped but for WiredTiger to report an `ObjectAlreadyExists` error when you try to create
 * a new table with that same ident. In that case, the ident can be treated as newly created iff
 * it is empty.
 */
[[MONGO_MOD_PUBLIC]] std::pair<Status, std::string> handleExistingFastCountIdent(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::string_view existingIdent,
    KeyFormat existingIdentFormat);

namespace replicated_fast_count {
/**
 * Creates the replicated fast count collection using the global namespace string
 * kReplicatedFastCountStore.
 */
Status createReplicatedFastCountCollection(repl::StorageInterface* storageInterface,
                                           OperationContext* opCtx);

/**
 * Creates the replicated fast count timestamp collection using the global namespace string
 * kReplicatedFastCountStoreTimestamps.
 */
Status createReplicatedFastCountTimestampCollection(repl::StorageInterface* storageInterface,
                                                    OperationContext* opCtx);
}  // namespace replicated_fast_count
}  // namespace mongo
