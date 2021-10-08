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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/request_types/move_chunk_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using unittest::assertGet;

namespace {

const NamespaceString kNs("TestDB.TestColl");
const BSONObj kMin = BSON("Key" << -100);
const BSONObj kMax = BSON("Key" << 100);
const ShardId kFromShard("shard0001");
const ShardId kToShard("shard0002");
const int kMaxChunkSizeBytes = 1024;
const bool kWaitForDelete = true;

TEST(MoveChunkRequest, Roundtrip) {
    const ChunkVersion chunkVersion(3, 1, OID::gen(), Timestamp(1, 1));

    BSONObjBuilder builder;
    MoveChunkRequest::appendAsCommand(
        &builder,
        kNs,
        chunkVersion,
        kFromShard,
        kToShard,
        ChunkRange(kMin, kMax),
        kMaxChunkSizeBytes,
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        kWaitForDelete,
        MoveChunkRequest::ForceJumbo::kDoNotForce);

    BSONObj cmdObj = builder.obj();

    auto request = assertGet(
        MoveChunkRequest::createFromCommand(NamespaceString(cmdObj["moveChunk"].String()), cmdObj));
    ASSERT_EQ(kNs.ns(), request.getNss().ns());
    ASSERT_EQ(kFromShard, request.getFromShardId());
    ASSERT_EQ(kToShard, request.getToShardId());
    ASSERT_BSONOBJ_EQ(kMin, request.getMinKey());
    ASSERT_BSONOBJ_EQ(kMax, request.getMaxKey());
    ASSERT_EQ(chunkVersion.epoch(), request.getVersionEpoch());
    ASSERT_EQ(kMaxChunkSizeBytes, request.getMaxChunkSizeBytes());
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOff,
              request.getSecondaryThrottle().getSecondaryThrottle());
    ASSERT_EQ(kWaitForDelete, request.getWaitForDelete());
}

TEST(MoveChunkRequest, EqualityOperatorSameValue) {
    const ChunkVersion chunkVersion(3, 1, OID::gen(), Timestamp(1, 1));

    BSONObjBuilder builder;
    MoveChunkRequest::appendAsCommand(
        &builder,
        kNs,
        chunkVersion,
        kFromShard,
        kToShard,
        ChunkRange(kMin, kMax),
        kMaxChunkSizeBytes,
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        kWaitForDelete,
        MoveChunkRequest::ForceJumbo::kDoNotForce);

    BSONObj obj = builder.obj();

    auto value1 = assertGet(MoveChunkRequest::createFromCommand(kNs, obj));
    auto value2 = assertGet(MoveChunkRequest::createFromCommand(kNs, obj));

    ASSERT(value1 == value2);
    ASSERT_FALSE(value1 != value2);
}

TEST(MoveChunkRequest, EqualityOperatorDifferentValues) {
    const ChunkVersion chunkVersion(3, 1, OID::gen(), Timestamp(1, 1));

    BSONObjBuilder builder1;
    MoveChunkRequest::appendAsCommand(
        &builder1,
        kNs,
        chunkVersion,
        kFromShard,
        kToShard,
        ChunkRange(kMin, kMax),
        kMaxChunkSizeBytes,
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        kWaitForDelete,
        MoveChunkRequest::ForceJumbo::kDoNotForce);

    auto value1 = assertGet(MoveChunkRequest::createFromCommand(kNs, builder1.obj()));

    BSONObjBuilder builder2;
    MoveChunkRequest::appendAsCommand(
        &builder2,
        kNs,
        chunkVersion,
        kFromShard,
        kToShard,
        ChunkRange(BSON("Key" << 100), BSON("Key" << 200)),  // Different key ranges
        kMaxChunkSizeBytes,
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        kWaitForDelete,
        MoveChunkRequest::ForceJumbo::kDoNotForce);
    auto value2 = assertGet(MoveChunkRequest::createFromCommand(kNs, builder2.obj()));

    ASSERT_FALSE(value1 == value2);
    ASSERT(value1 != value2);
}

}  // namespace
}  // namespace mongo
