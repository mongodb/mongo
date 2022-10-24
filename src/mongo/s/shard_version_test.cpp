/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#include "mongo/s/shard_version.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ShardVersionTest, ConstructCorrectly) {
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 2));
    const ChunkVersion chunkVersion(gen, {3, 4});
    const CollectionIndexes collectionIndexes(UUID::gen(), Timestamp(5, 6));
    const ShardVersion shardVersion(chunkVersion, collectionIndexes);
    ASSERT_EQ(shardVersion.placementVersion().getTimestamp(), Timestamp(1, 2));
    ASSERT_EQ(shardVersion.placementVersion().majorVersion(), 3);
    ASSERT_EQ(shardVersion.placementVersion().minorVersion(), 4);
    ASSERT_EQ(shardVersion.indexVersion(), Timestamp(5, 6));
}

TEST(ShardVersionTest, ToAndFromBSON) {
    const CollectionGeneration gen(OID::gen(), Timestamp(1, 2));
    const ChunkVersion chunkVersion(gen, {3, 4});
    const CollectionIndexes collectionIndexes(UUID::gen(), Timestamp(5, 6));
    const ShardVersion shardVersion(chunkVersion, collectionIndexes);

    BSONObjBuilder builder;
    shardVersion.serialize(ShardVersion::kShardVersionField, &builder);
    const auto obj = builder.obj();

    const auto fromBSON = ShardVersion::parse(obj[ShardVersion::kShardVersionField]);
    ASSERT_EQ(fromBSON, shardVersion);
}

}  // namespace
}  // namespace mongo
