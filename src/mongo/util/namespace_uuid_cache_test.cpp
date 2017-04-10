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

#include "mongo/util/namespace_uuid_cache.h"

#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

TEST(NamespaceUUIDCache, ensureNamespaceInCache) {
    NamespaceUUIDCache cache;
    CollectionUUID uuid = CollectionUUID::gen();
    CollectionUUID uuidConflict = CollectionUUID::gen();
    NamespaceString nss("test", "test_collection_ns");
    // Add nss, uuid to cache.
    cache.ensureNamespaceInCache(nss, uuid);
    // Do nothing if we query for existing nss, uuid pairing.
    cache.ensureNamespaceInCache(nss, uuid);
    // Uassert if we query for existing nss and uuid that does not match.
    ASSERT_THROWS(cache.ensureNamespaceInCache(nss, uuidConflict), UserException);
}

TEST(NamespaceUUIDCache, onDropCollection) {
    NamespaceUUIDCache cache;
    CollectionUUID uuid = CollectionUUID::gen();
    CollectionUUID newUuid = CollectionUUID::gen();
    NamespaceString nss("test", "test_collection_ns");
    cache.ensureNamespaceInCache(nss, uuid);
    cache.onDropCollection(nss);
    // Add nss to the cache with a different uuid. This should not throw since
    // we evicted the previous entry from the cache.
    cache.ensureNamespaceInCache(nss, newUuid);
}
}  // namespace
