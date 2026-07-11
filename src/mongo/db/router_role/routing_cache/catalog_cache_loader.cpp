// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/router_role/routing_cache/catalog_cache_loader.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

CatalogCacheLoader::CollectionAndChangedChunks::CollectionAndChangedChunks() = default;

CatalogCacheLoader::CollectionAndChangedChunks::CollectionAndChangedChunks(
    OID epoch,
    Timestamp timestamp,
    UUID collUuid,
    bool unsplittable,
    const BSONObj& collShardKeyPattern,
    const BSONObj& collDefaultCollation,
    bool collShardKeyIsUnique,
    boost::optional<TypeCollectionTimeseriesFields> collTimeseriesFields,
    boost::optional<TypeCollectionReshardingFields> collReshardingFields,
    bool allowMigrations,
    std::vector<ChunkType> chunks)
    : epoch(std::move(epoch)),
      timestamp(std::move(timestamp)),
      uuid(std::move(collUuid)),
      unsplittable(unsplittable),
      shardKeyPattern(collShardKeyPattern),
      defaultCollation(collDefaultCollation),
      shardKeyIsUnique(collShardKeyIsUnique),
      timeseriesFields(std::move(collTimeseriesFields)),
      reshardingFields(std::move(collReshardingFields)),
      allowMigrations(allowMigrations),
      changedChunks(std::move(chunks)) {}

CatalogCacheLoader::CatalogCacheLoader() = default;

CatalogCacheLoader::~CatalogCacheLoader() = default;

}  // namespace mongo
