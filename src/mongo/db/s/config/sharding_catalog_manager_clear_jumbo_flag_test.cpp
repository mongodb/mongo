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
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_test_fixture.h"

namespace mongo {
namespace {

using unittest::assertGet;


class ClearJumboFlagTest : public ConfigServerTestFixture {
public:
    const NamespaceString& ns() {
        return _namespace;
    }

    const OID& epoch() {
        return _epoch;
    }

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
        shard.setName("shard");
        shard.setHost("shard:12");

        setupShards({shard});

        CollectionType collection;
        collection.setNs(_namespace);
        collection.setEpoch(_epoch);
        collection.setKeyPattern(BSON("x" << 1));

        ASSERT_OK(insertToConfigCollection(
            operationContext(), CollectionType::ConfigNS, collection.toBSON()));

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setNS(_namespace);
        chunk.setVersion({12, 7, _epoch});
        chunk.setShard(shard.getName());
        chunk.setMin(jumboChunk().getMin());
        chunk.setMax(jumboChunk().getMax());
        chunk.setJumbo(true);

        ChunkType otherChunk;
        otherChunk.setName(OID::gen());
        otherChunk.setNS(_namespace);
        otherChunk.setVersion({14, 7, _epoch});
        otherChunk.setShard(shard.getName());
        otherChunk.setMin(nonJumboChunk().getMin());
        otherChunk.setMax(nonJumboChunk().getMax());

        setupChunks({chunk, otherChunk});
    }

private:
    const NamespaceString _namespace{"TestDB.TestColl"};
    const OID _epoch{OID::gen()};
};

TEST_F(ClearJumboFlagTest, ClearJumboShouldBumpVersion) {
    ShardingCatalogManager::get(operationContext())
        ->clearJumboFlag(operationContext(), ns(), epoch(), jumboChunk());

    auto chunkDoc = uassertStatusOK(getChunkDoc(operationContext(), jumboChunk().getMin()));
    ASSERT_FALSE(chunkDoc.getJumbo());
    ASSERT_EQ(ChunkVersion(15, 0, epoch()), chunkDoc.getVersion());
}

TEST_F(ClearJumboFlagTest, ClearJumboShouldNotBumpVersionIfChunkNotJumbo) {
    ShardingCatalogManager::get(operationContext())
        ->clearJumboFlag(operationContext(), ns(), epoch(), nonJumboChunk());

    auto chunkDoc = uassertStatusOK(getChunkDoc(operationContext(), nonJumboChunk().getMin()));
    ASSERT_FALSE(chunkDoc.getJumbo());
    ASSERT_EQ(ChunkVersion(14, 7, epoch()), chunkDoc.getVersion());
}

TEST_F(ClearJumboFlagTest, AssertsOnEpochMismatch) {
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->clearJumboFlag(operationContext(), ns(), OID::gen(), jumboChunk()),
                       AssertionException,
                       ErrorCodes::StaleEpoch);
}

TEST_F(ClearJumboFlagTest, AssertsIfChunkCantBeFound) {
    ChunkRange imaginaryChunk(BSON("x" << 0), BSON("x" << 10));
    ASSERT_THROWS(ShardingCatalogManager::get(operationContext())
                      ->clearJumboFlag(operationContext(), ns(), OID::gen(), imaginaryChunk),
                  AssertionException);
}

}  // namespace
}  // namespace mongo
