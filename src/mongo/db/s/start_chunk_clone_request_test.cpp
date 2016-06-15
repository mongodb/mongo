/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/s/start_chunk_clone_request.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/shard_id.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using unittest::assertGet;

namespace {

TEST(StartChunkCloneRequest, CreateAsCommandComplete) {
    MigrationSessionId sessionId = MigrationSessionId::generate("shard0001", "shard0002");

    BSONObjBuilder builder;
    StartChunkCloneRequest::appendAsCommand(
        &builder,
        NamespaceString("TestDB.TestColl"),
        sessionId,
        assertGet(ConnectionString::parse("TestConfigRS/CS1:12345,CS2:12345,CS3:12345")),
        assertGet(ConnectionString::parse("TestDonorRS/Donor1:12345,Donor2:12345,Donor3:12345")),
        ShardId("shard0002"),
        BSON("Key" << -100),
        BSON("Key" << 100),
        BSON("Key" << 1),
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff));

    BSONObj cmdObj = builder.obj();

    auto request = assertGet(StartChunkCloneRequest::createFromCommand(
        NamespaceString(cmdObj["_recvChunkStart"].String()), cmdObj));
    ASSERT_EQ("TestDB.TestColl", request.getNss().ns());
    ASSERT_EQ(sessionId.toString(), request.getSessionId().toString());
    ASSERT(sessionId.matches(request.getSessionId()));
    ASSERT_EQ("TestConfigRS/CS1:12345,CS2:12345,CS3:12345", request.getConfigServerCS().toString());
    ASSERT_EQ(
        assertGet(ConnectionString::parse("TestDonorRS/Donor1:12345,Donor2:12345,Donor3:12345"))
            .toString(),
        request.getFromShardConnectionString().toString());
    ASSERT_EQ("shard0002", request.getToShardId());
    ASSERT_EQ(BSON("Key" << -100), request.getMinKey());
    ASSERT_EQ(BSON("Key" << 100), request.getMaxKey());
    ASSERT_EQ(BSON("Key" << 1), request.getShardKeyPattern());
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOff,
              request.getSecondaryThrottle().getSecondaryThrottle());
}

}  // namespace
}  // namespace mongo
