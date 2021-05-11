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

#pragma once

#include "mongo/db/record_id.h"
#include "mongo/util/uuid.h"

namespace mongo {

class Collection;
class Ident;
class OperationContext;
class NamespaceString;

namespace catalog {

/**
 * Performs two-phase index drop.
 *
 * Passthrough to DurableCatalog::removeIndex to execute the first phase of drop by removing the
 * index catalog entry, then registers an onCommit hook to schedule the second phase of drop to
 * delete the index data.
 *
 * Uses 'ident' shared_ptr to ensure that the second phase of drop (data table drop) will not
 * execute until no users of the index (shared owners) remain. 'ident' is allowed to be a nullptr,
 * in which case the caller guarantees that there are no remaining users of the index. This handles
 * situations wherein there is no in-memory state available for an index, such as during repair.
 */
void removeIndex(OperationContext* opCtx,
                 StringData indexName,
                 Collection* collection,
                 std::shared_ptr<Ident> ident);

/**
 * Performs two-phase collection drop.
 *
 * Passthrough to DurableCatalog::dropCollection to execute the first phase of drop by removing the
 * collection entry, then registers and onCommit hook to schedule the second phase of drop to delete
 * the collection data.
 *
 * Uses 'ident' shared_ptr to ensure that the second phase of drop (data table drop) will not
 * execute until no users of the collection record store (shared owners) remain. 'ident' is not
 * allowed to be nullptr.
 */
Status dropCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      RecordId collectionCatalogId,
                      std::shared_ptr<Ident> ident);


}  // namespace catalog
}  // namespace mongo
