/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/shard_id.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString kNss("test.foo");
const ShardId kShardOne("shardOne");
const ShardId kShardTwo("shardTwo");
const KeyPattern kShardKeyPattern(BSON("a" << 1));

TEST(ChunkTest, HasMovedSincePinnedTimestamp) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    ChunkVersion version{1, 0, epoch, Timestamp(1, 1)};

    ChunkType chunkType(uuid,
                        ChunkRange{kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax()},
                        version,
                        kShardOne);
    chunkType.setHistory(
        {ChunkHistory(Timestamp(101, 0), kShardOne), ChunkHistory(Timestamp(100, 0), kShardTwo)});

    ChunkInfo chunkInfo(chunkType);
    Chunk chunk(chunkInfo, Timestamp(100, 0));
    ASSERT_THROWS_CODE(chunk.throwIfMoved(), AssertionException, ErrorCodes::MigrationConflict);
}

TEST(ChunkTest, HasMovedAndReturnedSincePinnedTimestamp) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    ChunkVersion version{1, 0, epoch, Timestamp(1, 1)};

    ChunkType chunkType(uuid,
                        ChunkRange{kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax()},
                        version,
                        kShardOne);
    chunkType.setHistory({ChunkHistory(Timestamp(102, 0), kShardOne),
                          ChunkHistory(Timestamp(101, 0), kShardTwo),
                          ChunkHistory(Timestamp(100, 0), kShardOne)});

    ChunkInfo chunkInfo(chunkType);
    Chunk chunk(chunkInfo, Timestamp(100, 0));
    ASSERT_THROWS_CODE(chunk.throwIfMoved(), AssertionException, ErrorCodes::MigrationConflict);
}

TEST(ChunkTest, HasNotMovedSincePinnedTimestamp) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    ChunkVersion version{1, 0, epoch, Timestamp(1, 1)};

    ChunkType chunkType(uuid,
                        ChunkRange{kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax()},
                        version,
                        kShardOne);
    chunkType.setHistory(
        {ChunkHistory(Timestamp(100, 0), kShardOne), ChunkHistory(Timestamp(99, 0), kShardTwo)});

    ChunkInfo chunkInfo(chunkType);
    Chunk chunk(chunkInfo, Timestamp(100, 0));
    // Should not throw.
    chunk.throwIfMoved();
}

TEST(ChunkTest, HasNoHistoryValidForPinnedTimestamp_OneEntry) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    ChunkVersion version{1, 0, epoch, Timestamp(1, 1)};

    ChunkType chunkType(uuid,
                        ChunkRange{kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax()},
                        version,
                        kShardOne);
    chunkType.setHistory({ChunkHistory(Timestamp(101, 0), kShardOne)});

    ChunkInfo chunkInfo(chunkType);
    Chunk chunk(chunkInfo, Timestamp(100, 0));
    ASSERT_THROWS_CODE(chunk.throwIfMoved(), AssertionException, ErrorCodes::StaleChunkHistory);
}

TEST(ChunkTest, HasNoHistoryValidForPinnedTimestamp_MoreThanOneEntry) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    ChunkVersion version{1, 0, epoch, Timestamp(1, 1)};

    ChunkType chunkType(uuid,
                        ChunkRange{kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax()},
                        version,
                        kShardOne);
    chunkType.setHistory(
        {ChunkHistory(Timestamp(102, 0), kShardOne), ChunkHistory(Timestamp(101, 0), kShardTwo)});

    ChunkInfo chunkInfo(chunkType);
    Chunk chunk(chunkInfo, Timestamp(100, 0));
    ASSERT_THROWS_CODE(chunk.throwIfMoved(), AssertionException, ErrorCodes::StaleChunkHistory);
}

}  // namespace
}  // namespace mongo
