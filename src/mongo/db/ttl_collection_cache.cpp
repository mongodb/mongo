/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/ttl_collection_cache.h"

#include <algorithm>

#include "mongo/db/service_context.h"

namespace mongo {

namespace {
const auto getTTLCollectionCache = ServiceContext::declareDecoration<TTLCollectionCache>();
}

TTLCollectionCache& TTLCollectionCache::get(ServiceContext* ctx) {
    return getTTLCollectionCache(ctx);
}

void TTLCollectionCache::registerCollection(const NamespaceString& collectionNS) {
    stdx::lock_guard<stdx::mutex> lock(_ttlCollectionsLock);
    _ttlCollections.push_back(collectionNS.ns());
}

void TTLCollectionCache::unregisterCollection(const NamespaceString& collectionNS) {
    stdx::lock_guard<stdx::mutex> lock(_ttlCollectionsLock);
    auto collIter = std::find(_ttlCollections.begin(), _ttlCollections.end(), collectionNS.ns());
    fassert(40220, collIter != _ttlCollections.end());
    _ttlCollections.erase(collIter);
}

std::vector<std::string> TTLCollectionCache::getCollections() {
    stdx::lock_guard<stdx::mutex> lock(_ttlCollectionsLock);
    return _ttlCollections;
}
};  // namespace mongo
