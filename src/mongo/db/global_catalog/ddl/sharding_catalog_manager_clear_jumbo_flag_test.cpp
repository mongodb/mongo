/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <string>

namespace mongo {
namespace {


const KeyPattern kKeyPattern(BSON("x" << 1));

class ClearJumboFlagTest : public ConfigServerTestFixture {
public:
    ChunkRange jumboChunk() {
        return ChunkRange(BSON("x" << MINKEY), BSON("x" << 0));
    }

    ChunkRange nonJumboChunk() {
        return ChunkRange(BSON("x" << 0), BSON("x" << BSONType::maxKey));
    }

protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();
        ShardType shard;
        shard.setName(_shardName);
        shard.setHost("shard:12");
        setupShards({shard});
    }

    void makeCollection(const NamespaceString& nss,
                        const UUID& collUuid,
                        const OID& epoch,
                        const Timestamp& timestamp) {
        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(collUuid);
        chunk.setVersion(ChunkVersion({epoch, timestamp}, {12, 7}));
        chunk.setShard(_shardName);
        chunk.setRange(jumboChunk());
        chunk.setJumbo(true);

        ChunkType otherChunk;
        otherChunk.setName(OID::gen());
        otherChunk.setCollectionUUID(collUuid);
        otherChunk.setVersion(ChunkVersion({epoch, timestamp}, {14, 7}));
        otherChunk.setShard(_shardName);
        otherChunk.setRange(nonJumboChunk());

        setupCollection(nss, kKeyPattern, {chunk, otherChunk});
    }

    const std::string _shardName = "shard";
    const NamespaceString _nss1 =
        NamespaceString::createNamespaceString_forTest("TestDB.TestColl1");
    const NamespaceString _nss2 =
        NamespaceString::createNamespaceString_forTest("TestDB.TestColl2");
};

TEST_F(ClearJumboFlagTest, ClearJumboShouldBumpVersion) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collUuid = UUID::gen();
        const auto collEpoch = OID::gen();
        makeCollection(nss, collUuid, collEpoch, collTimestamp);

        ShardingCatalogManager::get(operationContext())
            ->clearJumboFlag(operationContext(), nss, collEpoch, jumboChunk());

        auto chunkDoc = uassertStatusOK(getChunkDoc(
            operationContext(), collUuid, jumboChunk().getMin(), collEpoch, collTimestamp));
        ASSERT_FALSE(chunkDoc.getJumbo());
        auto chunkVersion = chunkDoc.getVersion();
        ASSERT_EQ(ChunkVersion({collEpoch, collTimestamp}, {15, 0}), chunkVersion);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(ClearJumboFlagTest, ClearJumboShouldNotBumpVersionIfChunkNotJumbo) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collUuid = UUID::gen();
        const auto collEpoch = OID::gen();
        makeCollection(nss, collUuid, collEpoch, collTimestamp);

        ShardingCatalogManager::get(operationContext())
            ->clearJumboFlag(operationContext(), nss, collEpoch, nonJumboChunk());

        auto chunkDoc = uassertStatusOK(getChunkDoc(
            operationContext(), collUuid, nonJumboChunk().getMin(), collEpoch, collTimestamp));
        ASSERT_FALSE(chunkDoc.getJumbo());
        ASSERT_EQ(ChunkVersion({collEpoch, collTimestamp}, {14, 7}), chunkDoc.getVersion());
    };

    test(_nss2, Timestamp(42));
}

TEST_F(ClearJumboFlagTest, AssertsOnEpochMismatch) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collUuid = UUID::gen();
        const auto collEpoch = OID::gen();
        makeCollection(nss, collUuid, collEpoch, collTimestamp);

        ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                               ->clearJumboFlag(operationContext(), nss, OID::gen(), jumboChunk()),
                           AssertionException,
                           ErrorCodes::StaleEpoch);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(ClearJumboFlagTest, AssertsIfChunkCantBeFound) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();
        const auto collUuid = UUID::gen();
        makeCollection(nss, collUuid, collEpoch, collTimestamp);

        ChunkRange imaginaryChunk(BSON("x" << 0), BSON("x" << 10));
        ASSERT_THROWS(ShardingCatalogManager::get(operationContext())
                          ->clearJumboFlag(operationContext(), nss, OID::gen(), imaginaryChunk),
                      AssertionException);
    };

    test(_nss2, Timestamp(42));
}

}  // namespace
}  // namespace mongo
