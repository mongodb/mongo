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

#include "namespace_uuid_cache.h"

#include "mongo/util/assert_util.h"

namespace mongo {

const OperationContext::Decoration<NamespaceUUIDCache> NamespaceUUIDCache::get =
    OperationContext::declareDecoration<NamespaceUUIDCache>();

void NamespaceUUIDCache::ensureNamespaceInCache(const NamespaceString& nss, CollectionUUID uuid) {
    StringData ns(nss.ns());
    CollectionUUIDMap::const_iterator it = _cache.find(ns);
    if (it == _cache.end()) {
        // Add ns, uuid pair to the cache if it does not yet exist.
        invariant(_cache.try_emplace(ns, uuid).second == true);
    } else {
        // If ns exists in the cache, make sure it does not correspond to another uuid.
        uassert(40418,
                "Cannot continue operation on namespace " + ns + ": it now resolves " +
                    uuid.toString() + " instead of " + it->second.toString(),
                it->second == uuid);
    }
}
void NamespaceUUIDCache::onDropCollection(const NamespaceString& nss) {
    _evictNamespace(nss);
}

void NamespaceUUIDCache::onRenameCollection(const NamespaceString& nss) {
    _evictNamespace(nss);
}

void NamespaceUUIDCache::_evictNamespace(const NamespaceString& nss) {
    invariant(_cache.erase(nss.ns()) <= 1);
}
}  // namespace mongo
