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

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace sharding_ddl_util {

/**
 * Erase tags metadata from config server for the given namespace.
 */
void removeTagsMetadataFromConfig(OperationContext* opCtx, const NamespaceString& nss);


/**
 * Erase collection metadata from config server and invalidate the locally cached once.
 * In particular remove chunks, tags and the description associated with the given namespace.
 */
void removeCollMetadataFromConfig(OperationContext* opCtx, const CollectionType& coll);

/**
 * Rename sharded collection metadata as part of a renameCollection operation.
 *
 * Transaction:
 * - Update config.collections entry: update nss and epoch.
 * - Update config.chunks entries: change epoch/timestamp.
 */
void shardedRenameMetadata(OperationContext* opCtx,
                           const NamespaceString& fromNss,
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
 * Acquires the critical section for the specified namespace.
 * It works even if the namespace's current metadata are UNKNOWN.
 */
void acquireCriticalSection(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Releases the critical section for the specified namespace.
 */
void releaseCriticalSection(OperationContext* opCtx, const NamespaceString& nss);

}  // namespace sharding_ddl_util
}  // namespace mongo
