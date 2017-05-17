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
#include "mongo/s/chunk_version.h"
#include "mongo/util/concurrency/notification.h"

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
     * Used as a return value for getChunksSince.
     */
    struct CollectionAndChangedChunks {
        // Information about the entire collection
        OID epoch;
        BSONObj shardKeyPattern;
        BSONObj defaultCollation;
        bool shardKeyIsUnique;

        // The chunks which have changed sorted by their chunkVersion. This list might potentially
        // contain all the chunks in the collection.
        std::vector<ChunkType> changedChunks;
    };

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
        const NamespaceString& nss,
        ChunkVersion version,
        stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)>
            callbackFn) = 0;

protected:
    CatalogCacheLoader() = default;
};

}  // namespace mongo
