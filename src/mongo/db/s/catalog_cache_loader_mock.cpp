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

#include "mongo/db/s/catalog_cache_loader_mock.h"

#include "mongo/db/operation_context.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/stdx/thread.h"

namespace mongo {

using CollectionAndChangedChunks = CatalogCacheLoader::CollectionAndChangedChunks;

namespace {

/**
 * Constructs the options for the loader thread pool.
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "CatalogCacheLoaderMock";
    options.minThreads = 0;
    options.maxThreads = 1;

    // Ensure all threads have a client.
    options.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };

    return options;
}

}  // namespace

CatalogCacheLoaderMock::CatalogCacheLoaderMock() : _threadPool(makeDefaultThreadPoolOptions()) {
    _threadPool.startup();
}

CatalogCacheLoaderMock::~CatalogCacheLoaderMock() {
    _threadPool.shutdown();
    _threadPool.join();
}

void CatalogCacheLoaderMock::initializeReplicaSetRole(bool isPrimary) {
    MONGO_UNREACHABLE;
}

void CatalogCacheLoaderMock::onStepDown() {
    MONGO_UNREACHABLE;
}

void CatalogCacheLoaderMock::onStepUp() {
    MONGO_UNREACHABLE;
}

void CatalogCacheLoaderMock::notifyOfCollectionVersionUpdate(const NamespaceString& nss) {
    MONGO_UNREACHABLE;
}

void CatalogCacheLoaderMock::waitForCollectionFlush(OperationContext* opCtx,
                                                    const NamespaceString& nss) {
    MONGO_UNREACHABLE;
}

void CatalogCacheLoaderMock::waitForDatabaseFlush(OperationContext* opCtx, StringData dbName) {
    MONGO_UNREACHABLE;
}

std::shared_ptr<Notification<void>> CatalogCacheLoaderMock::getChunksSince(
    const NamespaceString& nss, ChunkVersion version, GetChunksSinceCallbackFn callbackFn) {
    auto notify = std::make_shared<Notification<void>>();

    uassertStatusOK(_threadPool.schedule([ this, notify, callbackFn ]() noexcept {
        auto opCtx = Client::getCurrent()->makeOperationContext();

        auto swCollAndChunks = [&]() -> StatusWith<CollectionAndChangedChunks> {
            try {
                uassertStatusOK(_swCollectionReturnValue);
                uassertStatusOK(_swChunksReturnValue);

                return CollectionAndChangedChunks(
                    _swCollectionReturnValue.getValue().getUUID(),
                    _swCollectionReturnValue.getValue().getEpoch(),
                    _swCollectionReturnValue.getValue().getKeyPattern().toBSON(),
                    _swCollectionReturnValue.getValue().getDefaultCollation(),
                    _swCollectionReturnValue.getValue().getUnique(),
                    _swChunksReturnValue.getValue());
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        }();

        callbackFn(opCtx.get(), std::move(swCollAndChunks));
        notify->set();
    }));

    return notify;
}

void CatalogCacheLoaderMock::getDatabase(
    StringData dbName,
    stdx::function<void(OperationContext*, StatusWith<DatabaseType>)> callbackFn) {
    // Not implemented
}

void CatalogCacheLoaderMock::setCollectionRefreshReturnValue(
    StatusWith<CollectionType> statusWithCollectionType) {
    _swCollectionReturnValue = std::move(statusWithCollectionType);
}

void CatalogCacheLoaderMock::setChunkRefreshReturnValue(
    StatusWith<std::vector<ChunkType>> statusWithChunks) {
    _swChunksReturnValue = std::move(statusWithChunks);
}

}  // namespace mongo
