// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/chunk.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.foo");
const ShardId kShardOne("shardOne");
const ShardId kShardTwo("shardTwo");
const KeyPattern kShardKeyPattern(BSON("a" << 1));

TEST(ChunkTest, HasMovedSincePinnedTimestamp) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    ChunkVersion version({epoch, Timestamp(1, 1)}, {1, 0});

    ChunkType chunkType(uuid,
                        ChunkRange{kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax()},
                        version,
                        kShardOne);
    chunkType.setOnCurrentShardSince(Timestamp(101, 0));
    chunkType.setHistory({ChunkHistory(*chunkType.getOnCurrentShardSince(), kShardOne),
                          ChunkHistory(Timestamp(100, 0), kShardTwo)});

    ChunkInfo chunkInfo(chunkType);
    Chunk chunk(chunkInfo, Timestamp(100, 0));
    ASSERT_THROWS_CODE(chunk.throwIfMoved(), AssertionException, ErrorCodes::MigrationConflict);
}

TEST(ChunkTest, HasMovedAndReturnedSincePinnedTimestamp) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    ChunkVersion version({epoch, Timestamp(1, 1)}, {1, 0});

    ChunkType chunkType(uuid,
                        ChunkRange{kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax()},
                        version,
                        kShardOne);
    chunkType.setOnCurrentShardSince(Timestamp(102, 0));
    chunkType.setHistory({ChunkHistory(*chunkType.getOnCurrentShardSince(), kShardOne),
                          ChunkHistory(Timestamp(101, 0), kShardTwo),
                          ChunkHistory(Timestamp(100, 0), kShardOne)});

    ChunkInfo chunkInfo(chunkType);
    Chunk chunk(chunkInfo, Timestamp(100, 0));
    ASSERT_THROWS_CODE(chunk.throwIfMoved(), AssertionException, ErrorCodes::MigrationConflict);
}

TEST(ChunkTest, HasNotMovedSincePinnedTimestamp) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    ChunkVersion version({epoch, Timestamp(1, 1)}, {1, 0});

    ChunkType chunkType(uuid,
                        ChunkRange{kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax()},
                        version,
                        kShardOne);
    chunkType.setOnCurrentShardSince(Timestamp(100, 0));
    chunkType.setHistory({ChunkHistory(*chunkType.getOnCurrentShardSince(), kShardOne),
                          ChunkHistory(Timestamp(99, 0), kShardTwo)});

    ChunkInfo chunkInfo(chunkType);
    Chunk chunk(chunkInfo, Timestamp(100, 0));
    // Should not throw.
    chunk.throwIfMoved();
}

TEST(ChunkTest, HasNoHistoryValidForPinnedTimestamp_OneEntry) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    ChunkVersion version({epoch, Timestamp(1, 1)}, {1, 0});

    ChunkType chunkType(uuid,
                        ChunkRange{kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax()},
                        version,
                        kShardOne);
    chunkType.setOnCurrentShardSince(Timestamp(101, 0));
    chunkType.setHistory({ChunkHistory(*chunkType.getOnCurrentShardSince(), kShardOne)});

    ChunkInfo chunkInfo(chunkType);
    Chunk chunk(chunkInfo, Timestamp(100, 0));
    ASSERT_THROWS_CODE(chunk.throwIfMoved(), AssertionException, ErrorCodes::StaleChunkHistory);
}

TEST(ChunkTest, HasNoHistoryValidForPinnedTimestamp_MoreThanOneEntry) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    ChunkVersion version({epoch, Timestamp(1, 1)}, {1, 0});

    ChunkType chunkType(uuid,
                        ChunkRange{kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax()},
                        version,
                        kShardOne);
    chunkType.setOnCurrentShardSince(Timestamp(102, 0));
    chunkType.setHistory({ChunkHistory(*chunkType.getOnCurrentShardSince(), kShardOne),
                          ChunkHistory(Timestamp(101, 0), kShardTwo)});

    ChunkInfo chunkInfo(chunkType);
    Chunk chunk(chunkInfo, Timestamp(100, 0));
    ASSERT_THROWS_CODE(chunk.throwIfMoved(), AssertionException, ErrorCodes::StaleChunkHistory);
}

}  // namespace
}  // namespace mongo
