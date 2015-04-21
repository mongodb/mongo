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

    TEST(ChunkType, MissingRequiredFields) {
        ChunkType chunk;
        BSONArray version = BSON_ARRAY(Date_t(1) << OID::gen());

        BSONObj objModNS = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                                ChunkType::min(BSON("a" << 10 << "b" << 10)) <<
                                ChunkType::max(BSON("a" << 20)) <<
                                ChunkType::version(version) <<
                                ChunkType::shard("shard0001"));
        StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(objModNS);
        ASSERT_FALSE(chunkRes.isOK());

        BSONObj objModName = BSON(ChunkType::ns("test.mycol") <<
                                  ChunkType::min(BSON("a" << 10 << "b" << 10)) <<
                                  ChunkType::max(BSON("a" << 20)) <<
                                  ChunkType::version(version) <<
                                  ChunkType::shard("shard0001"));
        chunkRes = ChunkType::fromBSON(objModName);
        ASSERT_FALSE(chunkRes.isOK());

        BSONObj objModKeys = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                                  ChunkType::ns("test.mycol") <<
                                  ChunkType::version(version) <<
                                  ChunkType::shard("shard0001"));
        chunkRes = ChunkType::fromBSON(objModKeys);
        ASSERT_FALSE(chunkRes.isOK());

        BSONObj objModShard = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                                   ChunkType::ns("test.mycol") <<
                                   ChunkType::min(BSON("a" << 10 << "b" << 10)) <<
                                   ChunkType::max(BSON("a" << 20)) <<
                                   ChunkType::version(version));
        chunkRes = ChunkType::fromBSON(objModShard);
        ASSERT_FALSE(chunkRes.isOK());
    }

    TEST(ChunkType, DifferentNumberOfColumns) {
        BSONArray version = BSON_ARRAY(Date_t(1) << OID::gen());
        BSONObj obj = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                           ChunkType::ns("test.mycol") <<
                           ChunkType::min(BSON("a" << 10 << "b" << 10)) <<
                           ChunkType::max(BSON("a" << 20)) <<
                           ChunkType::version(version) <<
                           ChunkType::shard("shard0001"));
        StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(obj);
        ASSERT(chunkRes.isOK());
        ASSERT_FALSE(chunkRes.getValue().validate().isOK());
    }

    TEST(ChunkType, DifferentColumns) {
        BSONArray version = BSON_ARRAY(Date_t(1) << OID::gen());
        BSONObj obj = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                           ChunkType::ns("test.mycol") <<
                           ChunkType::min(BSON("a" << 10)) <<
                           ChunkType::max(BSON("b" << 20)) <<
                           ChunkType::version(version) <<
                           ChunkType::shard("shard0001"));
        StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(obj);
        ASSERT(chunkRes.isOK());
        ASSERT_FALSE(chunkRes.getValue().validate().isOK());
    }

    TEST(ChunkType, NotAscending) {
        BSONArray version = BSON_ARRAY(Date_t(1) << OID::gen());
        BSONObj obj = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                           ChunkType::ns("test.mycol") <<
                           ChunkType::min(BSON("a" << 20)) <<
                           ChunkType::max(BSON("a" << 10)) <<
                           ChunkType::version(version) <<
                           ChunkType::shard("shard0001"));
        StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(obj);
        ASSERT(chunkRes.isOK());
        ASSERT_FALSE(chunkRes.getValue().validate().isOK());
    }

    TEST(ChunkType, NewFormatVersion) {
        ChunkType chunk;
        OID epoch = OID::gen();
        BSONArray version = BSON_ARRAY(Date_t(1) << epoch);
        BSONObj obj = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                           ChunkType::ns("test.mycol") <<
                           ChunkType::min(BSON("a" << 10)) <<
                           ChunkType::max(BSON("a" << 20)) <<
                           ChunkType::version(version) <<
                           ChunkType::shard("shard0001"));
        StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(obj);
        ASSERT(chunkRes.isOK());
        chunk = chunkRes.getValue();

        ASSERT_EQUALS(chunk.getName(), "test.mycol-a_MinKey");
        ASSERT_EQUALS(chunk.getNS(), "test.mycol");
        ASSERT_EQUALS(chunk.getMin(), BSON("a" << 10));
        ASSERT_EQUALS(chunk.getMax(), BSON("a" << 20));
        ChunkVersion fetchedVersion = chunk.getVersion();
        ASSERT_EQUALS(fetchedVersion._combined, 1ULL);
        ASSERT_EQUALS(fetchedVersion._epoch, epoch);
        ASSERT_EQUALS(chunk.getShard(), "shard0001");
        ASSERT_TRUE(chunk.validate().isOK());
    }

    TEST(ChunkType, OldFormatVersion) {
        ChunkType chunk;
        OID epoch = OID::gen();
        BSONObj obj = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                           ChunkType::ns("test.mycol") <<
                           ChunkType::min(BSON("a" << 10)) <<
                           ChunkType::max(BSON("a" << 20)) <<
                           ChunkType::DEPRECATED_lastmod(Date_t(1)) <<
                           ChunkType::DEPRECATED_epoch(epoch) <<
                           ChunkType::shard("shard0001"));
        StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(obj);
        ASSERT(chunkRes.isOK());
        chunk = chunkRes.getValue();

        ASSERT_EQUALS(chunk.getName(), "test.mycol-a_MinKey");
        ASSERT_EQUALS(chunk.getNS(), "test.mycol");
        ASSERT_EQUALS(chunk.getMin(), BSON("a" << 10));
        ASSERT_EQUALS(chunk.getMax(), BSON("a" << 20));
        ChunkVersion fetchedVersion = chunk.getVersion();
        ASSERT_EQUALS(fetchedVersion._combined, 1ULL);
        ASSERT_EQUALS(fetchedVersion._epoch, epoch);
        ASSERT_EQUALS(chunk.getShard(), "shard0001");
        ASSERT_TRUE(chunk.validate().isOK());
    }

    TEST(ChunkType, BadType) {
        BSONObj obj = BSON(ChunkType::name() << 0);
        StatusWith<ChunkType> chunkRes = ChunkType::fromBSON(obj);
        ASSERT_FALSE(chunkRes.isOK());
    }

} // unnamed namespace
