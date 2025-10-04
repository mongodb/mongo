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

#include "mongo/db/global_catalog/type_chunk.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/logv2/log_debug.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

using unittest::assertGet;

const BSONObj kMin = BSON("a" << 10);
const BSONObj kMax = BSON("a" << 20);
const ShardId kShard("shard0000");

TEST(ChunkType, MissingConfigRequiredFields) {
    const auto collUuid = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(1, 1);

    ChunkVersion chunkVersion({collEpoch, collTimestamp}, {1, 2});

    BSONObj objModNS =
        BSON(ChunkType::name(OID::gen())
             << ChunkType::min(BSON("a" << 10 << "b" << 10)) << ChunkType::max(BSON("a" << 20))
             << "lastmod" << Timestamp(chunkVersion.toLong()) << ChunkType::shard("shard0001"));
    StatusWith<ChunkType> chunkRes =
        ChunkType::parseFromConfigBSON(objModNS, collEpoch, collTimestamp);
    ASSERT_FALSE(chunkRes.isOK());

    BSONObj objModKeys = BSON(ChunkType::name(OID::gen())
                              << ChunkType::collectionUUID() << collUuid << "lastmod"
                              << Timestamp(chunkVersion.toLong()) << ChunkType::shard("shard0001"));
    chunkRes = ChunkType::parseFromConfigBSON(objModKeys, collEpoch, collTimestamp);
    ASSERT_FALSE(chunkRes.isOK());

    BSONObj objModShard = BSON(
        ChunkType::name(OID::gen())
        << ChunkType::collectionUUID() << collUuid << ChunkType::min(BSON("a" << 10 << "b" << 10))
        << ChunkType::max(BSON("a" << 20)) << "lastmod" << Timestamp(chunkVersion.toLong()));
    chunkRes = ChunkType::parseFromConfigBSON(objModShard, collEpoch, collTimestamp);
    ASSERT_FALSE(chunkRes.isOK());

    BSONObj objModVersion = BSON(
        ChunkType::name(OID::gen())
        << ChunkType::collectionUUID() << collUuid << ChunkType::min(BSON("a" << 10 << "b" << 10))
        << ChunkType::max(BSON("a" << 20)) << ChunkType::shard("shard0001"));
    chunkRes = ChunkType::parseFromConfigBSON(objModVersion, collEpoch, collTimestamp);
    ASSERT_FALSE(chunkRes.isOK());
}

