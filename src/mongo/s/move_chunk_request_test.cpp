/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using unittest::assertGet;

namespace {

const ConnectionString kTestConnectionString =
    assertGet(ConnectionString::parse("TestConfigRS/CS1:12345,CS2:12345,CS3:12345"));

TEST(MoveChunkRequest, Roundtrip) {
    const ChunkVersion collectionVersion(3, 1, OID::gen());
    const ChunkVersion chunkVersion(2, 3, OID::gen());

    BSONObjBuilder builder;
    MoveChunkRequest::appendAsCommand(
        &builder,
        NamespaceString("TestDB", "TestColl"),
        collectionVersion,
        kTestConnectionString,
        ShardId("shard0001"),
        ShardId("shard0002"),
        ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
        chunkVersion,
        1024,
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        true,
        true);

    BSONObj cmdObj = builder.obj();

    auto request = assertGet(
        MoveChunkRequest::createFromCommand(NamespaceString(cmdObj["moveChunk"].String()), cmdObj));
    ASSERT_EQ("TestDB.TestColl", request.getNss().ns());
    ASSERT_EQ(kTestConnectionString.toString(), request.getConfigServerCS().toString());
    ASSERT_EQ(ShardId("shard0001"), request.getFromShardId());
    ASSERT_EQ(ShardId("shard0002"), request.getToShardId());
    ASSERT_BSONOBJ_EQ(BSON("Key" << -100), request.getMinKey());
    ASSERT_BSONOBJ_EQ(BSON("Key" << 100), request.getMaxKey());
    ASSERT(request.hasChunkVersion());
    ASSERT_EQ(chunkVersion, request.getChunkVersion());
    ASSERT_EQ(1024, request.getMaxChunkSizeBytes());
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOff,
              request.getSecondaryThrottle().getSecondaryThrottle());
    ASSERT_EQ(true, request.getWaitForDelete());
    ASSERT_EQ(true, request.getTakeDistLock());
}

TEST(MoveChunkRequest, BackwardsCompatibilityNoChunkVersionAndDefaults) {
    const ChunkVersion collectionVersion(3, 1, OID::gen());

    auto request =
        assertGet(MoveChunkRequest::createFromCommand(NamespaceString("TestDB", "TestColl"),
                                                      BSON("moveChunk"
                                                           << "TestDB.TestColl"
                                                           << "shardVersion"
                                                           << collectionVersion.toBSON()
                                                           << "configdb"
                                                           << kTestConnectionString.toString()
                                                           << "fromShard"
                                                           << "shard0001"
                                                           << "toShard"
                                                           << "shard0002"
                                                           << "min"
                                                           << BSON("Key" << -1)
                                                           << "max"
                                                           << BSON("Key" << 1)
                                                           // Omit the chunkVersion
                                                           << "maxChunkSizeBytes"
                                                           << 1024)));

    ASSERT_EQ("TestDB.TestColl", request.getNss().ns());
    ASSERT_EQ(kTestConnectionString.toString(), request.getConfigServerCS().toString());
    ASSERT_EQ(ShardId("shard0001"), request.getFromShardId());
    ASSERT_EQ(ShardId("shard0002"), request.getToShardId());
    ASSERT_BSONOBJ_EQ(BSON("Key" << -1), request.getMinKey());
    ASSERT_BSONOBJ_EQ(BSON("Key" << 1), request.getMaxKey());
    ASSERT(!request.hasChunkVersion());
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kDefault,
              request.getSecondaryThrottle().getSecondaryThrottle());
    ASSERT_EQ(false, request.getWaitForDelete());
    ASSERT_EQ(true, request.getTakeDistLock());
}

TEST(MoveChunkRequest, EqualityOperatorSameValue) {
    const ChunkVersion collectionVersion(3, 1, OID::gen());
    const ChunkVersion chunkVersion(2, 3, OID::gen());

    BSONObjBuilder builder;
    MoveChunkRequest::appendAsCommand(
        &builder,
        NamespaceString("TestDB", "TestColl"),
        collectionVersion,
        assertGet(ConnectionString::parse("TestConfigRS/CS1:12345,CS2:12345,CS3:12345")),
        ShardId("shard0001"),
        ShardId("shard0002"),
        ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
        chunkVersion,
        1024,
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        true,
        true);

    BSONObj obj = builder.obj();

    auto value1 =
        assertGet(MoveChunkRequest::createFromCommand(NamespaceString("TestDB", "TestColl"), obj));
    auto value2 =
        assertGet(MoveChunkRequest::createFromCommand(NamespaceString("TestDB", "TestColl"), obj));

    ASSERT(value1 == value2);
    ASSERT_FALSE(value1 != value2);
}

TEST(MoveChunkRequest, EqualityOperatorDifferentValues) {
    const ChunkVersion collectionVersion(3, 1, OID::gen());
    const ChunkVersion chunkVersion(2, 3, OID::gen());

    BSONObjBuilder builder1;
    MoveChunkRequest::appendAsCommand(
        &builder1,
        NamespaceString("TestDB", "TestColl"),
        collectionVersion,
        assertGet(ConnectionString::parse("TestConfigRS/CS1:12345,CS2:12345,CS3:12345")),
        ShardId("shard0001"),
        ShardId("shard0002"),
        ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
        chunkVersion,
        1024,
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        true,
        true);

    auto value1 = assertGet(
        MoveChunkRequest::createFromCommand(NamespaceString("TestDB", "TestColl"), builder1.obj()));

    BSONObjBuilder builder2;
    MoveChunkRequest::appendAsCommand(
        &builder2,
        NamespaceString("TestDB", "TestColl"),
        collectionVersion,
        assertGet(ConnectionString::parse("TestConfigRS/CS1:12345,CS2:12345,CS3:12345")),
        ShardId("shard0001"),
        ShardId("shard0002"),
        ChunkRange(BSON("Key" << 100), BSON("Key" << 200)),  // Different key ranges
        chunkVersion,
        1024,
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        true,
        true);
    auto value2 = assertGet(
        MoveChunkRequest::createFromCommand(NamespaceString("TestDB", "TestColl"), builder2.obj()));

    ASSERT_FALSE(value1 == value2);
    ASSERT(value1 != value2);
}

}  // namespace
}  // namespace mongo
