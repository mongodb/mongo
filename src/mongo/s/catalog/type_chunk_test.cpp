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
        BSON(ChunkType::name(OID::gen())
             << ChunkType::min(BSON("a" << 10 << "b" << 10)) << ChunkType::max(BSON("a" << 20))
             << "lastmod" << Timestamp(chunkVersion.toLong()) << "lastmodEpoch"
             << chunkVersion.epoch() << ChunkType::shard("shard0001"));
    StatusWith<ChunkType> chunkRes = ChunkType::fromConfigBSON(objModNS);
    ASSERT_FALSE(chunkRes.isOK());

    BSONObj objModKeys =
        BSON(ChunkType::name(OID::gen())
             << ChunkType::ns("test.mycol") << "lastmod" << Timestamp(chunkVersion.toLong())
             << "lastmodEpoch" << chunkVersion.epoch() << ChunkType::shard("shard0001"));
    chunkRes = ChunkType::fromConfigBSON(objModKeys);
    ASSERT_FALSE(chunkRes.isOK());

    BSONObj objModShard =
        BSON(ChunkType::name(OID::gen())
             << ChunkType::ns("test.mycol") << ChunkType::min(BSON("a" << 10 << "b" << 10))
             << ChunkType::max(BSON("a" << 20)) << "lastmod" << Timestamp(chunkVersion.toLong())
             << "lastmodEpoch" << chunkVersion.epoch());
    chunkRes = ChunkType::fromConfigBSON(objModShard);
    ASSERT_FALSE(chunkRes.isOK());

    BSONObj objModVersion =
        BSON(ChunkType::name(OID::gen())
             << ChunkType::ns("test.mycol") << ChunkType::min(BSON("a" << 10 << "b" << 10))
             << ChunkType::max(BSON("a" << 20)) << ChunkType::shard("shard0001"));
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

    BSONObj objModMax = BSON(ChunkType::minShardID(kMin)
                             << ChunkType::shard(kShard.toString()) << "lastmod" << lastmod);
    chunkRes = ChunkType::fromShardBSON(objModMax, epoch);
    ASSERT_EQUALS(chunkRes.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(chunkRes.getStatus().reason(), ChunkType::max.name());

    BSONObj objModShard =
        BSON(ChunkType::minShardID(kMin) << ChunkType::max(kMax) << "lastmod" << lastmod);
    chunkRes = ChunkType::fromShardBSON(objModShard, epoch);
    ASSERT_EQUALS(chunkRes.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(chunkRes.getStatus().reason(), ChunkType::shard.name());

    BSONObj objModLastmod = BSON(ChunkType::minShardID(kMin)
                                 << ChunkType::max(kMax) << ChunkType::shard(kShard.toString()));
    chunkRes = ChunkType::fromShardBSON(objModLastmod, epoch);
    ASSERT_EQUALS(chunkRes.getStatus(), ErrorCodes::NoSuchKey);
}

TEST(ChunkType, ToFromShardBSON) {
    const OID epoch = OID::gen();
    ChunkVersion chunkVersion(1, 2, epoch);
    auto lastmod = Timestamp(chunkVersion.toLong());

    BSONObj obj = BSON(ChunkType::minShardID(kMin)
                       << ChunkType::max(kMax) << ChunkType::shard(kShard.toString()) << "lastmod"
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
        BSON(ChunkType::name(OID::gen())
             << ChunkType::ns("test.mycol") << ChunkType::min(BSON("a" << 10 << "b" << 10))
             << ChunkType::max(BSON("a" << 20)) << "lastmod" << Timestamp(chunkVersion.toLong())
             << "lastmodEpoch" << chunkVersion.epoch() << ChunkType::shard("shard0001"));
    StatusWith<ChunkType> chunkRes = ChunkType::fromConfigBSON(obj);
    ASSERT_OK(chunkRes.getStatus());
    ASSERT_FALSE(chunkRes.getValue().validate().isOK());
}

TEST(ChunkType, MinAndMaxShardKeysDifferInKeyNames) {
    ChunkVersion chunkVersion(1, 2, OID::gen());
    BSONObj obj =
        BSON(ChunkType::name(OID::gen())
             << ChunkType::ns("test.mycol") << ChunkType::min(BSON("a" << 10))
             << ChunkType::max(BSON("b" << 20)) << "lastmod" << Timestamp(chunkVersion.toLong())
             << "lastmodEpoch" << chunkVersion.epoch() << ChunkType::shard("shard0001"));
    StatusWith<ChunkType> chunkRes = ChunkType::fromConfigBSON(obj);
    ASSERT_OK(chunkRes.getStatus());
    ASSERT_FALSE(chunkRes.getValue().validate().isOK());
}

TEST(ChunkType, MinToMaxNotAscending) {
    ChunkVersion chunkVersion(1, 2, OID::gen());
    BSONObj obj =
        BSON(ChunkType::name(OID::gen())
             << ChunkType::ns("test.mycol") << ChunkType::min(BSON("a" << 20))
             << ChunkType::max(BSON("a" << 10)) << "lastmod" << Timestamp(chunkVersion.toLong())
             << "lastmodEpoch" << chunkVersion.epoch() << ChunkType::shard("shard0001"));
    StatusWith<ChunkType> chunkRes = ChunkType::fromConfigBSON(obj);
    ASSERT_EQ(ErrorCodes::FailedToParse, chunkRes.getStatus());
}

TEST(ChunkType, ToFromConfigBSON) {
    const auto chunkID = OID::gen();
    ChunkVersion chunkVersion(1, 2, OID::gen());
    BSONObj obj =
        BSON(ChunkType::name(chunkID)
             << ChunkType::ns("test.mycol") << ChunkType::min(BSON("a" << 10))
             << ChunkType::max(BSON("a" << 20)) << ChunkType::shard("shard0001") << "lastmod"
             << Timestamp(chunkVersion.toLong()) << "lastmodEpoch" << chunkVersion.epoch());
    StatusWith<ChunkType> chunkRes = ChunkType::fromConfigBSON(obj);
    ASSERT_OK(chunkRes.getStatus());
    ChunkType chunk = chunkRes.getValue();

    ASSERT_BSONOBJ_EQ(chunk.toConfigBSON(), obj);

    ASSERT_EQUALS(chunk.getName(), chunkID);
    ASSERT_EQUALS(chunk.getNS().ns(), "test.mycol");
    ASSERT_BSONOBJ_EQ(chunk.getMin(), BSON("a" << 10));
    ASSERT_BSONOBJ_EQ(chunk.getMax(), BSON("a" << 20));
    ASSERT_EQUALS(chunk.getVersion().toLong(), chunkVersion.toLong());
    ASSERT_EQUALS(chunk.getVersion().epoch(), chunkVersion.epoch());
    ASSERT_EQUALS(chunk.getShard(), "shard0001");
    ASSERT_OK(chunk.validate());
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

TEST(ChunkRange, Covers) {
    auto target = ChunkRange(BSON("x" << 5), BSON("x" << 10));
    ASSERT(!target.covers(ChunkRange(BSON("x" << 0), BSON("x" << 5))));
    ASSERT(!target.covers(ChunkRange(BSON("x" << 10), BSON("x" << 15))));
    ASSERT(!target.covers(ChunkRange(BSON("x" << 0), BSON("x" << 7))));
    ASSERT(!target.covers(ChunkRange(BSON("x" << 7), BSON("x" << 15))));
    ASSERT(!target.covers(ChunkRange(BSON("x" << 0), BSON("x" << 15))));
    ASSERT(!target.covers(ChunkRange(BSON("x" << 0), BSON("x" << 10))));
    ASSERT(!target.covers(ChunkRange(BSON("x" << 5), BSON("x" << 15))));
    ASSERT(target.covers(ChunkRange(BSON("x" << 5), BSON("x" << 10))));
    ASSERT(target.covers(ChunkRange(BSON("x" << 6), BSON("x" << 10))));
    ASSERT(target.covers(ChunkRange(BSON("x" << 5), BSON("x" << 9))));
    ASSERT(target.covers(ChunkRange(BSON("x" << 6), BSON("x" << 9))));
}

TEST(ChunkRange, Overlap) {
    auto target = ChunkRange(BSON("x" << 5), BSON("x" << 10));
    ASSERT(!target.overlapWith(ChunkRange(BSON("x" << 0), BSON("x" << 5))));
    ASSERT(!target.overlapWith(ChunkRange(BSON("x" << 0), BSON("x" << 4))));
    ASSERT(!target.overlapWith(ChunkRange(BSON("x" << 10), BSON("x" << 15))));
    ASSERT(!target.overlapWith(ChunkRange(BSON("x" << 11), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 7), BSON("x" << 10)) ==
           *target.overlapWith(ChunkRange(BSON("x" << 7), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 10)) ==
           *target.overlapWith(ChunkRange(BSON("x" << 0), BSON("x" << 10))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 10)) ==
           *target.overlapWith(ChunkRange(BSON("x" << 0), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 10)) ==
           *target.overlapWith(ChunkRange(BSON("x" << 5), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 9)) ==
           *target.overlapWith(ChunkRange(BSON("x" << 0), BSON("x" << 9))));
    ASSERT(ChunkRange(BSON("x" << 9), BSON("x" << 10)) ==
           *target.overlapWith(ChunkRange(BSON("x" << 9), BSON("x" << 15))));
}

TEST(ChunkRange, Union) {
    auto target = ChunkRange(BSON("x" << 5), BSON("x" << 10));
    ASSERT(ChunkRange(BSON("x" << 0), BSON("x" << 10)) ==
           target.unionWith(ChunkRange(BSON("x" << 0), BSON("x" << 5))));
    ASSERT(ChunkRange(BSON("x" << 0), BSON("x" << 10)) ==
           target.unionWith(ChunkRange(BSON("x" << 0), BSON("x" << 4))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 15)) ==
           target.unionWith(ChunkRange(BSON("x" << 10), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 15)) ==
           target.unionWith(ChunkRange(BSON("x" << 11), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 15)) ==
           target.unionWith(ChunkRange(BSON("x" << 7), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 0), BSON("x" << 10)) ==
           target.unionWith(ChunkRange(BSON("x" << 0), BSON("x" << 10))));
    ASSERT(ChunkRange(BSON("x" << 0), BSON("x" << 14)) ==
           target.unionWith(ChunkRange(BSON("x" << 0), BSON("x" << 14))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 15)) ==
           target.unionWith(ChunkRange(BSON("x" << 5), BSON("x" << 15))));
    ASSERT(ChunkRange(BSON("x" << 0), BSON("x" << 10)) ==
           target.unionWith(ChunkRange(BSON("x" << 0), BSON("x" << 9))));
    ASSERT(ChunkRange(BSON("x" << 5), BSON("x" << 15)) ==
           target.unionWith(ChunkRange(BSON("x" << 9), BSON("x" << 15))));
}

