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

#include "mongo/db/s/balancer/migration_test_fixture.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_field.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {


void MigrationTestFixture::setUp() {
    setUpAndInitializeConfigDb();
}

std::shared_ptr<RemoteCommandTargeterMock> MigrationTestFixture::shardTargeterMock(
    OperationContext* opCtx, ShardId shardId) {
    return RemoteCommandTargeterMock::get(
        uassertStatusOK(shardRegistry()->getShard(opCtx, shardId))->getTargeter());
}

void MigrationTestFixture::setUpCollection(
    const NamespaceString& collName,
    const UUID& collUUID,
    const ChunkVersion& version,
    boost::optional<TypeCollectionTimeseriesFields> timeseriesFields) {
    CollectionType coll(
        collName, version.epoch(), version.getTimestamp(), Date_t::now(), collUUID, kKeyPattern);
    coll.setTimeseriesFields(std::move(timeseriesFields));
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), CollectionType::ConfigNS, coll.toBSON(), kMajorityWriteConcern));
}

CollectionType MigrationTestFixture::setUpUnsplittableCollection(
    const NamespaceString& collName, const ShardId& shardId, boost::optional<const UUID> collUUID) {
    ChunkVersion defaultVersion({OID::gen(), Timestamp(42)}, {2, 0});
    UUID uuid = collUUID.get_value_or(UUID::gen());
    std::vector<ChunkType> chunks;
    ChunkRange keyRange{kKeyPattern.globalMin(), kKeyPattern.globalMax()};
    chunks.emplace_back(uuid, keyRange, defaultVersion, shardId);
    return setupCollection(
        collName, kKeyPattern, chunks, [](CollectionType& coll) { coll.setUnsplittable(true); });
}

ChunkType MigrationTestFixture::setUpChunk(const UUID& collUUID,
                                           const BSONObj& chunkMin,
                                           const BSONObj& chunkMax,
                                           const ShardId& shardId,
                                           const ChunkVersion& version) {
    ChunkType chunk;
    chunk.setCollectionUUID(collUUID);

    chunk.setRange({chunkMin, chunkMax});
    chunk.setShard(shardId);
    chunk.setVersion(version);
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    NamespaceString::kConfigsvrChunksNamespace,
                                                    chunk.toConfigBSON(),
                                                    kMajorityWriteConcern));
    return chunk;
}

void MigrationTestFixture::setUpZones(const NamespaceString& collName,
                                      const StringMap<ChunkRange>& zoneChunkRanges) {
    for (auto const& zoneChunkRange : zoneChunkRanges) {
        BSONObjBuilder zoneDocBuilder;
        zoneDocBuilder.append("_id",
                              BSON(TagsType::ns(collName.toString_forTest())
                                   << TagsType::min(zoneChunkRange.second.getMin())));
        zoneDocBuilder.append(TagsType::ns(), collName.ns_forTest());
        zoneDocBuilder.append(TagsType::min(), zoneChunkRange.second.getMin());
        zoneDocBuilder.append(TagsType::max(), zoneChunkRange.second.getMax());
        zoneDocBuilder.append(TagsType::tag(), zoneChunkRange.first);

        ASSERT_OK(catalogClient()->insertConfigDocument(
            operationContext(), TagsType::ConfigNS, zoneDocBuilder.obj(), kMajorityWriteConcern));
    }
}

void MigrationTestFixture::removeAllZones(const NamespaceString& collName) {
    const auto query = BSON("ns" << collName.ns_forTest());
    ASSERT_OK(catalogClient()->removeConfigDocuments(
        operationContext(), TagsType::ConfigNS, query, kMajorityWriteConcern));
    auto findStatus = findOneOnConfigCollection(operationContext(), collName, query);
    ASSERT_EQ(ErrorCodes::NoMatchingDocument, findStatus);
}

void MigrationTestFixture::removeAllChunks(const NamespaceString& collName, const UUID& uuid) {
    const auto query = BSON("uuid" << uuid);
    ASSERT_OK(catalogClient()->removeConfigDocuments(operationContext(),
                                                     NamespaceString::kConfigsvrChunksNamespace,
                                                     query,
                                                     kMajorityWriteConcern));
    auto findStatus = findOneOnConfigCollection(operationContext(), collName, query);
    ASSERT_EQ(ErrorCodes::NoMatchingDocument, findStatus);
}

ShardId MigrationTestFixture::getShardIdByHost(HostAndPort host) {
    if (host == kShardHost0) {
        return kShardId0;
    } else if (host == kShardHost1) {
        return kShardId1;
    } else if (host == kShardHost2) {
        return kShardId2;
    } else if (host == kShardHost3) {
        return kShardId3;
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo
