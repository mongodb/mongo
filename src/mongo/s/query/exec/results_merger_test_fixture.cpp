// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/results_merger_test_fixture.h"

#include "mongo/db/curop.h"
#include "mongo/db/global_catalog/type_shard.h"


namespace mongo {
const HostAndPort ResultsMergerTestFixture::kTestConfigShardHost =
    HostAndPort("FakeConfigHost", 12345);
const std::vector<ShardId> ResultsMergerTestFixture::kTestShardIds = {
    ShardId("FakeShard1"), ShardId("FakeShard2"), ShardId("FakeShard3")};
const std::vector<HostAndPort> ResultsMergerTestFixture::kTestShardHosts = {
    HostAndPort("FakeShard1Host", 12345),
    HostAndPort("FakeShard2Host", 12345),
    HostAndPort("FakeShard3Host", 12345)};

const NamespaceString ResultsMergerTestFixture::kTestNss =
    NamespaceString::createNamespaceString_forTest("testdb.testcoll");

void ResultsMergerTestFixture::setUp() {
    ShardingTestFixture::setUp();

    configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

    // Setup the shards
    std::vector<std::tuple<ShardId, HostAndPort>> shardInfos;
    for (size_t i = 0; i < kTestShardIds.size(); i++) {
        shardInfos.emplace_back(std::make_tuple(kTestShardIds[i], kTestShardHosts[i]));
    }
    addRemoteShards(shardInfos);

    CurOp::get(operationContext())->ensureStarted();
}

}  // namespace mongo
