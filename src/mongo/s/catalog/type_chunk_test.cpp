/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/type_chunk.h"

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using std::string;
using unittest::assertGet;

const BSONObj kMin = BSON("a" << 10);
const BSONObj kMax = BSON("a" << 20);
const ShardId kShard("shard0000");

TEST(ChunkType, MissingConfigRequiredFields) {
    ChunkVersion chunkVersion(1, 2, OID::gen());

    BSONObj objModNS =
        BSON(ChunkType::name("test.mycol-a_MinKey") << ChunkType::min(BSON("a" << 10 << "b" << 10))
                                                    << ChunkType::max(BSON("a" << 20))
                                                    << "lastmod"
                                                    << Timestamp(chunkVersion.toLong())
                                                    << "lastmodEpoch"
                                                    << chunkVersion.epoch()
                                                    << ChunkType::shard("shard0001"));
    StatusWith<ChunkType> chunkRes = ChunkType::fromConfigBSON(objModNS);
    ASSERT_FALSE(chunkRes.isOK());

    BSONObj objModKeys =
        BSON(ChunkType::name("test.mycol-a_MinKey") << ChunkType::ns("test.mycol") << "lastmod"
                                                    << Timestamp(chunkVersion.toLong())
                                                    << "lastmodEpoch"
                                                    << chunkVersion.epoch()
                                                    << ChunkType::shard("shard0001"));
    chunkRes = ChunkType::fromConfigBSON(objModKeys);
    ASSERT_FALSE(chunkRes.isOK());

    BSONObj objModShard =
        BSON(ChunkType::name("test.mycol-a_MinKey") << ChunkType::ns("test.mycol")
                                                    << ChunkType::min(BSON("a" << 10 << "b" << 10))
                                                    << ChunkType::max(BSON("a" << 20))
                                                    << "lastmod"
                                                    << Timestamp(chunkVersion.toLong())
                                                    << "lastmodEpoch"
                                                    << chunkVersion.epoch());
    chunkRes = ChunkType::fromConfigBSON(objModShard);
    ASSERT_FALSE(chunkRes.isOK());

    BSONObj objModVersion =
        BSON(ChunkType::name("test.mycol-a_MinKey") << ChunkType::ns("test.mycol")
                                                    << ChunkType::min(BSON("a" << 10 << "b" << 10))
                                                    << ChunkType::max(BSON("a" << 20))
                                                    << ChunkType::shard("shard0001"));
    chunkRes = ChunkType::fromConfigBSON(objModVersion);
    ASSERT_FALSE(chunkRes.isOK());
}

