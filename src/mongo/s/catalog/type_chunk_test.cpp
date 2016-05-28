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

namespace {

using namespace mongo;

using std::string;
using unittest::assertGet;

TEST(ChunkType, MissingRequiredFields) {
    ChunkVersion chunkVersion(1, 2, OID::gen());

    BSONObj objModNS =
        BSON(ChunkType::name("test.mycol-a_MinKey") << ChunkType::min(BSON("a" << 10 << "b" << 10))
                                                    << ChunkType::max(BSON("a" << 20))
                                                    << "lastmod"
                                                    << Timestamp(chunkVersion.toLong())
                                                    << "lastmodEpoch"
                                                    << chunkVersion.epoch()
                                                    << ChunkType::shard("shard0001"));
    StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(objModNS);
    ASSERT_FALSE(chunkRes.isOK());

    BSONObj objModKeys =
        BSON(ChunkType::name("test.mycol-a_MinKey") << ChunkType::ns("test.mycol") << "lastmod"
                                                    << Timestamp(chunkVersion.toLong())
                                                    << "lastmodEpoch"
                                                    << chunkVersion.epoch()
                                                    << ChunkType::shard("shard0001"));
    chunkRes = ChunkType::fromBSON(objModKeys);
    ASSERT_FALSE(chunkRes.isOK());

    BSONObj objModShard =
        BSON(ChunkType::name("test.mycol-a_MinKey") << ChunkType::ns("test.mycol")
                                                    << ChunkType::min(BSON("a" << 10 << "b" << 10))
                                                    << ChunkType::max(BSON("a" << 20))
                                                    << "lastmod"
                                                    << Timestamp(chunkVersion.toLong())
                                                    << "lastmodEpoch"
                                                    << chunkVersion.epoch());
    chunkRes = ChunkType::fromBSON(objModShard);
    ASSERT_FALSE(chunkRes.isOK());
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
    StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(obj);
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
    StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(obj);
    ASSERT_OK(chunkRes.getStatus());
    ASSERT_FALSE(chunkRes.getValue().validate().isOK());
}

TEST(ChunkType, NotAscending) {
    ChunkVersion chunkVersion(1, 2, OID::gen());
    BSONObj obj = BSON(ChunkType::name("test.mycol-a_MinKey") << ChunkType::ns("test.mycol")
                                                              << ChunkType::min(BSON("a" << 20))
                                                              << ChunkType::max(BSON("a" << 10))
                                                              << "lastmod"
                                                              << Timestamp(chunkVersion.toLong())
                                                              << "lastmodEpoch"
                                                              << chunkVersion.epoch()
                                                              << ChunkType::shard("shard0001"));
    StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(obj);
    ASSERT_OK(chunkRes.getStatus());
    ASSERT_FALSE(chunkRes.getValue().validate().isOK());
}

TEST(ChunkType, CorrectContents) {
    ChunkVersion chunkVersion(1, 2, OID::gen());
    BSONObj obj = BSON(ChunkType::name("test.mycol-a_MinKey") << ChunkType::ns("test.mycol")
                                                              << ChunkType::min(BSON("a" << 10))
                                                              << ChunkType::max(BSON("a" << 20))
                                                              << "lastmod"
                                                              << Timestamp(chunkVersion.toLong())
                                                              << "lastmodEpoch"
                                                              << chunkVersion.epoch()
                                                              << ChunkType::shard("shard0001"));
    StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(obj);
    ASSERT_OK(chunkRes.getStatus());
    ChunkType chunk = chunkRes.getValue();

    ASSERT_EQUALS(chunk.getNS(), "test.mycol");
    ASSERT_EQUALS(chunk.getMin(), BSON("a" << 10));
    ASSERT_EQUALS(chunk.getMax(), BSON("a" << 20));
    ASSERT_EQUALS(chunk.getVersion().toLong(), chunkVersion.toLong());
    ASSERT_EQUALS(chunk.getVersion().epoch(), chunkVersion.epoch());
    ASSERT_EQUALS(chunk.getShard(), "shard0001");
    ASSERT_OK(chunk.validate());
}

TEST(ChunkType, Pre22Format) {
    ChunkType chunk = assertGet(ChunkType::fromBSON(BSON("_id"
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
    ASSERT_EQUALS(chunk.getMin(), BSON("a" << 10));
    ASSERT_EQUALS(chunk.getMax(), BSON("a" << 20));
    ASSERT_EQUALS(chunk.getVersion().toLong(), 1ULL);
    ASSERT(!chunk.getVersion().epoch().isSet());
    ASSERT_EQUALS(chunk.getShard(), "shard0001");
}

TEST(ChunkType, BadType) {
    BSONObj obj = BSON(ChunkType::name() << 0);
    StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(obj);
    ASSERT_FALSE(chunkRes.isOK());
}

}  // unnamed namespace
