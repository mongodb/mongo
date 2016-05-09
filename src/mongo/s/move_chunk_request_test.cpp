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
#include "mongo/s/move_chunk_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using unittest::assertGet;

namespace {

TEST(MoveChunkRequest, CreateAsCommandComplete) {
    BSONObjBuilder builder;
    MoveChunkRequest::appendAsCommand(
        &builder,
        NamespaceString("TestDB.TestColl"),
        ChunkVersion(2, 3, OID::gen()),
        assertGet(ConnectionString::parse("TestConfigRS/CS1:12345,CS2:12345,CS3:12345")),
        "shard0001",
        "shard0002",
        ChunkRange(BSON("Key" << -100), BSON("Key" << 100)),
        1024,
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        true);

    BSONObj cmdObj = builder.obj();

    auto request = assertGet(
        MoveChunkRequest::createFromCommand(NamespaceString(cmdObj["moveChunk"].String()), cmdObj));
    ASSERT_EQ("TestDB.TestColl", request.getNss().ns());
    ASSERT_EQ("TestConfigRS/CS1:12345,CS2:12345,CS3:12345", request.getConfigServerCS().toString());
    ASSERT_EQ("shard0001", request.getFromShardId());
    ASSERT_EQ("shard0002", request.getToShardId());
    ASSERT_EQ(BSON("Key" << -100), request.getMinKey());
    ASSERT_EQ(BSON("Key" << 100), request.getMaxKey());
    ASSERT_EQ(1024, request.getMaxChunkSizeBytes());
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOff,
              request.getSecondaryThrottle().getSecondaryThrottle());
    ASSERT_EQ(true, request.getWaitForDelete());
}

}  // namespace
}  // namespace mongo
