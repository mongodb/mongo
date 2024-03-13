/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/optional/optional.hpp>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/catalog/type_index_catalog.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * Renames the global indexes metadata. This function should only be called after stopping
 * migrations and holding the critical section in all shards with data for userCollectionNss. This
 * function is not currently compatible with transactions.
 */
void renameCollectionShardingIndexCatalog(OperationContext* opCtx,
                                          const NamespaceString& fromNss,
                                          const NamespaceString& toNss,
                                          const Timestamp& indexVersion);

/**
 * Adds a new index entry into the in-memory catalog and persist it to disk. It effectively executes
 * two writes, so it should only be called after stopping migrations and holding the critical
 * section in all shards with data for userCollectionNss. This function is not currently compatible
 * with transactions.
 */
void addShardingIndexCatalogEntryToCollection(OperationContext* opCtx,
                                              const NamespaceString& userCollectionNss,
                                              const std::string& name,
                                              const BSONObj& keyPattern,
                                              const BSONObj& options,
                                              const UUID& collectionUUID,
                                              const Timestamp& lastmod,
                                              const boost::optional<UUID>& indexCollectionUUID);

/**
 * Removes the index identified by indexName from the catalog. This function updates the in-memory
 * state and the persisted state, so it should only be called after stopping migrations and holding
 * the critical section in all shards with data for nss. This function is not
 * currently compatible with transactions.
 */
void removeShardingIndexCatalogEntryFromCollection(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const UUID& uuid,
                                                   StringData indexName,
                                                   const Timestamp& lastmod);

/**
 * Removes all the indexes and the current index version, and replace them for the specified indexes
 * and indexVersion. This function should only be called after stopping migrations and holding the
 * critical section in all shards with data for userCollectionNss. This function is not currently
 * compatible with transactions.
 */
void replaceCollectionShardingIndexCatalog(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const UUID& uuid,
                                           const Timestamp& indexVersion,
                                           const std::vector<IndexCatalogType>& indexes);

/**
 * Drops all indexes and the collection entry. This function should only be called after stopping
 * migrations and holding the critical section in all shards with data for userCollectionNss. This
 * function is not currently compatible with transactions.
 */
void dropCollectionShardingIndexCatalog(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Removes all the indexes and unset the current index version. This function should only be called
 * after stopping migrations and holding the critical section in all shards with data for
 * userCollectionNss. This function is not currently compatible with transactions.
 */
void clearCollectionShardingIndexCatalog(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const UUID& uuid);

}  // namespace mongo
