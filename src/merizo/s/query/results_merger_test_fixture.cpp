/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/client/remote_command_targeter_factory_mock.h"
#include "merizo/client/remote_command_targeter_mock.h"
#include "merizo/executor/network_interface_mock.h"
#include "merizo/executor/thread_pool_task_executor_test_fixture.h"
#include "merizo/s/catalog/type_shard.h"
#include "merizo/s/query/results_merger_test_fixture.h"

namespace merizo {
const HostAndPort ResultsMergerTestFixture::kTestConfigShardHost =
    HostAndPort("FakeConfigHost", 12345);
const std::vector<ShardId> ResultsMergerTestFixture::kTestShardIds = {
    ShardId("FakeShard1"), ShardId("FakeShard2"), ShardId("FakeShard3")};
const std::vector<HostAndPort> ResultsMergerTestFixture::kTestShardHosts = {
    HostAndPort("FakeShard1Host", 12345),
    HostAndPort("FakeShard2Host", 12345),
    HostAndPort("FakeShard3Host", 12345)};

const NamespaceString ResultsMergerTestFixture::kTestNss = NamespaceString{"testdb.testcoll"};

void ResultsMergerTestFixture::setUp() {
    setRemote(HostAndPort("ClientHost", 12345));

    configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

    std::vector<ShardType> shards;

    for (size_t i = 0; i < kTestShardIds.size(); i++) {
        ShardType shardType;
        shardType.setName(kTestShardIds[i].toString());
        shardType.setHost(kTestShardHosts[i].toString());

        shards.push_back(shardType);

        std::unique_ptr<RemoteCommandTargeterMock> targeter(
            stdx::make_unique<RemoteCommandTargeterMock>());
        targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHosts[i]));
        targeter->setFindHostReturnValue(kTestShardHosts[i]);

        targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHosts[i]),
                                               std::move(targeter));
    }

    setupShards(shards);
}

}  // namespace merizo