TEST(ChunkRange, MinGreaterThanMaxShouldError) {
    auto parseStatus =
        ChunkRange::fromBSON(BSON("min" << BSON("x" << 10) << "max" << BSON("x" << 0)));
    ASSERT_EQ(ErrorCodes::FailedToParse, parseStatus.getStatus());
}

// TODO SERVER-44034: Delete this test.
TEST(ChunkType, FromConfigBSONParsesIgnores42_idFormat) {
    NamespaceString nss("test.mycol");
    auto minBound = BSON("a" << 10);
    ChunkVersion chunkVersion(1, 2, OID::gen());

    BSONObj obj = BSON("_id" << ChunkType::genLegacyID(nss, minBound) << ChunkType::ns(nss.ns())
                             << ChunkType::min(minBound) << ChunkType::max(BSON("a" << 20))
                             << "lastmod" << Timestamp(chunkVersion.toLong()) << "lastmodEpoch"
                             << chunkVersion.epoch() << ChunkType::shard("shard0001"));

    // Parsing will succeed despite the string _id.
    auto chunk = uassertStatusOK(ChunkType::fromConfigBSON(obj));

    // Attempting to get the 4.4 _id will throw since it hasn't been set.
    ASSERT_THROWS_CODE(chunk.getName(), AssertionException, 51264);
}

