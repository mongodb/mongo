/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/s/set_shard_version_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using unittest::assertGet;

namespace {

const ConnectionString configCS = ConnectionString::forReplicaSet(
    "ConfigRS", {HostAndPort{"configHost1:27017"}, HostAndPort{"configHost2:27017"}});

const ConnectionString shardCS = ConnectionString::forReplicaSet(
    "ShardRS", {HostAndPort{"shardHost1:12345"}, HostAndPort{"shardHost2:12345"}});

TEST(SetShardVersionRequest, ParseInitMissingAuthoritative) {
    SetShardVersionRequest request =
        assertGet(SetShardVersionRequest::parseFromBSON(BSON("setShardVersion"
                                                             << ""
                                                             << "init"
                                                             << true
                                                             << "configdb"
                                                             << configCS.toString()
                                                             << "shard"
                                                             << "TestShard"
                                                             << "shardHost"
                                                             << shardCS.toString())));

    ASSERT(request.isInit());
    ASSERT(!request.isAuthoritative());
    ASSERT(!request.getNoConnectionVersioning());
    ASSERT_EQ(request.getConfigServer().toString(), configCS.toString());
    ASSERT_EQ(request.getShardName(), "TestShard");
    ASSERT_EQ(request.getShardConnectionString().toString(), shardCS.toString());
}

TEST(SetShardVersionRequest, ParseInitWithAuthoritative) {
    SetShardVersionRequest request =
        assertGet(SetShardVersionRequest::parseFromBSON(BSON("setShardVersion"
                                                             << ""
                                                             << "init"
                                                             << true
                                                             << "authoritative"
                                                             << true
                                                             << "configdb"
                                                             << configCS.toString()
                                                             << "shard"
                                                             << "TestShard"
                                                             << "shardHost"
                                                             << shardCS.toString())));

    ASSERT(request.isInit());
    ASSERT(request.isAuthoritative());
    ASSERT(!request.getNoConnectionVersioning());
    ASSERT_EQ(request.getConfigServer().toString(), configCS.toString());
    ASSERT_EQ(request.getShardName(), "TestShard");
    ASSERT_EQ(request.getShardConnectionString().toString(), shardCS.toString());
}

TEST(SetShardVersionRequest, ParseInitNoConnectionVersioning) {
    SetShardVersionRequest request =
        assertGet(SetShardVersionRequest::parseFromBSON(BSON("setShardVersion"
                                                             << ""
                                                             << "init"
                                                             << true
                                                             << "authoritative"
                                                             << true
                                                             << "configdb"
                                                             << configCS.toString()
                                                             << "shard"
                                                             << "TestShard"
                                                             << "shardHost"
                                                             << shardCS.toString()
                                                             << "noConnectionVersioning"
                                                             << true)));

    ASSERT(request.isInit());
    ASSERT(request.isAuthoritative());
    ASSERT(request.getNoConnectionVersioning());
    ASSERT_EQ(request.getConfigServer().toString(), configCS.toString());
    ASSERT_EQ(request.getShardName(), "TestShard");
    ASSERT_EQ(request.getShardConnectionString().toString(), shardCS.toString());
}

TEST(SetShardVersionRequest, ParseFull) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    SetShardVersionRequest request =
        assertGet(SetShardVersionRequest::parseFromBSON(BSON("setShardVersion"
                                                             << "db.coll"
                                                             << "configdb"
                                                             << configCS.toString()
                                                             << "shard"
                                                             << "TestShard"
                                                             << "shardHost"
                                                             << shardCS.toString()
                                                             << "version"
                                                             << Timestamp(chunkVersion.toLong())
                                                             << "versionEpoch"
                                                             << chunkVersion.epoch())));

    ASSERT(!request.isInit());
    ASSERT(!request.isAuthoritative());
    ASSERT(!request.getNoConnectionVersioning());
    ASSERT_EQ(request.getConfigServer().toString(), configCS.toString());
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
        assertGet(SetShardVersionRequest::parseFromBSON(BSON("setShardVersion"
                                                             << "db.coll"
                                                             << "configdb"
                                                             << configCS.toString()
                                                             << "shard"
                                                             << "TestShard"
                                                             << "shardHost"
                                                             << shardCS.toString()
                                                             << "version"
                                                             << Timestamp(chunkVersion.toLong())
                                                             << "versionEpoch"
                                                             << chunkVersion.epoch()
                                                             << "authoritative"
                                                             << true)));

