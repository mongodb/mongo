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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <algorithm>

#include "namespace_uuid_cache.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

// TODO(geert): Enable checks by default
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(debugCollectionUUIDs, bool, false);

const OperationContext::Decoration<NamespaceUUIDCache> NamespaceUUIDCache::get =
    OperationContext::declareDecoration<NamespaceUUIDCache>();

void NamespaceUUIDCache::ensureNamespaceInCache(const NamespaceString& nss, CollectionUUID uuid) {
    StringData ns(nss.ns());
    CollectionUUIDMap::const_iterator it = _cache.find(ns);
    if (it == _cache.end()) {
        // Add ns, uuid pair to the cache if it does not yet exist.
        invariant(_cache.try_emplace(ns, uuid).second == true);
        LOG(3) << "NamespaceUUIDCache: registered namespace " << nss.ns() << " with UUID " << uuid;

    } else if (it->second != uuid) {
        // If ns exists in the cache, make sure it does not correspond to another uuid.
        auto msg = "Namespace " + ns + " now resolves to UUID " + uuid.toString() +
            " instead of UUID " + it->second.toString();
        LOG(1) << msg;
        uassert(40418, "Cannot continue operation: " + msg, !debugCollectionUUIDs);
    }
}

void NamespaceUUIDCache::evictNamespace(const NamespaceString& nss) {
    size_t evicted = _cache.erase(nss.ns());
    if (evicted) {
        LOG(2) << "NamespaceUUIDCache: evicted namespace " << nss.ns();
    }
    invariant(evicted <= 1);
}

void NamespaceUUIDCache::evictNamespacesInDatabase(StringData dbname) {
    for (auto&& it = _cache.begin(); it != _cache.end();) {
        auto entry = it++;
        if (entry->first.empty() || nsToDatabaseSubstring(entry->first) == dbname)
            _cache.erase(entry);
    }
}

}  // namespace mongo
