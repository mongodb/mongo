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

#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/chunk_version.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/uuid.h"

namespace mongo {

class NamespaceString;
class OperationContext;

/**
 * Interface through which the sharding catalog cache requests the set of changed chunks to be
 * retrieved from the persisted metadata store.
 */
class CatalogCacheLoader {
public:
    virtual ~CatalogCacheLoader() = default;

    /**
     * Stores a loader on the specified service context. May only be called once for the lifetime of
     * the service context.
     */
    static void set(ServiceContext* serviceContext, std::unique_ptr<CatalogCacheLoader> loader);

    static CatalogCacheLoader& get(ServiceContext* serviceContext);
    static CatalogCacheLoader& get(OperationContext* opCtx);

    /**
     * Used as a return value for getChunksSince.
     */
    struct CollectionAndChangedChunks {
        CollectionAndChangedChunks();
        CollectionAndChangedChunks(boost::optional<UUID> uuid,
                                   const OID& collEpoch,
                                   const BSONObj& collShardKeyPattern,
                                   const BSONObj& collDefaultCollation,
                                   bool collShardKeyIsUnique,
                                   std::vector<ChunkType> chunks);

        // Information about the entire collection
        boost::optional<UUID> uuid;
        OID epoch;
        BSONObj shardKeyPattern;
        BSONObj defaultCollation;
        bool shardKeyIsUnique{false};

        // The chunks which have changed sorted by their chunkVersion. This list might potentially
        // contain all the chunks in the collection.
        std::vector<ChunkType> changedChunks;
    };

    using GetChunksSinceCallbackFn =
        stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)>;

    /**
     * Initializes internal state. Must be called only once when sharding state is initialized.
     */
    virtual void initializeReplicaSetRole(bool isPrimary) = 0;

    /**
     * Changes internal state on step down.
     */
    virtual void onStepDown() = 0;

    /**
     * Changes internal state on step up.
     */
    virtual void onStepUp() = 0;

    /**
     * Notifies the loader that the persisted collection version for 'nss' has been updated.
     */
    virtual void notifyOfCollectionVersionUpdate(const NamespaceString& nss) = 0;

    /**
     * Non-blocking call, which requests the chunks changed since the specified version to be
     * fetched from the persistent metadata store and invokes the callback function with the result.
     * The callback function must never throw - it is a fatal error to do so.
     *
     * If for some reason the asynchronous fetch operation cannot be dispatched (for example on
     * shutdown), throws a DBException. Otherwise it is guaranteed that the callback function will
     * be invoked even on error and the returned notification will be signalled.
     *
     * The callbackFn object must not be destroyed until it has been called. The returned
     * Notification object can be waited on in order to ensure that.
     */
    virtual std::shared_ptr<Notification<void>> getChunksSince(
        const NamespaceString& nss, ChunkVersion version, GetChunksSinceCallbackFn callbackFn) = 0;

    /**
     * Non-blocking call, which requests the most recent db version for the given dbName from the
     * the persistent metadata store and invokes the callback function with the result.
     * The callback function must never throw - it is a fatal error to do so.
     *
     * If for some reason the asynchronous fetch operation cannot be dispatched (for example on
     * shutdown), throws a DBException. Otherwise it is guaranteed that the callback function will
     * be invoked even on error.
     *
     * The callbackFn object must not be destroyed until it has been called.
     */
    virtual void getDatabase(
        StringData dbName,
        stdx::function<void(OperationContext*, StatusWith<DatabaseType>)> callbackFn) = 0;

    /**
     * Waits for any pending changes for the specified collection to be persisted locally (not
     * necessarily replicated). If newer changes come after this method has started running, they
     * will not be waited for except if there is a drop.
     *
     * May throw if the node steps down from primary or if the operation time is exceeded or due to
     * any other error condition.
     *
     * If the specific loader implementation does not support persistence, this method is undefined
     * and must fassert.
     */
    virtual void waitForCollectionFlush(OperationContext* opCtx, const NamespaceString& nss) = 0;

    virtual void waitForDatabaseFlush(OperationContext* opCtx, StringData dbName) = 0;

    /**
     * Only used for unit-tests, clears a previously-created catalog cache loader from the specified
     * service context, so that 'create' can be called again.
     */
    static void clearForTests(ServiceContext* serviceContext);

protected:
    CatalogCacheLoader() = default;
};

}  // namespace mongo
