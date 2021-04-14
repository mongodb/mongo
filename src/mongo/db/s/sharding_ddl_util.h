/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace sharding_ddl_util {

/**
 * Generic utility to send a command to a list of shards. Throws if one of the commands fails.
 */
void sendAuthenticatedCommandToShards(OperationContext* opCtx,
                                      StringData dbName,
                                      const BSONObj& command,
                                      const std::vector<ShardId>& shardIds,
                                      const std::shared_ptr<executor::TaskExecutor>& executor);

/**
 * Erase tags metadata from config server for the given namespace.
 */
void removeTagsMetadataFromConfig(OperationContext* opCtx, const NamespaceString& nss);


/**
 * Erase collection metadata from config server and invalidate the locally cached one.
 * In particular remove chunks, tags and the description associated with the given namespace.
 */
void removeCollMetadataFromConfig(OperationContext* opCtx, const CollectionType& coll);

/**
 * Erase collection metadata from config server and invalidate the locally cached one.
 * In particular remove chunks, tags and the description associated with the given namespace.
 *
 * Returns true if the collection existed before being removed.
 */
bool removeCollMetadataFromConfig(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Rename sharded collection metadata as part of a renameCollection operation.
 *
 * - Update namespace associated with tags (FROM -> TO)
 * - Update FROM collection entry to TO
 *
 * This function is idempotent.
 */
void shardedRenameMetadata(OperationContext* opCtx,
                           CollectionType& fromCollType,
                           const NamespaceString& toNss);

/**
 * Ensures rename preconditions for sharded collections are met:
 * - Check that `dropTarget` is true if the destination collection exists
 * - Check that no tags exist for the destination collection
 */
void checkShardedRenamePreconditions(OperationContext* opCtx,
                                     const NamespaceString& toNss,
                                     const bool dropTarget);

/**
 * Throws if the DB primary shards of the provided namespaces differs.
 *
 * Optimistically assume that no movePrimary is performed during the check: it's currently not
 * possible to ensure primary shard stability for both databases.
 */
void checkDbPrimariesOnTheSameShard(OperationContext* opCtx,
                                    const NamespaceString& fromNss,
                                    const NamespaceString& toNss);

/**
 * Throws an exception if the collection is already sharded with different options.
 *
 * If the collection is already sharded with the same options, returns the existing collection's
 * full spec, else returns boost::none.
 */
boost::optional<CreateCollectionResponse> checkIfCollectionAlreadySharded(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& key,
    const BSONObj& collation,
    bool unique);

/**
 * Acquires the collection critical section in the catch-up phase (i.e. blocking writes) for the
 * specified namespace and reason. It works even if the namespace's current metadata are UNKNOWN.
 *
 * It adds a doc to config.collectionCriticalSections with with writeConcern write concern.
 *
 * Do nothing if the collection critical section is taken for that nss and reason, and will
 * invariant otherwise since it is the responsibility of the caller to ensure that only one thread
 * is taking the critical section.
 */
void acquireRecoverableCriticalSectionBlockWrites(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& reason,
    const WriteConcernOptions& writeConcern,
    const boost::optional<BSONObj>& additionalInfo = boost::none);

/**
 * Advances the recoverable critical section from the catch-up phase (i.e. blocking writes) to the
 * commit phase (i.e. blocking reads) for the specified nss and reason. The recoverable critical
 * section must have been acquired first through 'acquireRecoverableCriticalSectionBlockWrites'
 * function.
 *
 * It updates a doc from config.collectionCriticalSections with writeConcern write concern.
 *
 * Do nothing if the collection critical section is already taken in commit phase.
 */
void acquireRecoverableCriticalSectionBlockReads(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const BSONObj& reason,
                                                 const WriteConcernOptions& writeConcern);

/**
 * Releases the recoverable critical section for the given nss and reason.
 *
 * It removes a doc from config.collectionCriticalSections with writeConcern write concern.
 *
 * Do nothing if the collection critical section is not taken for that nss and reason.
 */
void releaseRecoverableCriticalSection(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const BSONObj& reason,
                                       const WriteConcernOptions& writeConcern);

/**
 * Retakes the in-memory collection critical section for each recoverable critical section
 * persisted on config.collectionCriticalSections. It also clears the filtering metadata.
 */
void retakeInMemoryRecoverableCriticalSections(OperationContext* opCtx);

/**
 * Stops ongoing migrations and prevents future ones to start for the given nss.
 */
void stopMigrations(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Locally drops a collection and cleans its CollectionShardingRuntime metadata
 */
DropReply dropCollectionLocally(OperationContext* opCtx, const NamespaceString& nss);

}  // namespace sharding_ddl_util
}  // namespace mongo
