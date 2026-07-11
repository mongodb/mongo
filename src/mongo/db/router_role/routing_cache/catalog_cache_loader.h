// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Interface through which the sharding catalog cache requests the set of changed chunks to be
 * retrieved from the persisted metadata store.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] CatalogCacheLoader {
public:
    virtual ~CatalogCacheLoader();

    /**
     * Used as a return value for getChunksSince.
     */
    struct CollectionAndChangedChunks {
        CollectionAndChangedChunks();
        CollectionAndChangedChunks(
            OID epoch,
            Timestamp timestamp,
            UUID uuid,
            bool unsplittable,
            const BSONObj& collShardKeyPattern,
            const BSONObj& collDefaultCollation,
            bool collShardKeyIsUnique,
            boost::optional<TypeCollectionTimeseriesFields> collTimeseriesFields,
            boost::optional<TypeCollectionReshardingFields> collReshardingFields,
            bool allowMigrations,
            std::vector<ChunkType> chunks);

        // Information about the entire collection
        OID epoch;
        Timestamp timestamp;
        boost::optional<UUID> uuid;  // This value can never be boost::none,
                                     // except under the default constructor

        bool unsplittable;

        BSONObj shardKeyPattern;
        BSONObj defaultCollation;
        bool shardKeyIsUnique;

        // This information will be valid if the collection is a time-series buckets collection.
        boost::optional<TypeCollectionTimeseriesFields> timeseriesFields;

        // If the collection is currently undergoing a resharding operation, the optional will be
        // populated.
        boost::optional<TypeCollectionReshardingFields> reshardingFields;

        bool allowMigrations;

        // The chunks which have changed sorted by their chunkVersion. This list might potentially
        // contain all the chunks in the collection.
        std::vector<ChunkType> changedChunks;
    };

    /**
     * Transitions into shut down and cleans up state. Once this transitions to shut down, should
     * not be able to transition back to normal. Should be safe to be called more than once.
     */
    virtual void shutDown() = 0;

    /**
     * Non-blocking call, which returns the chunks changed since the specified version to be
     * fetched from the persistent metadata store.
     *
     * If for some reason the asynchronous fetch operation cannot be dispatched (for example on
     * shutdown), throws a DBException.
     */
    virtual SemiFuture<CollectionAndChangedChunks> getChunksSince(const NamespaceString& nss,
                                                                  ChunkVersion version) = 0;

    /**
     * Non-blocking call, which returns the most recent db version for the given dbName from the
     * persistent metadata store.
     *
     * If for some reason the asynchronous fetch operation cannot be dispatched (for example on
     * shutdown), throws a DBException.
     */
    virtual SemiFuture<DatabaseType> getDatabase(const DatabaseName& dbName) = 0;

protected:
    CatalogCacheLoader();
};

}  // namespace mongo

#define LOGV2_FOR_CATALOG_REFRESH(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                                    \
        ID, DLEVEL, {logv2::LogComponent::kShardingCatalogRefresh}, MESSAGE, ##__VA_ARGS__)
