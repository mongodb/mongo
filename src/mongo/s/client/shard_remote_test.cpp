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

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/db/logical_time.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/shard_id.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const ShardId dummyShardId("dummyShard");
const ConnectionString dummyShardCS("rs/dummy1:1234,dummy2:2345,dummy3:3456",
                                    ConnectionString::SET);

class ShardRemoteTest : public unittest::Test {
public:
    ShardFactory* getShardFactory() {
        return _shardFactory.get();
    }

private:
    void setUp() {
        auto targeterFactory = stdx::make_unique<RemoteCommandTargeterFactoryMock>();
        auto targeterFactoryPtr = targeterFactory.get();

        ShardFactory::BuilderCallable setBuilder =
            [targeterFactoryPtr](const ShardId& shardId, const ConnectionString& connStr) {
                return stdx::make_unique<ShardRemote>(
                    shardId, connStr, targeterFactoryPtr->create(connStr));
            };

        ShardFactory::BuilderCallable masterBuilder =
            [targeterFactoryPtr](const ShardId& shardId, const ConnectionString& connStr) {
                return stdx::make_unique<ShardRemote>(
                    shardId, connStr, targeterFactoryPtr->create(connStr));
            };

        ShardFactory::BuildersMap buildersMap{
            {ConnectionString::SET, std::move(setBuilder)},
            {ConnectionString::MASTER, std::move(masterBuilder)},
        };

        _shardFactory =
            stdx::make_unique<ShardFactory>(std::move(buildersMap), std::move(targeterFactory));
    }

    std::unique_ptr<ShardFactory> _shardFactory;
};

TEST_F(ShardRemoteTest, GetAndSetLatestLastCommittedOpTime) {
    auto shard = getShardFactory()->createShard(dummyShardId, dummyShardCS);

    // Shards can store last committed opTimes.
    LogicalTime time(Timestamp(10, 2));
    shard->updateLastCommittedOpTime(time);
    ASSERT_EQ(time, shard->getLastCommittedOpTime());

    // Later times overwrite earlier times.
    LogicalTime laterTime(Timestamp(20, 2));
    shard->updateLastCommittedOpTime(laterTime);
    ASSERT_EQ(laterTime, shard->getLastCommittedOpTime());

    // Earlier times are ignored.
    LogicalTime earlierTime(Timestamp(5, 1));
    shard->updateLastCommittedOpTime(earlierTime);
    ASSERT_EQ(laterTime, shard->getLastCommittedOpTime());
}

}  // namespace
}  // namespace mongo
