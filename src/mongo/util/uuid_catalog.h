/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <unordered_map>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/service_context.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * This class comprises a UUID to collection catalog, allowing for efficient
 * collection lookup by UUID.
 */
using CollectionUUID = UUID;

class UUIDCatalog {
    MONGO_DISALLOW_COPYING(UUIDCatalog);

public:
    static const ServiceContext::Decoration<UUIDCatalog> get;
    UUIDCatalog() = default;

    /* This function inserts the entry for uuid, coll into the UUID
     * Collection. It is called by the op observer when a collection
     * is created.
     */
    void onCreateCollection(OperationContext* opCtx, Collection* coll, CollectionUUID uuid);

    /* This function gets the Collection* pointer that corresponds to
     * CollectionUUID uuid. The required locks should be obtained prior
     * to calling this function, or else the found Collection pointer
     * might no longer be valid when the call returns.
     */
    Collection* lookupCollectionByUUID(CollectionUUID uuid);

    /* This function gets the NamespaceString from the Collection* pointer that
     * corresponds to CollectionUUID uuid. If there is no such pointer, an empty
     * NamespaceString is returned.
     */
    NamespaceString lookupNSSByUUID(CollectionUUID uuid);

    /* This function removes the entry for uuid from the UUID catalog. It
     * is called by the op observer when a collection is dropped.
     */
    void onDropCollection(OperationContext* opCtx, CollectionUUID uuid);

private:
    mongo::stdx::mutex _catalogLock;
    mongo::stdx::unordered_map<CollectionUUID, Collection*, CollectionUUID::Hash> _catalog;

    void _registerUUIDCatalogEntry(CollectionUUID uuid, Collection* coll);
    Collection* _removeUUIDCatalogEntry(CollectionUUID uuid);
};

}  // namespace mongo
