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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"

namespace mongo {
namespace {

using unittest::assertGet;

const KeyPattern kKeyPattern(BSON("x" << 1));

class ClearJumboFlagTest : public ConfigServerTestFixture {
public:
    ChunkRange jumboChunk() {
        return ChunkRange(BSON("x" << MINKEY), BSON("x" << 0));
    }

    ChunkRange nonJumboChunk() {
        return ChunkRange(BSON("x" << 0), BSON("x" << MaxKey));
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
                        const boost::optional<Timestamp>& timestamp) {
        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setNS(nss);
        chunk.setCollectionUUID(collUuid);
        chunk.setVersion({12, 7, epoch, timestamp});
        chunk.setShard(_shardName);
        chunk.setMin(jumboChunk().getMin());
        chunk.setMax(jumboChunk().getMax());
        chunk.setJumbo(true);

        ChunkType otherChunk;
        otherChunk.setName(OID::gen());
        otherChunk.setNS(nss);
        otherChunk.setCollectionUUID(collUuid);
        otherChunk.setVersion({14, 7, epoch, timestamp});
        otherChunk.setShard(_shardName);
        otherChunk.setMin(nonJumboChunk().getMin());
        otherChunk.setMax(nonJumboChunk().getMax());

        setupCollection(nss, kKeyPattern, {chunk, otherChunk});
    }

    const std::string _shardName = "shard";
    const NamespaceString _nss1{"TestDB.TestColl1"};
    const NamespaceString _nss2{"TestDB.TestColl2"};
};

TEST_F(ClearJumboFlagTest, ClearJumboShouldBumpVersion) {
    auto test = [&](const NamespaceString& nss, const boost::optional<Timestamp>& collTimestamp) {
        const auto collUuid = UUID::gen();
        const auto collEpoch = OID::gen();
        makeCollection(nss, collUuid, collEpoch, collTimestamp);

        ShardingCatalogManager::get(operationContext())
            ->clearJumboFlag(operationContext(), nss, collEpoch, jumboChunk());

        const auto nssOrUuid =
            collTimestamp ? NamespaceStringOrUUID(nss.db().toString(), collUuid) : nss;

        auto chunkDoc = uassertStatusOK(getChunkDoc(
            operationContext(), nssOrUuid, jumboChunk().getMin(), collEpoch, collTimestamp));
        ASSERT_FALSE(chunkDoc.getJumbo());
        auto chunkVersion = chunkDoc.getVersion();
        ASSERT_EQ(ChunkVersion(15, 0, collEpoch, collTimestamp), chunkVersion);
    };

    test(_nss1, boost::none /* timestamp */);
    test(_nss2, Timestamp(42));
}

TEST_F(ClearJumboFlagTest, ClearJumboShouldNotBumpVersionIfChunkNotJumbo) {
    auto test = [&](const NamespaceString& nss, const boost::optional<Timestamp>& collTimestamp) {
        const auto collUuid = UUID::gen();
        const auto collEpoch = OID::gen();
        makeCollection(nss, collUuid, collEpoch, collTimestamp);

        ShardingCatalogManager::get(operationContext())
            ->clearJumboFlag(operationContext(), nss, collEpoch, nonJumboChunk());

        const auto nssOrUuid =
            collTimestamp ? NamespaceStringOrUUID(nss.db().toString(), collUuid) : nss;

        auto chunkDoc = uassertStatusOK(getChunkDoc(
            operationContext(), nssOrUuid, nonJumboChunk().getMin(), collEpoch, collTimestamp));
        ASSERT_FALSE(chunkDoc.getJumbo());
        ASSERT_EQ(ChunkVersion(14, 7, collEpoch, collTimestamp), chunkDoc.getVersion());
    };

    test(_nss1, boost::none /* timestamp */);
    test(_nss2, Timestamp(42));
}

TEST_F(ClearJumboFlagTest, AssertsOnEpochMismatch) {
    auto test = [&](const NamespaceString& nss, const boost::optional<Timestamp>& collTimestamp) {
        const auto collUuid = UUID::gen();
        const auto collEpoch = OID::gen();
        makeCollection(nss, collUuid, collEpoch, collTimestamp);

        ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                               ->clearJumboFlag(operationContext(), nss, OID::gen(), jumboChunk()),
                           AssertionException,
                           ErrorCodes::StaleEpoch);
    };

    test(_nss1, boost::none /* timestamp */);
    test(_nss2, Timestamp(42));
}

TEST_F(ClearJumboFlagTest, AssertsIfChunkCantBeFound) {
    auto test = [&](const NamespaceString& nss, const boost::optional<Timestamp>& collTimestamp) {
        const auto collEpoch = OID::gen();
        const auto collUuid = UUID::gen();
        makeCollection(nss, collUuid, collEpoch, collTimestamp);

        ChunkRange imaginaryChunk(BSON("x" << 0), BSON("x" << 10));
        ASSERT_THROWS(ShardingCatalogManager::get(operationContext())
                          ->clearJumboFlag(operationContext(), nss, OID::gen(), imaginaryChunk),
                      AssertionException);
    };

    test(_nss1, boost::none /* timestamp */);
    test(_nss2, Timestamp(42));
}

}  // namespace
}  // namespace mongo