    ASSERT(!request.isInit());
    ASSERT(request.isAuthoritative());
    ASSERT(!request.getNoConnectionVersioning());
    ASSERT_EQ(request.getConfigServer().toString(), configCS.toString());
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
        assertGet(SetShardVersionRequest::parseFromBSON(BSON("setShardVersion"
                                                             << "db.coll"
                                                             << "configdb"
                                                             << configCS.toString()
                                                             << "shard"
                                                             << "TestShard"
                                                             << "shardHost"
                                                             << shardCS.toString()
                                                             << "version"
                                                             << Timestamp(chunkVersion.toLong())
                                                             << "versionEpoch"
                                                             << chunkVersion.epoch()
                                                             << "noConnectionVersioning"
                                                             << true)));

    ASSERT(!request.isInit());
    ASSERT(!request.isAuthoritative());
    ASSERT(request.getNoConnectionVersioning());
    ASSERT_EQ(request.getConfigServer().toString(), configCS.toString());
    ASSERT_EQ(request.getShardName(), "TestShard");
    ASSERT_EQ(request.getShardConnectionString().toString(), shardCS.toString());
    ASSERT_EQ(request.getNS().toString(), "db.coll");
    ASSERT_EQ(request.getNSVersion().majorVersion(), chunkVersion.majorVersion());
    ASSERT_EQ(request.getNSVersion().minorVersion(), chunkVersion.minorVersion());
    ASSERT_EQ(request.getNSVersion().epoch(), chunkVersion.epoch());
}

TEST(SetShardVersionRequest, ParseInitNoConfigServer) {
    auto ssvStatus = SetShardVersionRequest::parseFromBSON(BSON("setShardVersion"
                                                                << ""
                                                                << "init"
                                                                << true
                                                                << "shard"
                                                                << "TestShard"
                                                                << "shardHost"
                                                                << shardCS.toString()));

    ASSERT_EQ(ErrorCodes::NoSuchKey, ssvStatus.getStatus().code());
}

TEST(SetShardVersionRequest, ParseFullNoNS) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    auto ssvStatus = SetShardVersionRequest::parseFromBSON(BSON("setShardVersion"
                                                                << ""
                                                                << "configdb"
                                                                << configCS.toString()
                                                                << "shard"
                                                                << "TestShard"
                                                                << "shardHost"
                                                                << shardCS.toString()
                                                                << "version"
                                                                << Timestamp(chunkVersion.toLong())
                                                                << "versionEpoch"
                                                                << chunkVersion.epoch()));

    ASSERT_EQ(ErrorCodes::InvalidNamespace, ssvStatus.getStatus().code());
}

TEST(SetShardVersionRequest, ParseFullNSContainsDBOnly) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    auto ssvStatus = SetShardVersionRequest::parseFromBSON(BSON("setShardVersion"
                                                                << "dbOnly"
                                                                << "configdb"
                                                                << configCS.toString()
                                                                << "shard"
                                                                << "TestShard"
                                                                << "shardHost"
                                                                << shardCS.toString()
                                                                << "version"
                                                                << Timestamp(chunkVersion.toLong())
                                                                << "versionEpoch"
                                                                << chunkVersion.epoch()));

    ASSERT_EQ(ErrorCodes::InvalidNamespace, ssvStatus.getStatus().code());
}

TEST(SetShardVersionRequest, ToSSVCommandInit) {
    SetShardVersionRequest ssv =
        SetShardVersionRequest::makeForInit(configCS, ShardId("TestShard"), shardCS);

    ASSERT(ssv.isInit());
    ASSERT(ssv.isAuthoritative());
    ASSERT(!ssv.getNoConnectionVersioning());
    ASSERT_EQ(ssv.getConfigServer().toString(), configCS.toString());
    ASSERT_EQ(ssv.getShardName(), "TestShard");
    ASSERT_EQ(ssv.getShardConnectionString().toString(), shardCS.toString());

    ASSERT_EQ(ssv.toBSON(),
              BSON("setShardVersion"
                   << ""
                   << "init"
                   << true
                   << "authoritative"
                   << true
                   << "configdb"
                   << configCS.toString()
                   << "shard"
                   << "TestShard"
                   << "shardHost"
                   << shardCS.toString()
                   << "maxTimeMS"
                   << 30000));
}

TEST(SetShardVersionRequest, ToSSVCommandInitNoConnectionVersioning) {
    SetShardVersionRequest ssv =
        SetShardVersionRequest::makeForInitNoPersist(configCS, ShardId("TestShard"), shardCS);

    ASSERT(ssv.isInit());
    ASSERT(ssv.isAuthoritative());
    ASSERT(ssv.getNoConnectionVersioning());
    ASSERT_EQ(ssv.getConfigServer().toString(), configCS.toString());
    ASSERT_EQ(ssv.getShardName(), "TestShard");
    ASSERT_EQ(ssv.getShardConnectionString().toString(), shardCS.toString());

    ASSERT_EQ(ssv.toBSON(),
              BSON("setShardVersion"
                   << ""
                   << "init"
                   << true
                   << "authoritative"
                   << true
                   << "configdb"
                   << configCS.toString()
                   << "shard"
                   << "TestShard"
                   << "shardHost"
                   << shardCS.toString()
                   << "maxTimeMS"
                   << 30000
                   << "noConnectionVersioning"
                   << true));
}

