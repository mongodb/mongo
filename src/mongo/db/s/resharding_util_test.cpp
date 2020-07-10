/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/unittest/unittest.h"
#include <vector>

namespace mongo {
namespace {

class ReshardingUtilTest : public ConfigServerTestFixture {
protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();
        ShardType shard1;
        shard1.setName("a");
        shard1.setHost("a:1234");
        ShardType shard2;
        shard2.setName("b");
        shard2.setHost("b:1234");
        setupShards({shard1, shard2});
    }
    void tearDown() override {
        ConfigServerTestFixture::tearDown();
    }

    const std::string shardKey() {
        return _shardKey;
    }

    const KeyPattern& keyPattern() {
        return _shardKeyPattern.getKeyPattern();
    }

    const NamespaceString nss() {
        return _nss;
    }

    BSONObj makeReshardedChunk(const ChunkRange range, std::string shardId) {
        BSONObjBuilder reshardedchunkBuilder;
        reshardedchunkBuilder.append(ReshardedChunk::kRecipientShardIdFieldName, shardId);
        reshardedchunkBuilder.append(ReshardedChunk::kMinFieldName, range.getMin());
        reshardedchunkBuilder.append(ReshardedChunk::kMaxFieldName, range.getMax());
        return reshardedchunkBuilder.obj();
    }

    BSONObj makeZone(const ChunkRange range, std::string zoneName) {
        BSONObjBuilder tagDocBuilder;
        tagDocBuilder.append("_id",
                             BSON(TagsType::ns(nss().ns()) << TagsType::min(range.getMin())));
        tagDocBuilder.append(TagsType::ns(), nss().ns());
        tagDocBuilder.append(TagsType::min(), range.getMin());
        tagDocBuilder.append(TagsType::max(), range.getMax());
        tagDocBuilder.append(TagsType::tag(), zoneName);
        return tagDocBuilder.obj();
    }

    TagsType makeTagType(const ChunkRange range, std::string zoneName) {
        return unittest::assertGet(TagsType::fromBSON(makeZone(range, zoneName)));
    }

    const std::string zoneName(std::string zoneNum) {
        return "_zoneName" + zoneNum;
    }

private:
    const NamespaceString _nss{"test.foo"};
    const std::string _shardKey = "x";
    const ShardKeyPattern _shardKeyPattern = ShardKeyPattern(BSON("x"
                                                                  << "hashed"));
};

// Validate resharded chunks tests.

TEST_F(ReshardingUtilTest, SuccessfulValidateReshardedChunkCase) {
    std::vector<mongo::BSONObj> chunks;
    const std::vector<ChunkRange> chunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), keyPattern().globalMax()),
    };
    chunks.push_back(makeReshardedChunk(chunkRanges[0], "a"));
    chunks.push_back(makeReshardedChunk(chunkRanges[1], "b"));

    validateReshardedChunks(chunks, operationContext(), keyPattern());
}

