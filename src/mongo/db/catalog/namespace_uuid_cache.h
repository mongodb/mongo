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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo {

extern bool debugCollectionUUIDs;

/**
 * This class comprises the NamespaceString to UUID cache that prevents given namespaces
 * from resolving to multiple UUIDs.
 */
using CollectionUUID = UUID;
class Database;

class NamespaceUUIDCache {
    MONGO_DISALLOW_COPYING(NamespaceUUIDCache);

public:
    static const OperationContext::Decoration<NamespaceUUIDCache> get;
    NamespaceUUIDCache() = default;

    /**
     * This function adds the pair nss.ns(), uuid to the namespace UUID cache
     * if it does not yet exist. If nss.ns() already exists in the cache with
     * a different UUID, a AssertionException is thrown, so we can guarantee that
     * an operation will always resolve the same name to the same collection,
     * even in presence of drops and renames.
     */
    void ensureNamespaceInCache(const NamespaceString& nss, CollectionUUID uuid);

    /**
     * This function removes the entry for nss.ns() from the namespace UUID
     * cache. Does nothing if the entry doesn't exist. It is called via the
     * op observer when a collection is dropped or renamed.
     */
    void evictNamespace(const NamespaceString& nss);

    /**
     * Same as above, but for all registered namespaces in the given dbname.
     */
    void evictNamespacesInDatabase(StringData dbname);

    /**
     * For testing only: verify that 'nss' is not cached.
     */
    void verifyNotCached(const NamespaceString& nss, CollectionUUID uuid) {
        auto it = _cache.find(nss.ns());
        invariant(it == _cache.end() || it->second != uuid);
    }

private:
    using CollectionUUIDMap = StringMap<CollectionUUID>;
    CollectionUUIDMap _cache;
};
}  // namespace mongo
