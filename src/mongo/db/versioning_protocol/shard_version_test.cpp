// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/versioning_protocol/shard_version.h"

#include "mongo/bson/oid.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

TEST(ShardVersionTest, ConstructCorrectly) {
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 2));
    const ChunkVersion chunkVersion(gen, {3, 4});
    const ShardVersion shardVersion = ShardVersionFactory::make(chunkVersion);
    ASSERT_EQ(shardVersion.placementVersion().getTimestamp(), Timestamp(1, 2));
    ASSERT_EQ(shardVersion.placementVersion().majorVersion(), 3);
    ASSERT_EQ(shardVersion.placementVersion().minorVersion(), 4);
}

TEST(ShardVersionTest, ToAndFromBSON) {
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 2));
    const ChunkVersion chunkVersion(gen, {3, 4});
    ShardVersion shardVersion = ShardVersionFactory::make(chunkVersion);
    shardVersion.setPlacementConflictTime_DEPRECATED(LogicalTime(Timestamp(7, 8)));

    BSONObjBuilder builder;
    shardVersion.serialize(ShardVersion::kShardVersionField, &builder);
    const auto obj = builder.obj();

    const auto fromBSON = ShardVersion::parse(obj[ShardVersion::kShardVersionField]);
    ASSERT_EQ(fromBSON, shardVersion);
}

}  // namespace
}  // namespace mongo
