/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/operation_context.h"

namespace mongo {

class Collection;
class CollectionPtr;
class CollectionCatalogEntry;

namespace catalog {

/**
 * Returns ErrorCodes::NamespaceExists if a collection or any type of views exists on the given
 * namespace 'nss'. Otherwise returns Status::OK().
 *
 * Note: If the caller calls this method without locking the collection, then the returned result
 * could be stale right after this call.
 */
Status checkIfNamespaceExists(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Iterates through all the collections in the given database and runs the callback function on each
 * collection. If a predicate is provided, then the callback will only be executed against the
 * collections that satisfy the predicate.
 *
 * Additionally, no collection lock is held while checking the outcome of the predicate. The
 * predicate must not block, as an internal collection catalog mutex is held during its evaluation.
 * The collection lock is acquired when executing the callback only on the satisfying collections.
 *
 * Iterating through the remaining collections stops when the callback returns false.
 */
void forEachCollectionFromDb(OperationContext* opCtx,
                             const TenantDatabaseName& tenantDbName,
                             LockMode collLockMode,
                             CollectionCatalog::CollectionInfoFn callback,
                             CollectionCatalog::CollectionInfoFn predicate = nullptr);

}  // namespace catalog
}  // namespace mongo
