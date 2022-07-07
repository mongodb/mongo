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

#include "mongo/db/s/balancer/migration_test_fixture.h"

#include "mongo/db/s/type_locks.h"

namespace mongo {

using unittest::assertGet;

void MigrationTestFixture::setUp() {
    setUpAndInitializeConfigDb();
}

std::shared_ptr<RemoteCommandTargeterMock> MigrationTestFixture::shardTargeterMock(
    OperationContext* opCtx, ShardId shardId) {
    return RemoteCommandTargeterMock::get(
        uassertStatusOK(shardRegistry()->getShard(opCtx, shardId))->getTargeter());
}

void MigrationTestFixture::setUpDatabase(const std::string& dbName, const ShardId primaryShard) {
    DatabaseType db(dbName, primaryShard, DatabaseVersion(UUID::gen(), Timestamp()));
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    NamespaceString::kConfigDatabasesNamespace,
                                                    db.toBSON(),
                                                    kMajorityWriteConcern));
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

ChunkType MigrationTestFixture::setUpChunk(const UUID& collUUID,
                                           const BSONObj& chunkMin,
                                           const BSONObj& chunkMax,
                                           const ShardId& shardId,
                                           const ChunkVersion& version) {
    ChunkType chunk;
    chunk.setCollectionUUID(collUUID);

    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);
    chunk.setShard(shardId);
    chunk.setVersion(version);
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ChunkType::ConfigNS, chunk.toConfigBSON(), kMajorityWriteConcern));
    return chunk;
}

void MigrationTestFixture::setUpZones(const NamespaceString& collName,
                                      const StringMap<ChunkRange>& zoneChunkRanges) {
    for (auto const& zoneChunkRange : zoneChunkRanges) {
        BSONObjBuilder zoneDocBuilder;
        zoneDocBuilder.append(
            "_id",
            BSON(TagsType::ns(collName.ns()) << TagsType::min(zoneChunkRange.second.getMin())));
        zoneDocBuilder.append(TagsType::ns(), collName.ns());
        zoneDocBuilder.append(TagsType::min(), zoneChunkRange.second.getMin());
        zoneDocBuilder.append(TagsType::max(), zoneChunkRange.second.getMax());
        zoneDocBuilder.append(TagsType::tag(), zoneChunkRange.first);

        ASSERT_OK(catalogClient()->insertConfigDocument(
            operationContext(), TagsType::ConfigNS, zoneDocBuilder.obj(), kMajorityWriteConcern));
    }
}

void MigrationTestFixture::removeAllZones(const NamespaceString& collName) {
    const auto query = BSON("ns" << collName.ns());
    ASSERT_OK(catalogClient()->removeConfigDocuments(
        operationContext(), TagsType::ConfigNS, query, kMajorityWriteConcern));
    auto findStatus = findOneOnConfigCollection(operationContext(), collName, query);
    ASSERT_EQ(ErrorCodes::NoMatchingDocument, findStatus);
}

void MigrationTestFixture::removeAllChunks(const NamespaceString& collName, const UUID& uuid) {
    const auto query = BSON("uuid" << uuid);
    ASSERT_OK(catalogClient()->removeConfigDocuments(
        operationContext(), ChunkType::ConfigNS, query, kMajorityWriteConcern));
    auto findStatus = findOneOnConfigCollection(operationContext(), collName, query);
    ASSERT_EQ(ErrorCodes::NoMatchingDocument, findStatus);
}

void MigrationTestFixture::setUpMigration(const NamespaceString& ns,
                                          const ChunkType& chunk,
                                          const ShardId& toShard) {
    BSONObjBuilder builder;
    builder.append(MigrationType::ns(), ns.ns());
    builder.append(MigrationType::min(), chunk.getMin());
    builder.append(MigrationType::max(), chunk.getMax());
    builder.append(MigrationType::toShard(), toShard.toString());
    builder.append(MigrationType::fromShard(), chunk.getShard().toString());
    chunk.getVersion().serializeToBSON("chunkVersion", &builder);
    builder.append(MigrationType::forceJumbo(), "doNotForceJumbo");

    MigrationType migrationType = assertGet(MigrationType::fromBSON(builder.obj()));
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    MigrationType::ConfigNS,
                                                    migrationType.toBSON(),
                                                    kMajorityWriteConcern));
}

void MigrationTestFixture::checkMigrationsCollectionIsEmptyAndLocksAreUnlocked() {
    auto statusWithMigrationsQueryResponse =
        shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            operationContext(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kMajorityReadConcern,
            MigrationType::ConfigNS,
            BSONObj(),
            BSONObj(),
            boost::none);
    Shard::QueryResponse migrationsQueryResponse =
        uassertStatusOK(statusWithMigrationsQueryResponse);
    ASSERT_EQUALS(0U, migrationsQueryResponse.docs.size());

    auto statusWithLocksQueryResponse = shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        operationContext(),
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kMajorityReadConcern,
        LocksType::ConfigNS,
        BSON(LocksType::state(LocksType::LOCKED) << LocksType::name("{ '$ne' : 'balancer'}")),
        BSONObj(),
        boost::none);
    Shard::QueryResponse locksQueryResponse = uassertStatusOK(statusWithLocksQueryResponse);
    ASSERT_EQUALS(0U, locksQueryResponse.docs.size());
}

}  // namespace mongo
