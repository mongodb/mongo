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

#include "mongo/s/catalog_cache_loader.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

/**
 * Mocks the metadata refresh results with settable return values. The purpose of this class is to
 * facilitate testing of classes that use a CatalogCacheLoader.
 */
class CatalogCacheLoaderMock final : public CatalogCacheLoader {
    MONGO_DISALLOW_COPYING(CatalogCacheLoaderMock);

public:
    CatalogCacheLoaderMock();
    ~CatalogCacheLoaderMock();

    /**
     * These functions should never be called. They trigger invariants if called.
     */
    void initializeReplicaSetRole(bool isPrimary) override;
    void onStepDown() override;
    void onStepUp() override;
    void notifyOfCollectionVersionUpdate(const NamespaceString& nss) override;
    void waitForCollectionFlush(OperationContext* opCtx, const NamespaceString& nss) override;

    std::shared_ptr<Notification<void>> getChunksSince(
        const NamespaceString& nss,
        ChunkVersion version,
        stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)> callbackFn)
        override;

    /**
     * Sets the mocked collection entry result that getChunksSince will use to construct its return
     * value.
     */
    void setCollectionRefreshReturnValue(StatusWith<CollectionType> statusWithCollectionType);

    /**
     * Sets the mocked chunk results that getChunksSince will use to construct its return value.
     */
    void setChunkRefreshReturnValue(StatusWith<std::vector<ChunkType>> statusWithChunks);

private:
    // These variables hold the mocked chunks and collection entry results used to construct the
    // return value of getChunksSince above.
    StatusWith<CollectionType> _swCollectionReturnValue{Status(
        ErrorCodes::InternalError, "config loader mock collection response is uninitialized")};

    StatusWith<std::vector<ChunkType>> _swChunksReturnValue{
        Status(ErrorCodes::InternalError, "config loader mock chunks response is uninitialized")};

    // Thread pool on which to mock load chunk metadata.
    ThreadPool _threadPool;
};

}  // namespace mongo
