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

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/storage/key_format.h"

namespace mongo {
/**
 * Sets up the internal collections that are used to persist replicated fast count metadata and
 * starts up the replicated fast count manager thread. Throws an exception if the internal
 * collections cannot be created.
 */
MONGO_MOD_PUBLIC void setUpReplicatedFastCount(OperationContext* opCtx);

MONGO_MOD_PUBLIC Status createInternalFastCountContainers(OperationContext* opCtx,
                                                          const NamespaceString& nss,
                                                          StringData metadataIdent,
                                                          KeyFormat metadataKeyFormat,
                                                          StringData timestampsIdent,
                                                          KeyFormat timestampsKeyFormat,
                                                          bool writeToOplog);

MONGO_MOD_PUBLIC std::pair<Status, std::string> handleExistingFastCountIdent(
    OperationContext* opCtx,
    const NamespaceString& nss,
    StringData existingIdent,
    KeyFormat existingIdentFormat,
    StringData nonExistentIdent);

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
