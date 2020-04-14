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
#include "mongo/bson/oid.h"
#include "mongo/s/request_types/set_shard_version_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using unittest::assertGet;

namespace {

const ConnectionString configCS = ConnectionString::forReplicaSet(
    "ConfigRS", {HostAndPort{"configHost1:27017"}, HostAndPort{"configHost2:27017"}});

const ConnectionString shardCS = ConnectionString::forReplicaSet(
    "ShardRS", {HostAndPort{"shardHost1:12345"}, HostAndPort{"shardHost2:12345"}});

TEST(SetShardVersionRequest, ParseFull) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    SetShardVersionRequest request =
        assertGet(SetShardVersionRequest::parseFromBSON(
            BSON("setShardVersion"
                 << "db.coll"
                 << "shard"
                 << "TestShard"
                 << "shardHost" << shardCS.toString() << "version"
                 << Timestamp(chunkVersion.toLong()) << "versionEpoch" << chunkVersion.epoch())));

    ASSERT(!request.shouldForceRefresh());
    ASSERT(!request.isAuthoritative());
    ASSERT_EQ(request.getShardName(), "TestShard");
    ASSERT_EQ(request.getShardConnectionString().toString(), shardCS.toString());
    ASSERT_EQ(request.getNS().toString(), "db.coll");
    ASSERT_EQ(request.getNSVersion().majorVersion(), chunkVersion.majorVersion());
    ASSERT_EQ(request.getNSVersion().minorVersion(), chunkVersion.minorVersion());
    ASSERT_EQ(request.getNSVersion().epoch(), chunkVersion.epoch());
}

TEST(SetShardVersionRequest, ParseFullWithAuthoritative) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    SetShardVersionRequest request =
        assertGet(SetShardVersionRequest::parseFromBSON(
            BSON("setShardVersion"
                 << "db.coll"
                 << "shard"
                 << "TestShard"
                 << "shardHost" << shardCS.toString() << "version"
                 << Timestamp(chunkVersion.toLong()) << "versionEpoch" << chunkVersion.epoch()
                 << "authoritative" << true)));

    ASSERT(!request.shouldForceRefresh());
    ASSERT(request.isAuthoritative());
    ASSERT_EQ(request.getShardName(), "TestShard");
    ASSERT_EQ(request.getShardConnectionString().toString(), shardCS.toString());
    ASSERT_EQ(request.getNS().toString(), "db.coll");
    ASSERT_EQ(request.getNSVersion().majorVersion(), chunkVersion.majorVersion());
    ASSERT_EQ(request.getNSVersion().minorVersion(), chunkVersion.minorVersion());
    ASSERT_EQ(request.getNSVersion().epoch(), chunkVersion.epoch());
}

TEST(SetShardVersionRequest, ParseFullNoConnectionVersioning) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    SetShardVersionRequest request =
        assertGet(SetShardVersionRequest::parseFromBSON(
            BSON("setShardVersion"
                 << "db.coll"
                 << "shard"
                 << "TestShard"
                 << "shardHost" << shardCS.toString() << "version"
                 << Timestamp(chunkVersion.toLong()) << "versionEpoch" << chunkVersion.epoch()
                 << "noConnectionVersioning" << true)));

    ASSERT(!request.shouldForceRefresh());
    ASSERT(!request.isAuthoritative());
    ASSERT_EQ(request.getShardName(), "TestShard");
    ASSERT_EQ(request.getShardConnectionString().toString(), shardCS.toString());
    ASSERT_EQ(request.getNS().toString(), "db.coll");
    ASSERT_EQ(request.getNSVersion().majorVersion(), chunkVersion.majorVersion());
    ASSERT_EQ(request.getNSVersion().minorVersion(), chunkVersion.minorVersion());
    ASSERT_EQ(request.getNSVersion().epoch(), chunkVersion.epoch());
}

TEST(SetShardVersionRequest, ParseFullNoNS) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    auto ssvStatus =
        SetShardVersionRequest::parseFromBSON(BSON("setShardVersion"
                                                   << ""
                                                   << "shard"
                                                   << "TestShard"
                                                   << "shardHost" << shardCS.toString() << "version"
                                                   << Timestamp(chunkVersion.toLong())
                                                   << "versionEpoch" << chunkVersion.epoch()));

    ASSERT_EQ(ErrorCodes::InvalidNamespace, ssvStatus.getStatus().code());
}