TEST(ChunkType, MissingShardRequiredFields) {
    const OID epoch = OID::gen();
    ChunkVersion chunkVersion(1, 2, epoch);
    const auto lastmod = Timestamp(chunkVersion.toLong());

    BSONObj objModMin =
        BSON(ChunkType::max(kMax) << ChunkType::shard(kShard.toString()) << "lastmod" << lastmod);
    StatusWith<ChunkType> chunkRes = ChunkType::fromShardBSON(objModMin, epoch);
    ASSERT_EQUALS(chunkRes.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(chunkRes.getStatus().reason(), ChunkType::minShardID.name());

    BSONObj objModMax = BSON(
        ChunkType::minShardID(kMin) << ChunkType::shard(kShard.toString()) << "lastmod" << lastmod);
    chunkRes = ChunkType::fromShardBSON(objModMax, epoch);
    ASSERT_EQUALS(chunkRes.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(chunkRes.getStatus().reason(), ChunkType::max.name());

    BSONObj objModShard =
        BSON(ChunkType::minShardID(kMin) << ChunkType::max(kMax) << "lastmod" << lastmod);
    chunkRes = ChunkType::fromShardBSON(objModShard, epoch);
    ASSERT_EQUALS(chunkRes.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(chunkRes.getStatus().reason(), ChunkType::shard.name());

    BSONObj objModLastmod = BSON(
        ChunkType::minShardID(kMin) << ChunkType::max(kMax) << ChunkType::shard(kShard.toString()));
    chunkRes = ChunkType::fromShardBSON(objModLastmod, epoch);
    ASSERT_EQUALS(chunkRes.getStatus(), ErrorCodes::BadValue);
}

TEST(ChunkType, ToFromShardBSON) {
    const OID epoch = OID::gen();
    ChunkVersion chunkVersion(1, 2, epoch);
    auto lastmod = Timestamp(chunkVersion.toLong());

    BSONObj obj = BSON(ChunkType::minShardID(kMin) << ChunkType::max(kMax)
                                                   << ChunkType::shard(kShard.toString())
                                                   << "lastmod"
                                                   << lastmod);
    ChunkType shardChunk = assertGet(ChunkType::fromShardBSON(obj, epoch));

    ASSERT_BSONOBJ_EQ(obj, shardChunk.toShardBSON());

    ASSERT_BSONOBJ_EQ(kMin, shardChunk.getMin());
    ASSERT_BSONOBJ_EQ(kMax, shardChunk.getMax());
    ASSERT_EQUALS(kShard, shardChunk.getShard());
    ASSERT_EQUALS(chunkVersion, shardChunk.getVersion());
}

TEST(ChunkType, MinAndMaxShardKeysDifferInNumberOfKeys) {
    ChunkVersion chunkVersion(1, 2, OID::gen());
    BSONObj obj =
        BSON(ChunkType::name("test.mycol-a_MinKey") << ChunkType::ns("test.mycol")
                                                    << ChunkType::min(BSON("a" << 10 << "b" << 10))
                                                    << ChunkType::max(BSON("a" << 20))
                                                    << "lastmod"
                                                    << Timestamp(chunkVersion.toLong())
                                                    << "lastmodEpoch"
                                                    << chunkVersion.epoch()
                                                    << ChunkType::shard("shard0001"));
    StatusWith<ChunkType> chunkRes = ChunkType::fromConfigBSON(obj);
    ASSERT_OK(chunkRes.getStatus());
    ASSERT_FALSE(chunkRes.getValue().validate().isOK());
}

TEST(ChunkType, MinAndMaxShardKeysDifferInKeyNames) {
    ChunkVersion chunkVersion(1, 2, OID::gen());
    BSONObj obj = BSON(ChunkType::name("test.mycol-a_MinKey") << ChunkType::ns("test.mycol")
                                                              << ChunkType::min(BSON("a" << 10))
                                                              << ChunkType::max(BSON("b" << 20))
                                                              << "lastmod"
                                                              << Timestamp(chunkVersion.toLong())
                                                              << "lastmodEpoch"
                                                              << chunkVersion.epoch()
                                                              << ChunkType::shard("shard0001"));
    StatusWith<ChunkType> chunkRes = ChunkType::fromConfigBSON(obj);
    ASSERT_OK(chunkRes.getStatus());
    ASSERT_FALSE(chunkRes.getValue().validate().isOK());
}

TEST(ChunkType, MinToMaxNotAscending) {
    ChunkVersion chunkVersion(1, 2, OID::gen());
    BSONObj obj = BSON(ChunkType::name("test.mycol-a_MinKey") << ChunkType::ns("test.mycol")
                                                              << ChunkType::min(BSON("a" << 20))
                                                              << ChunkType::max(BSON("a" << 10))
                                                              << "lastmod"
                                                              << Timestamp(chunkVersion.toLong())
                                                              << "lastmodEpoch"
                                                              << chunkVersion.epoch()
                                                              << ChunkType::shard("shard0001"));
    StatusWith<ChunkType> chunkRes = ChunkType::fromConfigBSON(obj);
    ASSERT_EQ(ErrorCodes::FailedToParse, chunkRes.getStatus());
}

TEST(ChunkType, ToFromConfigBSON) {
    ChunkVersion chunkVersion(1, 2, OID::gen());
    BSONObj obj = BSON(ChunkType::name("test.mycol-a_10") << ChunkType::ns("test.mycol")
                                                          << ChunkType::min(BSON("a" << 10))
                                                          << ChunkType::max(BSON("a" << 20))
                                                          << ChunkType::shard("shard0001")
                                                          << "lastmod"
                                                          << Timestamp(chunkVersion.toLong())
                                                          << "lastmodEpoch"
                                                          << chunkVersion.epoch());
    StatusWith<ChunkType> chunkRes = ChunkType::fromConfigBSON(obj);
    ASSERT_OK(chunkRes.getStatus());
    ChunkType chunk = chunkRes.getValue();

    ASSERT_BSONOBJ_EQ(chunk.toConfigBSON(), obj);

    ASSERT_EQUALS(chunk.getNS(), "test.mycol");
    ASSERT_BSONOBJ_EQ(chunk.getMin(), BSON("a" << 10));
    ASSERT_BSONOBJ_EQ(chunk.getMax(), BSON("a" << 20));
    ASSERT_EQUALS(chunk.getVersion().toLong(), chunkVersion.toLong());
    ASSERT_EQUALS(chunk.getVersion().epoch(), chunkVersion.epoch());
    ASSERT_EQUALS(chunk.getShard(), "shard0001");
    ASSERT_OK(chunk.validate());
}

TEST(ChunkType, Pre22Format) {
    ChunkType chunk = assertGet(ChunkType::fromConfigBSON(BSON("_id"
                                                               << "test.mycol-a_MinKey"
                                                               << "lastmod"
                                                               << Date_t::fromMillisSinceEpoch(1)
                                                               << "ns"
                                                               << "test.mycol"
                                                               << "min"
                                                               << BSON("a" << 10)
                                                               << "max"
                                                               << BSON("a" << 20)
                                                               << "shard"
                                                               << "shard0001")));

    ASSERT_OK(chunk.validate());
    ASSERT_EQUALS(chunk.getNS(), "test.mycol");
    ASSERT_BSONOBJ_EQ(chunk.getMin(), BSON("a" << 10));
    ASSERT_BSONOBJ_EQ(chunk.getMax(), BSON("a" << 20));
    ASSERT_EQUALS(chunk.getVersion().toLong(), 1ULL);
    ASSERT(!chunk.getVersion().epoch().isSet());
    ASSERT_EQUALS(chunk.getShard(), "shard0001");
}

TEST(ChunkType, BadType) {
    BSONObj obj = BSON(ChunkType::name() << 0);
    StatusWith<ChunkType> chunkRes = ChunkType::fromConfigBSON(obj);
    ASSERT_FALSE(chunkRes.isOK());
}

TEST(ChunkRange, BasicBSONParsing) {
    auto parseStatus =
        ChunkRange::fromBSON(BSON("min" << BSON("x" << 0) << "max" << BSON("x" << 10)));
    ASSERT_OK(parseStatus.getStatus());

    auto chunkRange = parseStatus.getValue();
    ASSERT_BSONOBJ_EQ(BSON("x" << 0), chunkRange.getMin());
    ASSERT_BSONOBJ_EQ(BSON("x" << 10), chunkRange.getMax());
}

TEST(ChunkRange, MinGreaterThanMaxShouldError) {
    auto parseStatus =
        ChunkRange::fromBSON(BSON("min" << BSON("x" << 10) << "max" << BSON("x" << 0)));
    ASSERT_EQ(ErrorCodes::FailedToParse, parseStatus.getStatus());
}

}  // namespace
}  // namespace mongo
