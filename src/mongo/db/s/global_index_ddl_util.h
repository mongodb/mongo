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

#include "mongo/db/operation_context.h"

namespace mongo {

/**
 * Adds a new index entry into the in-memory catalog and persist it to disk. It effectively executes
 * two writes, so it should only be called after stopping migrations and holding the critical
 * section in all shards with data for userCollectionNss. This function is not currently compatible
 * with transactions.
 */
void addGlobalIndexCatalogEntryToCollection(OperationContext* opCtx,
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
 * the critical section in all shards with data for userCollectionNss. This function is not
 * currently compatible with transactions.
 */
void removeGlobalIndexCatalogEntryFromCollection(OperationContext* opCtx,
                                                 const NamespaceString& userCollectionNss,
                                                 const UUID& collectionUUID,
                                                 const std::string& indexName,
                                                 const Timestamp& lastmod);

}  // namespace mongo