TEST(ChunkType, MissingShardRequiredFields) {
    const OID epoch = OID::gen();
    const Timestamp timestamp(1, 1);
    ChunkVersion chunkVersion({epoch, timestamp}, {1, 2});
    const auto lastmod = Timestamp(chunkVersion.toLong());

    BSONObj objModMin =
        BSON(ChunkType::max(kMax) << ChunkType::shard(kShard.toString()) << "lastmod" << lastmod);
    StatusWith<ChunkType> chunkRes = ChunkType::parseFromShardBSON(objModMin, epoch, timestamp);
    ASSERT_EQUALS(chunkRes.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(chunkRes.getStatus().reason(), ChunkType::minShardID.name());

    BSONObj objModMax = BSON(ChunkType::minShardID(kMin)
                             << ChunkType::shard(kShard.toString()) << "lastmod" << lastmod);
    chunkRes = ChunkType::parseFromShardBSON(objModMax, epoch, timestamp);
    ASSERT_EQUALS(chunkRes.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(chunkRes.getStatus().reason(), ChunkType::max.name());

    BSONObj objModShard =
        BSON(ChunkType::minShardID(kMin) << ChunkType::max(kMax) << "lastmod" << lastmod);
    chunkRes = ChunkType::parseFromShardBSON(objModShard, epoch, timestamp);
    ASSERT_EQUALS(chunkRes.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(chunkRes.getStatus().reason(), ChunkType::shard.name());

    BSONObj objModLastmod = BSON(ChunkType::minShardID(kMin)
                                 << ChunkType::max(kMax) << ChunkType::shard(kShard.toString()));
    chunkRes = ChunkType::parseFromShardBSON(objModLastmod, epoch, timestamp);
    ASSERT_EQUALS(chunkRes.getStatus(), ErrorCodes::NoSuchKey);
}

TEST(ChunkType, ToFromShardBSON) {
    const OID collEpoch = OID::gen();
    const Timestamp collTimestamp(1, 1);

    ChunkVersion chunkVersion({collEpoch, collTimestamp}, {1, 2});
    auto lastmod = Timestamp(chunkVersion.toLong());

    BSONObj obj = BSON(ChunkType::minShardID(kMin)
                       << ChunkType::max(kMax) << ChunkType::shard(kShard.toString()) << "lastmod"
                       << lastmod);
    ChunkType shardChunk = assertGet(ChunkType::parseFromShardBSON(obj, collEpoch, collTimestamp));

    ASSERT_BSONOBJ_EQ(obj, shardChunk.toShardBSON());

    ASSERT_BSONOBJ_EQ(kMin, shardChunk.getMin());
    ASSERT_BSONOBJ_EQ(kMax, shardChunk.getMax());
    ASSERT_EQUALS(kShard, shardChunk.getShard());
    ASSERT_EQUALS(chunkVersion, shardChunk.getVersion());
}

TEST(ChunkType, MinAndMaxShardKeysDifferInNumberOfKeys) {
    const auto collUuid = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(1);

    ChunkVersion chunkVersion({collEpoch, collTimestamp}, {1, 2});
    const auto onCurrentShardSince = Timestamp(2);
    BSONObj obj = BSON(
        ChunkType::name(OID::gen())
        << ChunkType::collectionUUID() << collUuid << ChunkType::min(BSON("a" << 10 << "b" << 10))
        << ChunkType::max(BSON("a" << 20)) << "lastmod" << Timestamp(chunkVersion.toLong())
        << "lastmodEpoch" << chunkVersion.epoch() << "lastmodTimestamp"
        << chunkVersion.getTimestamp() << ChunkType::shard("shard0001")
        << ChunkType::onCurrentShardSince() << onCurrentShardSince << ChunkType::history()
        << BSON_ARRAY(BSON(ChunkHistoryBase::kValidAfterFieldName
                           << onCurrentShardSince << ChunkHistoryBase::kShardFieldName
                           << "shard0001")));
    StatusWith<ChunkType> chunkRes = ChunkType::parseFromConfigBSON(obj, collEpoch, collTimestamp);
    ASSERT_OK(chunkRes.getStatus());
    ASSERT_FALSE(chunkRes.getValue().validate().isOK());
}

TEST(ChunkType, MinAndMaxShardKeysDifferInKeyNames) {
    const auto collUuid = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(1);

    ChunkVersion chunkVersion({collEpoch, collTimestamp}, {1, 2});
    const auto onCurrentShardSince = Timestamp(2);
    BSONObj obj =
        BSON(ChunkType::name(OID::gen())
             << ChunkType::collectionUUID() << collUuid << ChunkType::min(BSON("a" << 10))
             << ChunkType::max(BSON("b" << 20)) << "lastmod" << Timestamp(chunkVersion.toLong())
             << "lastmodEpoch" << chunkVersion.epoch() << "lastmodTimestamp"
             << chunkVersion.getTimestamp() << ChunkType::shard("shard0001")
             << ChunkType::onCurrentShardSince() << onCurrentShardSince << ChunkType::history()
             << BSON_ARRAY(BSON(ChunkHistoryBase::kValidAfterFieldName
                                << onCurrentShardSince << ChunkHistoryBase::kShardFieldName
                                << "shard0001")));
    StatusWith<ChunkType> chunkRes = ChunkType::parseFromConfigBSON(obj, collEpoch, collTimestamp);
    ASSERT_OK(chunkRes.getStatus());
    ASSERT_FALSE(chunkRes.getValue().validate().isOK());
}

TEST(ChunkType, MinToMaxNotAscending) {
    const auto collUuid = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(1);

    ChunkVersion chunkVersion({collEpoch, collTimestamp}, {1, 2});
    const auto onCurrentShardSince = Timestamp(2);
    BSONObj obj =
        BSON(ChunkType::name(OID::gen())
             << ChunkType::collectionUUID() << collUuid << ChunkType::min(BSON("a" << 20))
             << ChunkType::max(BSON("a" << 10)) << "lastmod" << Timestamp(chunkVersion.toLong())
             << "lastmodEpoch" << chunkVersion.epoch() << ChunkType::shard("shard0001")
             << ChunkType::onCurrentShardSince() << onCurrentShardSince << ChunkType::history()
             << BSON_ARRAY(BSON(ChunkHistoryBase::kValidAfterFieldName
                                << onCurrentShardSince << ChunkHistoryBase::kShardFieldName
                                << "shard0001")));
    StatusWith<ChunkType> chunkRes = ChunkType::parseFromConfigBSON(obj, collEpoch, collTimestamp);
    ASSERT_EQ(ErrorCodes::BadValue, chunkRes.getStatus());
}

TEST(ChunkType, ToFromConfigBSON) {
    const auto collUuid = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(1);

    const auto chunkID = OID::gen();
    const auto onCurrentShardSince = Timestamp(4);
    ChunkVersion chunkVersion({collEpoch, collTimestamp}, {1, 2});
    BSONObj obj =
        BSON(ChunkType::name(chunkID)
             << ChunkType::collectionUUID() << collUuid << ChunkType::min(BSON("a" << 10))
             << ChunkType::max(BSON("a" << 20)) << ChunkType::shard("shard0001") << "lastmod"
             << Timestamp(chunkVersion.toLong()) << ChunkType::onCurrentShardSince()
             << onCurrentShardSince << ChunkType::history()
             << BSON_ARRAY(BSON(ChunkHistoryBase::kValidAfterFieldName
                                << onCurrentShardSince << ChunkHistoryBase::kShardFieldName
                                << "shard0001")));
    StatusWith<ChunkType> chunkRes = ChunkType::parseFromConfigBSON(obj, collEpoch, collTimestamp);
    ASSERT_OK(chunkRes.getStatus());
    ChunkType chunk = chunkRes.getValue();

    ASSERT_BSONOBJ_EQ(chunk.toConfigBSON(), obj);

    ASSERT_EQUALS(chunk.getName(), chunkID);
    ASSERT_EQUALS(chunk.getCollectionUUID(), collUuid);
    ASSERT_BSONOBJ_EQ(chunk.getMin(), BSON("a" << 10));
    ASSERT_BSONOBJ_EQ(chunk.getMax(), BSON("a" << 20));
    ASSERT_EQUALS(chunk.getVersion().toLong(), chunkVersion.toLong());
    ASSERT_EQUALS(chunk.getVersion().epoch(), chunkVersion.epoch());
    ASSERT_EQUALS(chunk.getShard(), "shard0001");
    ASSERT_EQUALS(*chunk.getOnCurrentShardSince(), onCurrentShardSince);
    ASSERT_OK(chunk.validate());
}

TEST(ChunkType, BadType) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(1);

    BSONObj obj = BSON(ChunkType::name() << 0);
    StatusWith<ChunkType> chunkRes = ChunkType::parseFromConfigBSON(obj, collEpoch, collTimestamp);
    ASSERT_FALSE(chunkRes.isOK());
}

TEST(ChunkType, UUIDPresentAndNsMissing) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(1);

    ChunkVersion chunkVersion({collEpoch, collTimestamp}, {1, 2});
    const auto onCurrentShardSince = Timestamp(2);

    BSONObj objModNS = BSON(
        ChunkType::name(OID::gen())
        << ChunkType::collectionUUID() << mongo::UUID::gen()
        << ChunkType::min(BSON("a" << 10 << "b" << 10)) << ChunkType::max(BSON("a" << 20))
        << "lastmod" << Timestamp(chunkVersion.toLong()) << "lastmodEpoch" << chunkVersion.epoch()
        << "lastmodTimestamp" << chunkVersion.getTimestamp() << ChunkType::shard("shard0001")
        << ChunkType::onCurrentShardSince() << onCurrentShardSince << ChunkType::history()
        << BSON_ARRAY(BSON(ChunkHistoryBase::kValidAfterFieldName
                           << onCurrentShardSince << ChunkHistoryBase::kShardFieldName
                           << "shard0001")));
    StatusWith<ChunkType> chunkRes =
        ChunkType::parseFromConfigBSON(objModNS, collEpoch, collTimestamp);
    ASSERT_TRUE(chunkRes.isOK());
}

TEST(ChunkType, ParseFromNetworkRequest) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(1, 0);

    ChunkVersion chunkVersion({collEpoch, collTimestamp}, {1, 2});
    const auto onCurrentShardSince = Timestamp(2, 0);

    auto chunk = assertGet(ChunkType::parseFromNetworkRequest(
        BSON(ChunkType::name(OID::gen())
             << ChunkType::collectionUUID() << mongo::UUID::gen()
             << ChunkType::min(BSON("a" << 10 << "b" << 10)) << ChunkType::max(BSON("a" << 20))
             << "lastmod"
             << BSON("e" << chunkVersion.epoch() << "t" << chunkVersion.getTimestamp() << "v"
                         << Timestamp(chunkVersion.toLong()))
             << ChunkType::shard("shard0001") << ChunkType::onCurrentShardSince()
             << onCurrentShardSince << ChunkType::history()
             << BSON_ARRAY(BSON(ChunkHistoryBase::kValidAfterFieldName
                                << onCurrentShardSince << ChunkHistoryBase::kShardFieldName
                                << "shard0001")))));

    ASSERT_EQ("shard0001", chunk.getShard());
    ASSERT_EQ(chunkVersion, chunk.getVersion());
}

}  // namespace
}  // namespace mongo
