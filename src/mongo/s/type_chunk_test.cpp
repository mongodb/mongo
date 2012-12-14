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
 */

#include "mongo/pch.h"

#include "mongo/bson/oid.h"
#include "mongo/bson/util/misc.h" // for Date_t
#include "mongo/s/type_chunk.h"
#include "mongo/s/util.h" // for ShardChunkVersion
#include "mongo/unittest/unittest.h"

namespace {

    using std::string;
    using mongo::BSONArray;
    using mongo::BSONObj;
    using mongo::ChunkType;
    using mongo::Date_t;
    using mongo::OID;
    using mongo::ShardChunkVersion;

    TEST(Validity, MissingFields) {
        ChunkType chunk;
        BSONArray version = BSON_ARRAY(Date_t(1) << OID::gen());

        BSONObj objModNS = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                                ChunkType::min(BSON("a" << 10 << "b" << 10)) <<
                                ChunkType::max(BSON("a" << 20)) <<
                                ChunkType::version(version) <<
                                ChunkType::shard("shard0001"));
        string errMsg;
        ASSERT(chunk.parseBSON(objModNS, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(chunk.isValid(NULL));

        BSONObj objModName = BSON(ChunkType::ns("test.mycol") <<
                                  ChunkType::min(BSON("a" << 10 << "b" << 10)) <<
                                  ChunkType::max(BSON("a" << 20)) <<
                                  ChunkType::version(version) <<
                                  ChunkType::shard("shard0001"));

        ASSERT(chunk.parseBSON(objModName, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(chunk.isValid(NULL));

        BSONObj objModKeys = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                                  ChunkType::ns("test.mycol") <<
                                  ChunkType::version(version) <<
                                  ChunkType::shard("shard0001"));

        ASSERT(chunk.parseBSON(objModKeys, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(chunk.isValid(NULL));

        BSONObj objModVersion = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                                     ChunkType::ns("test.mycol") <<
                                     ChunkType::min(BSON("a" << 10 << "b" << 10)) <<
                                     ChunkType::max(BSON("a" << 20)) <<
                                     ChunkType::shard("shard0001"));

        ASSERT(chunk.parseBSON(objModVersion, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(chunk.isValid(NULL));

        BSONObj objModShard = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                                   ChunkType::ns("test.mycol") <<
                                   ChunkType::min(BSON("a" << 10 << "b" << 10)) <<
                                   ChunkType::max(BSON("a" << 20)) <<
                                   ChunkType::version(version) <<
                                   ChunkType::shard("shard0001"));

        ASSERT(chunk.parseBSON(objModShard, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(chunk.isValid(NULL));
    }

    TEST(MinMaxValidity, DifferentNumberOfColumns) {
        ChunkType chunk;
        BSONArray version = BSON_ARRAY(Date_t(1) << OID::gen());
        BSONObj obj = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                           ChunkType::ns("test.mycol") <<
                           ChunkType::min(BSON("a" << 10 << "b" << 10)) <<
                           ChunkType::max(BSON("a" << 20)) <<
                           ChunkType::version(version) <<
                           ChunkType::shard("shard0001"));
        string errMsg;
        ASSERT(chunk.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(chunk.isValid(NULL));
    }

    TEST(MinMaxValidity, DifferentColumns) {
        ChunkType chunk;
        BSONArray version = BSON_ARRAY(Date_t(1) << OID::gen());
        BSONObj obj = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                           ChunkType::ns("test.mycol") <<
                           ChunkType::min(BSON("a" << 10)) <<
                           ChunkType::max(BSON("b" << 20)) <<
                           ChunkType::version(version) <<
                           ChunkType::shard("shard0001"));
        string errMsg;
        ASSERT(chunk.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(chunk.isValid(NULL));
    }

    TEST(MinMaxValidity, NotAscending) {
        ChunkType chunk;
        BSONArray version = BSON_ARRAY(Date_t(1) << OID::gen());
        BSONObj obj = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                           ChunkType::ns("test.mycol") <<
                           ChunkType::min(BSON("a" << 20)) <<
                           ChunkType::max(BSON("a" << 10)) <<
                           ChunkType::version(version) <<
                           ChunkType::shard("shard0001"));
        string errMsg;
        ASSERT(chunk.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(chunk.isValid(NULL));
    }

    TEST(Compatibility, NewFormatVersion) {
        ChunkType chunk;
        OID epoch = OID::gen();
        BSONArray version = BSON_ARRAY(Date_t(1) << epoch);
        BSONObj obj = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                           ChunkType::ns("test.mycol") <<
                           ChunkType::min(BSON("a" << 10)) <<
                           ChunkType::max(BSON("a" << 20)) <<
                           ChunkType::version(version) <<
                           ChunkType::shard("shard0001"));
        string errMsg;
        ASSERT(chunk.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(chunk.isValid(NULL));
        ASSERT_EQUALS(chunk.getName(), "test.mycol-a_MinKey");
        ASSERT_EQUALS(chunk.getNS(), "test.mycol");
        ASSERT_EQUALS(chunk.getMin(), BSON("a" << 10));
        ASSERT_EQUALS(chunk.getMax(), BSON("a" << 20));
        ShardChunkVersion fetchedVersion = chunk.getVersion();
        ASSERT_EQUALS(fetchedVersion._combined, 1ULL);
        ASSERT_EQUALS(fetchedVersion._epoch, epoch);
        ASSERT_EQUALS(chunk.getShard(), "shard0001");
    }

    TEST(Compatibility, OldFormatVersion) {
        ChunkType chunk;
        OID epoch = OID::gen();
        BSONObj obj = BSON(ChunkType::name("test.mycol-a_MinKey") <<
                           ChunkType::ns("test.mycol") <<
                           ChunkType::min(BSON("a" << 10)) <<
                           ChunkType::max(BSON("a" << 20)) <<
                           ChunkType::DEPRECATED_lastmod(Date_t(1)) <<
                           ChunkType::DEPRECATED_epoch(epoch) <<
                           ChunkType::shard("shard0001"));
        string errMsg;
        ASSERT(chunk.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(chunk.isValid(NULL));
        ASSERT_EQUALS(chunk.getName(), "test.mycol-a_MinKey");
        ASSERT_EQUALS(chunk.getNS(), "test.mycol");
        ASSERT_EQUALS(chunk.getMin(), BSON("a" << 10));
        ASSERT_EQUALS(chunk.getMax(), BSON("a" << 20));
        ShardChunkVersion fetchedVersion = chunk.getVersion();
        ASSERT_EQUALS(fetchedVersion._combined, 1ULL);
        ASSERT_EQUALS(fetchedVersion._epoch, epoch);
        ASSERT_EQUALS(chunk.getShard(), "shard0001");
    }

    TEST(Validity, BadType) {
        ChunkType chunk;
        BSONObj obj = BSON(ChunkType::name() << 0);
        string errMsg;
        ASSERT((!chunk.parseBSON(obj, &errMsg)) && (errMsg != ""));
    }

} // unnamed namespace