TEST_F(ReshardingUtilTest, FailWhenHoleInChunkRange) {
    std::vector<mongo::BSONObj> chunks;
    const std::vector<ChunkRange> chunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 20), keyPattern().globalMax()),
    };
    chunks.push_back(makeReshardedChunk(chunkRanges[0], "a"));
    chunks.push_back(makeReshardedChunk(chunkRanges[1], "b"));
    ASSERT_THROWS_CODE(validateReshardedChunks(chunks, operationContext(), keyPattern()),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST_F(ReshardingUtilTest, FailWhenOverlapInChunkRange) {
    const std::vector<ChunkRange> overlapChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 10)),
        ChunkRange(BSON(shardKey() << 5), keyPattern().globalMax()),
    };
    std::vector<mongo::BSONObj> chunks;
    chunks.push_back(makeReshardedChunk(overlapChunkRanges[0], "a"));
    chunks.push_back(makeReshardedChunk(overlapChunkRanges[1], "b"));
    ASSERT_THROWS_CODE(validateReshardedChunks(chunks, operationContext(), keyPattern()),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST_F(ReshardingUtilTest, FailWhenChunkRangeDoesNotStartAtGlobalMin) {
    std::vector<mongo::BSONObj> chunks;
    const std::vector<ChunkRange> chunkRanges = {
        ChunkRange(BSON(shardKey() << 10), BSON(shardKey() << 20)),
        ChunkRange(BSON(shardKey() << 20), keyPattern().globalMax()),
    };
    chunks.push_back(makeReshardedChunk(chunkRanges[0], "a"));
    chunks.push_back(makeReshardedChunk(chunkRanges[1], "b"));
    ASSERT_THROWS_CODE(validateReshardedChunks(chunks, operationContext(), keyPattern()),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST_F(ReshardingUtilTest, FailWhenChunkRangeDoesNotEndAtGlobalMax) {
    std::vector<mongo::BSONObj> chunks;
    const std::vector<ChunkRange> chunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),
    };
    chunks.push_back(makeReshardedChunk(chunkRanges[0], "a"));
    chunks.push_back(makeReshardedChunk(chunkRanges[1], "b"));

    ASSERT_THROWS_CODE(validateReshardedChunks(chunks, operationContext(), keyPattern()),
                       DBException,
                       ErrorCodes::BadValue);
}

// Validate zones tests.

TEST_F(ReshardingUtilTest, SuccessfulValidateZoneCase) {
    const std::vector<ChunkRange> zoneRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),
    };
    const std::vector<TagsType> authoritativeTags = {makeTagType(zoneRanges[1], zoneName("1"))};
    std::vector<mongo::BSONObj> zones;
    zones.push_back(makeZone(zoneRanges[0], zoneName("1")));
    validateZones(zones, authoritativeTags);
}

TEST_F(ReshardingUtilTest, FailWhenMissingZoneNameInUserProvidedZone) {
    const std::vector<ChunkRange> zoneRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),
    };
    const std::vector<TagsType> authoritativeTags = {makeTagType(zoneRanges[1], zoneName("1"))};
    std::vector<mongo::BSONObj> zones;
    // make a zoneBSONObj and remove the zoneName field from it.
    auto zone = makeZone(zoneRanges[0], zoneName("0")).removeField(TagsType::tag());
    zones.push_back(zone);
    ASSERT_THROWS_CODE(validateZones(zones, authoritativeTags), DBException, ErrorCodes::NoSuchKey);
}

TEST_F(ReshardingUtilTest, FailWhenZoneNameDoesNotExistInConfigTagsCollection) {
    const std::vector<ChunkRange> zoneRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),
    };
    const std::vector<TagsType> authoritativeTags = {makeTagType(zoneRanges[1], zoneName("1"))};
    std::vector<mongo::BSONObj> zones;
    zones.push_back(makeZone(zoneRanges[0], zoneName("0")));
    ASSERT_THROWS_CODE(validateZones(zones, authoritativeTags), DBException, ErrorCodes::BadValue);
}

TEST_F(ReshardingUtilTest, FailWhenOverlappingZones) {
    const std::vector<ChunkRange> overlapZoneRanges = {
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),
        ChunkRange(BSON(shardKey() << 8), keyPattern().globalMax()),
    };
    const std::vector<TagsType> authoritativeTags = {
        makeTagType(overlapZoneRanges[0], zoneName("0")),
        makeTagType(overlapZoneRanges[1], zoneName("1"))};
    std::vector<mongo::BSONObj> zones;
    zones.push_back(makeZone(overlapZoneRanges[0], zoneName("0")));
    zones.push_back(makeZone(overlapZoneRanges[1], zoneName("1")));
    ASSERT_THROWS_CODE(validateZones(zones, authoritativeTags), DBException, ErrorCodes::BadValue);
}

}  // namespace
}  // namespace mongo