TEST(SetShardVersionRequest, ToSSVCommandFull) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    SetShardVersionRequest ssv = SetShardVersionRequest::makeForVersioning(
        configCS, ShardId("TestShard"), shardCS, NamespaceString("db.coll"), chunkVersion, false);

    ASSERT(!ssv.isInit());
    ASSERT(!ssv.isAuthoritative());
    ASSERT(!ssv.getNoConnectionVersioning());
    ASSERT_EQ(ssv.getConfigServer().toString(), configCS.toString());
    ASSERT_EQ(ssv.getShardName(), "TestShard");
    ASSERT_EQ(ssv.getShardConnectionString().toString(), shardCS.toString());
    ASSERT_EQ(ssv.getNS().ns(), "db.coll");
    ASSERT_EQ(ssv.getNSVersion().toBSONWithPrefix("version"),
              chunkVersion.toBSONWithPrefix("version"));

    ASSERT_EQ(ssv.toBSON(),
              BSON("setShardVersion"
                   << "db.coll"
                   << "init"
                   << false
                   << "authoritative"
                   << false
                   << "configdb"
                   << configCS.toString()
                   << "shard"
                   << "TestShard"
                   << "shardHost"
                   << shardCS.toString()
                   << "version"
                   << Timestamp(chunkVersion.toLong())
                   << "versionEpoch"
                   << chunkVersion.epoch()));
}

TEST(SetShardVersionRequest, ToSSVCommandFullAuthoritative) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    SetShardVersionRequest ssv = SetShardVersionRequest::makeForVersioning(
        configCS, ShardId("TestShard"), shardCS, NamespaceString("db.coll"), chunkVersion, true);

    ASSERT(!ssv.isInit());
    ASSERT(ssv.isAuthoritative());
    ASSERT(!ssv.getNoConnectionVersioning());
    ASSERT_EQ(ssv.getConfigServer().toString(), configCS.toString());
    ASSERT_EQ(ssv.getShardName(), "TestShard");
    ASSERT_EQ(ssv.getShardConnectionString().toString(), shardCS.toString());
    ASSERT_EQ(ssv.getNS().ns(), "db.coll");
    ASSERT_EQ(ssv.getNSVersion().toBSONWithPrefix("version"),
              chunkVersion.toBSONWithPrefix("version"));

    ASSERT_EQ(ssv.toBSON(),
              BSON("setShardVersion"
                   << "db.coll"
                   << "init"
                   << false
                   << "authoritative"
                   << true
                   << "configdb"
                   << configCS.toString()
                   << "shard"
                   << "TestShard"
                   << "shardHost"
                   << shardCS.toString()
                   << "version"
                   << Timestamp(chunkVersion.toLong())
                   << "versionEpoch"
                   << chunkVersion.epoch()));
}

TEST(SetShardVersionRequest, ToSSVCommandFullNoConnectionVersioning) {
    const ChunkVersion chunkVersion(1, 2, OID::gen());

    SetShardVersionRequest ssv = SetShardVersionRequest::makeForVersioningNoPersist(
        configCS, ShardId("TestShard"), shardCS, NamespaceString("db.coll"), chunkVersion, true);

    ASSERT(!ssv.isInit());
    ASSERT(ssv.isAuthoritative());
    ASSERT(ssv.getNoConnectionVersioning());
    ASSERT_EQ(ssv.getConfigServer().toString(), configCS.toString());
    ASSERT_EQ(ssv.getShardName(), "TestShard");
    ASSERT_EQ(ssv.getShardConnectionString().toString(), shardCS.toString());
    ASSERT_EQ(ssv.getNS().ns(), "db.coll");
    ASSERT_EQ(ssv.getNSVersion().toBSONWithPrefix("version"),
              chunkVersion.toBSONWithPrefix("version"));

    ASSERT_EQ(ssv.toBSON(),
              BSON("setShardVersion"
                   << "db.coll"
                   << "init"
                   << false
                   << "authoritative"
                   << true
                   << "configdb"
                   << configCS.toString()
                   << "shard"
                   << "TestShard"
                   << "shardHost"
                   << shardCS.toString()
                   << "version"
                   << Timestamp(chunkVersion.toLong())
                   << "versionEpoch"
                   << chunkVersion.epoch()
                   << "noConnectionVersioning"
                   << true));
}

}  // namespace
}  // namespace mongo
