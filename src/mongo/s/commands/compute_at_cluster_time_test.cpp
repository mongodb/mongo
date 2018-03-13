/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/logical_time.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);

const ShardId shardOneId("shardOne");
const HostAndPort shardOne("shardOne:1234");

const ShardId shardTwoId("shardTwo");
const HostAndPort shardTwo("shardTwo:1234");

class AtClusterTimeTest : public ShardingTestFixture {
protected:
    void setUp() {
        ShardingTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);
        std::vector<std::tuple<ShardId, HostAndPort>> shardInfos;
        shardInfos.push_back(std::make_tuple(shardOneId, shardOne));
        shardInfos.push_back(std::make_tuple(shardTwoId, shardTwo));

        ShardingTestFixture::addRemoteShards(shardInfos);
    }
};

TEST_F(AtClusterTimeTest, ComputeValidValid) {
    auto shardOne = shardRegistry()->getShardNoReload(shardOneId);
    LogicalTime timeOne(Timestamp(10, 2));
    shardOne->updateLastCommittedOpTime(timeOne);
    ASSERT_EQ(timeOne, shardOne->getLastCommittedOpTime());

    auto shardTwo = shardRegistry()->getShardNoReload(shardTwoId);
    LogicalTime timeTwo(Timestamp(15, 1));
    shardTwo->updateLastCommittedOpTime(timeTwo);
    ASSERT_EQ(timeTwo, shardTwo->getLastCommittedOpTime());

    auto maxTime = computeAtClusterTime(operationContext(), {shardOneId, shardTwoId});
    ASSERT_EQ(maxTime, timeTwo);
}

TEST_F(AtClusterTimeTest, ComputeValidInvalid) {
    auto shardOne = shardRegistry()->getShardNoReload(shardOneId);
    ASSERT_EQ(LogicalTime(), shardOne->getLastCommittedOpTime());

    auto shardTwo = shardRegistry()->getShardNoReload(shardTwoId);
    LogicalTime timeTwo(Timestamp(15, 1));
    shardTwo->updateLastCommittedOpTime(timeTwo);
    ASSERT_EQ(timeTwo, shardTwo->getLastCommittedOpTime());

    auto maxTime = computeAtClusterTime(operationContext(), {shardOneId, shardTwoId});
    ASSERT_EQ(maxTime, timeTwo);
}

TEST_F(AtClusterTimeTest, ComputeInvalidInvalid) {
    auto shardOne = shardRegistry()->getShardNoReload(shardOneId);
    ASSERT_EQ(LogicalTime(), shardOne->getLastCommittedOpTime());

    auto shardTwo = shardRegistry()->getShardNoReload(shardTwoId);
    ASSERT_EQ(LogicalTime(), shardTwo->getLastCommittedOpTime());

    auto maxTime = computeAtClusterTime(operationContext(), {shardOneId, shardTwoId});
    ASSERT_EQ(maxTime, LogicalTime());
}

}  // namespace
}  // namespace mongo