// TODO SERVER-44034: Delete this test.
TEST(ChunkType, LegacyNameBSONFieldIs_id) {
    auto obj = BSON(ChunkType::legacyName("dummyId"));
    ASSERT_BSONOBJ_EQ(obj,
                      BSON("_id"
                           << "dummyId"));
}

// TODO SERVER-44034: Delete this test.
TEST(ChunkType, GetLegacyNameAndGenLegacyIDReturn42_idFormat) {
    NamespaceString nss("test.mycol");
    auto minBound = BSON("a" << 10);
    ChunkVersion chunkVersion(1, 2, OID::gen());

    BSONObj obj =
        BSON(ChunkType::name(OID::gen())
             << ChunkType::ns(nss.ns()) << ChunkType::min(minBound)
             << ChunkType::max(BSON("a" << 20)) << "lastmod" << Timestamp(chunkVersion.toLong())
             << "lastmodEpoch" << chunkVersion.epoch() << ChunkType::shard("shard0001"));
    auto chunk = uassertStatusOK(ChunkType::fromConfigBSON(obj));

    ASSERT_EQ("test.mycol-a_10", ChunkType::genLegacyID(nss, minBound));
    ASSERT_EQ(ChunkType::genLegacyID(nss, minBound), chunk.getLegacyName());
}

// TODO SERVER-44034: Delete this test.
TEST(ChunkType, ToConfigBSONLegacyIDUses42_idFormat) {
    NamespaceString nss("test.mycol");
    auto minBound = BSON("a" << 10);
    ChunkVersion chunkVersion(1, 2, OID::gen());

    BSONObj obj =
        BSON(ChunkType::name(OID::gen())
             << ChunkType::ns(nss.ns()) << ChunkType::min(minBound)
             << ChunkType::max(BSON("a" << 20)) << "lastmod" << Timestamp(chunkVersion.toLong())
             << "lastmodEpoch" << chunkVersion.epoch() << ChunkType::shard("shard0001"));
    auto chunk = uassertStatusOK(ChunkType::fromConfigBSON(obj));

    ASSERT_BSONOBJ_EQ(chunk.toConfigBSONLegacyID(),
                      BSON("_id" << ChunkType::genLegacyID(nss, minBound)
                                 << ChunkType::ns("test.mycol") << ChunkType::min(minBound)
                                 << ChunkType::max(BSON("a" << 20)) << ChunkType::shard("shard0001")
                                 << "lastmod" << Timestamp(chunkVersion.toLong()) << "lastmodEpoch"
                                 << chunkVersion.epoch()));
}

}  // namespace
}  // namespace mongo
