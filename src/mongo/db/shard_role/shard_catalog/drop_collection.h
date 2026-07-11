// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/shard_role/ddl/drop_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <boost/optional/optional.hpp>

namespace mongo {
class NamespaceString;
class OperationContext;

namespace repl {
class OpTime;
}  // namespace repl

enum class [[MONGO_MOD_NEEDS_REPLACEMENT]] DropCollectionSystemCollectionMode {
    kDisallowSystemCollectionDrops,
    kAllowSystemCollectionDrops
};

/**
 * Drops the collection "collectionName" and populates "reply" with statistics about what
 * was removed. Aborts in-progress index builds on the collection if two phase index builds are
 * supported. Throws if the expectedUUID does not match the UUID of the collection being dropped.
 * When fromMigrate is set, the related oplog entry will be marked accordingly using the
 * 'fromMigrate' field to reduce its visibility (e.g. in change streams).
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status dropCollection(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    const boost::optional<UUID>& expectedUUID,
    DropReply* reply,
    DropCollectionSystemCollectionMode systemCollectionMode,
    bool fromMigrate = false);

[[MONGO_MOD_NEEDS_REPLACEMENT]] Status dropCollection(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    DropReply* reply,
    DropCollectionSystemCollectionMode systemCollectionMode,
    bool fromMigrate = false);

/**
 * Drops the collection with the given namespace only if its uuid is not matching 'expectedUUID'.
 * When 'fromMigrate' is set, the related oplog entry will be marked accordingly using the
 * 'fromMigrate' field to reduce its visibility (e.g. in change streams).
 */
[[MONGO_MOD_PARENT_PRIVATE]] Status dropCollectionIfUUIDNotMatching(OperationContext* opCtx,
                                                                    const NamespaceString& ns,
                                                                    const UUID& expectedUUID,
                                                                    bool fromMigrate);

/**
 * Drops the collection "collectionName". When applying a 'drop' oplog entry on a secondary, the
 * 'dropOpTime' will contain the optime of the oplog entry. When 'markFromMigrate' is set, the
 * related oplog entry will be marked accordingly to reduce its visibility in change streams.
 * Note: 'markFromMigrate' is only meaningful when 'collectionName' refers to a collection, not a
 * view.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status dropCollectionForApplyOps(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    const repl::OpTime& dropOpTime,
    DropCollectionSystemCollectionMode systemCollectionMode,
    bool markFromMigrate = false);

/**
 * If we are in a replset, every replicated collection must have an _id index. Issues a warning if
 * one is not found.
 *
 * The caller must have the database locked in X mode.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void checkForIdIndexes(OperationContext* opCtx,
                                                       const DatabaseName& dbName);

/**
 * Deletes all temporary collections under the specified database.
 *
 * The caller must have the database locked in at least IX mode.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void clearTempCollections(OperationContext* opCtx,
                                                          const DatabaseName& dbName);

/**
 * Checks that the namespace complies with naming restrictions and therefore can be dropped. It
 * returns a Status with details of that evaluation.
 */
[[MONGO_MOD_PARENT_PRIVATE]] Status isDroppableCollection(OperationContext* opCtx,
                                                          const NamespaceString& nss);

}  // namespace mongo
