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

#include "mongo/db/jsobj.h"
#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

const std::string kNs = "TestDB.TestColl";
const BSONObj kMin = BSON("a" << 10);
const BSONObj kMax = BSON("a" << 20);
const ShardId kFromShard("shard0000");
const ShardId kToShard("shard0001");
const bool kWaitForDelete{true};

TEST(MigrationTypeTest, FromAndToBSONWithoutOptionalFields) {
    const ChunkVersion version({OID::gen(), Timestamp(1, 1)}, {1, 2});

    BSONObjBuilder builder;
    builder.append(MigrationType::ns(), kNs);
    builder.append(MigrationType::min(), kMin);
    builder.append(MigrationType::max(), kMax);
    builder.append(MigrationType::fromShard(), kFromShard.toString());
    builder.append(MigrationType::toShard(), kToShard.toString());
    version.serializeToBSON("chunkVersion", &builder);
    builder.append(MigrationType::waitForDelete(), kWaitForDelete);
    builder.append(MigrationType::forceJumbo(), ForceJumbo_serializer(ForceJumbo::kDoNotForce));

    BSONObj obj = builder.obj();

    MigrationType migrationType = assertGet(MigrationType::fromBSON(obj));
    ASSERT_BSONOBJ_EQ(obj, migrationType.toBSON());
}

TEST(MigrationTypeTest, FromAndToBSONWitOptionalFields) {
    const ChunkVersion version({OID::gen(), Timestamp(1, 1)}, {1, 2});
    const auto secondaryThrottle =
        MigrationSecondaryThrottleOptions::createWithWriteConcern(WriteConcernOptions(
            "majority", WriteConcernOptions::SyncMode::JOURNAL, Milliseconds(60000)));

    BSONObjBuilder builder;
    builder.append(MigrationType::ns(), kNs);
    builder.append(MigrationType::min(), kMin);
    builder.append(MigrationType::max(), kMax);
    builder.append(MigrationType::fromShard(), kFromShard.toString());
    builder.append(MigrationType::toShard(), kToShard.toString());
    version.serializeToBSON("chunkVersion", &builder);
    builder.append(MigrationType::waitForDelete(), kWaitForDelete);
    builder.append(MigrationType::forceJumbo(), ForceJumbo_serializer(ForceJumbo::kDoNotForce));
    builder.append(MigrationType::maxChunkSizeBytes(), 512 * 1024 * 1024);
    secondaryThrottle.append(&builder);


    BSONObj obj = builder.obj();

    MigrationType migrationType = assertGet(MigrationType::fromBSON(obj));
    ASSERT_BSONOBJ_EQ(obj, migrationType.toBSON());
}

TEST(MigrationTypeTest, MissingRequiredNamespaceField) {
    const ChunkVersion version({OID::gen(), Timestamp(1, 1)}, {1, 2});

    BSONObjBuilder builder;
    builder.append(MigrationType::min(), kMin);
    builder.append(MigrationType::max(), kMax);
    builder.append(MigrationType::fromShard(), kFromShard.toString());
    builder.append(MigrationType::toShard(), kToShard.toString());
    version.serializeToBSON("chunkVersion", &builder);

    BSONObj obj = builder.obj();

    StatusWith<MigrationType> migrationType = MigrationType::fromBSON(obj);
    ASSERT_EQUALS(migrationType.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(migrationType.getStatus().reason(), MigrationType::ns.name());
}

TEST(MigrationTypeTest, MissingRequiredMinField) {
    const ChunkVersion version({OID::gen(), Timestamp(1, 1)}, {1, 2});

    BSONObjBuilder builder;
    builder.append(MigrationType::ns(), kNs);
    builder.append(MigrationType::max(), kMax);
    builder.append(MigrationType::fromShard(), kFromShard.toString());
    builder.append(MigrationType::toShard(), kToShard.toString());
    version.serializeToBSON("chunkVersion", &builder);

    BSONObj obj = builder.obj();

    StatusWith<MigrationType> migrationType = MigrationType::fromBSON(obj);
    ASSERT_EQUALS(migrationType.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(migrationType.getStatus().reason(), MigrationType::min.name());
}

TEST(MigrationTypeTest, MissingRequiredMaxField) {
    const ChunkVersion version({OID::gen(), Timestamp(1, 1)}, {1, 2});

    BSONObjBuilder builder;
    builder.append(MigrationType::ns(), kNs);
    builder.append(MigrationType::min(), kMin);
    builder.append(MigrationType::fromShard(), kFromShard.toString());
    builder.append(MigrationType::toShard(), kToShard.toString());
    version.serializeToBSON("chunkVersion", &builder);

    BSONObj obj = builder.obj();

    StatusWith<MigrationType> migrationType = MigrationType::fromBSON(obj);
    ASSERT_EQUALS(migrationType.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(migrationType.getStatus().reason(), MigrationType::max.name());
}

TEST(MigrationTypeTest, MissingRequiredFromShardField) {
    const ChunkVersion version({OID::gen(), Timestamp(1, 1)}, {1, 2});

    BSONObjBuilder builder;
    builder.append(MigrationType::ns(), kNs);
    builder.append(MigrationType::min(), kMin);
    builder.append(MigrationType::max(), kMax);
    builder.append(MigrationType::toShard(), kToShard.toString());
    version.serializeToBSON("chunkVersion", &builder);

    BSONObj obj = builder.obj();

    StatusWith<MigrationType> migrationType = MigrationType::fromBSON(obj);
    ASSERT_EQUALS(migrationType.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(migrationType.getStatus().reason(), MigrationType::fromShard.name());
}

TEST(MigrationTypeTest, MissingRequiredToShardField) {
    const ChunkVersion version({OID::gen(), Timestamp(1, 1)}, {1, 2});

    BSONObjBuilder builder;
    builder.append(MigrationType::ns(), kNs);
    builder.append(MigrationType::min(), kMin);
    builder.append(MigrationType::max(), kMax);
    builder.append(MigrationType::fromShard(), kFromShard.toString());
    version.serializeToBSON("chunkVersion", &builder);

    BSONObj obj = builder.obj();

    StatusWith<MigrationType> migrationType = MigrationType::fromBSON(obj);
    ASSERT_EQUALS(migrationType.getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_STRING_CONTAINS(migrationType.getStatus().reason(), MigrationType::toShard.name());
}

TEST(MigrationTypeTest, MissingRequiredVersionField) {
    BSONObjBuilder builder;
    builder.append(MigrationType::ns(), kNs);
    builder.append(MigrationType::min(), kMin);
    builder.append(MigrationType::max(), kMax);
    builder.append(MigrationType::fromShard(), kFromShard.toString());
    builder.append(MigrationType::toShard(), kToShard.toString());

    BSONObj obj = builder.obj();

    ASSERT_THROWS(uassertStatusOK(MigrationType::fromBSON(obj)), DBException);
}

}  // namespace
}  // namespace mongo
