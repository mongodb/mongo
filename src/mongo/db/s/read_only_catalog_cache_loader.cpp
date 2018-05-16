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

#include "mongo/platform/basic.h"

#include "mongo/db/s/read_only_catalog_cache_loader.h"

namespace mongo {

using CollectionAndChangedChunks = CatalogCacheLoader::CollectionAndChangedChunks;

void ReadOnlyCatalogCacheLoader::waitForCollectionFlush(OperationContext* opCtx,
                                                        const NamespaceString& nss) {
    MONGO_UNREACHABLE;
}

void ReadOnlyCatalogCacheLoader::waitForDatabaseFlush(OperationContext* opCtx, StringData dbName) {
    MONGO_UNREACHABLE;
}

std::shared_ptr<Notification<void>> ReadOnlyCatalogCacheLoader::getChunksSince(
    const NamespaceString& nss, ChunkVersion version, GetChunksSinceCallbackFn callbackFn) {
    return _configServerLoader.getChunksSince(nss, version, callbackFn);
}

void ReadOnlyCatalogCacheLoader::getDatabase(
    StringData dbName,
    stdx::function<void(OperationContext*, StatusWith<DatabaseType>)> callbackFn) {
    return _configServerLoader.getDatabase(dbName, callbackFn);
}

}  // namespace mongo