TEST(SetShardVersionRequest, ParseFullNSContainsDBOnly) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    auto ssvStatus =
        SetShardVersionRequest::parseFromBSON(BSON("setShardVersion"
                                                   << "dbOnly"
                                                   << "shard"
                                                   << "TestShard"
                                                   << "shardHost" << shardCS.toString() << "version"
                                                   << Timestamp(chunkVersion.toLong())
                                                   << "versionEpoch" << chunkVersion.epoch()));

    ASSERT_EQ(ErrorCodes::InvalidNamespace, ssvStatus.getStatus().code());
}

TEST(SetShardVersionRequest, ToSSVCommandFull) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    SetShardVersionRequest ssv(
        configCS, ShardId("TestShard"), shardCS, NamespaceString("db.coll"), chunkVersion, false);

    ASSERT(!ssv.shouldForceRefresh());
    ASSERT(!ssv.isAuthoritative());
    ASSERT_EQ(ssv.getShardName(), "TestShard");
    ASSERT_EQ(ssv.getShardConnectionString().toString(), shardCS.toString());
    ASSERT_EQ(ssv.getNS().ns(), "db.coll");
    ASSERT_BSONOBJ_EQ(ssv.getNSVersion().toBSON(), chunkVersion.toBSON());

    ASSERT_BSONOBJ_EQ(ssv.toBSON(),
                      BSON("setShardVersion"
                           << "db.coll"
                           << "forceRefresh" << false << "authoritative" << false << "configdb"
                           << configCS.toString() << "shard"
                           << "TestShard"
                           << "shardHost" << shardCS.toString() << "version"
                           << Timestamp(chunkVersion.toLong()) << "versionEpoch"
                           << chunkVersion.epoch()));
}

TEST(SetShardVersionRequest, ToSSVCommandFullAuthoritative) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    SetShardVersionRequest ssv(
        configCS, ShardId("TestShard"), shardCS, NamespaceString("db.coll"), chunkVersion, true);

    ASSERT(!ssv.shouldForceRefresh());
    ASSERT(ssv.isAuthoritative());
    ASSERT_EQ(ssv.getShardName(), "TestShard");
    ASSERT_EQ(ssv.getShardConnectionString().toString(), shardCS.toString());
    ASSERT_EQ(ssv.getNS().ns(), "db.coll");
    ASSERT_BSONOBJ_EQ(ssv.getNSVersion().toBSON(), chunkVersion.toBSON());

    ASSERT_BSONOBJ_EQ(ssv.toBSON(),
                      BSON("setShardVersion"
                           << "db.coll"
                           << "forceRefresh" << false << "authoritative" << true << "configdb"
                           << configCS.toString() << "shard"
                           << "TestShard"
                           << "shardHost" << shardCS.toString() << "version"
                           << Timestamp(chunkVersion.toLong()) << "versionEpoch"
                           << chunkVersion.epoch()));
}

TEST(SetShardVersionRequest, ToSSVCommandFullForceRefresh) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    SetShardVersionRequest ssv(configCS,
                               ShardId("TestShard"),
                               shardCS,
                               NamespaceString("db.coll"),
                               chunkVersion,
                               false,
                               true);

    ASSERT(ssv.shouldForceRefresh());
    ASSERT(!ssv.isAuthoritative());
    ASSERT_EQ(ssv.getShardName(), "TestShard");
    ASSERT_EQ(ssv.getShardConnectionString().toString(), shardCS.toString());
    ASSERT_EQ(ssv.getNS().ns(), "db.coll");
    ASSERT_BSONOBJ_EQ(ssv.getNSVersion().toBSON(), chunkVersion.toBSON());

    ASSERT_BSONOBJ_EQ(ssv.toBSON(),
                      BSON("setShardVersion"
                           << "db.coll"
                           << "forceRefresh" << true << "authoritative" << false << "configdb"
                           << configCS.toString() << "shard"
                           << "TestShard"
                           << "shardHost" << shardCS.toString() << "version"
                           << Timestamp(chunkVersion.toLong()) << "versionEpoch"
                           << chunkVersion.epoch()));
}

}  // namespace
}  // namespace mongo
