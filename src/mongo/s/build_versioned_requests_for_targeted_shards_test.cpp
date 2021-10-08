/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/unittest/unittest.h"

#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/database_version.h"

namespace mongo {
namespace {

const NamespaceString kNss("TestDB", "TestColl");

}  // namespace

class BuildVersionedRequestsForTargetedShardsTest : public CatalogCacheTestFixture {
protected:
    /**
     * Runs 'buildVersionedRequestsForTargetedShards' and asserts that the returned vector matches
     * the expected vector.
     */
    void runBuildVersionedRequestsExpect(
        const ChunkManager& cm,
        const std::set<ShardId>& shardsToSkip,
        const BSONObj& cmdObj,
        const BSONObj& query,
        const BSONObj& collation,
        const std::vector<AsyncRequestsSender::Request>& expectedRequests) {

        const auto actualRequests = buildVersionedRequestsForTargetedShards(
            operationContext(), kNss, cm, shardsToSkip, cmdObj, query, collation);

        ASSERT_EQ(expectedRequests.size(), actualRequests.size());
        _assertShardIdsMatch(expectedRequests, actualRequests);
    }

    void setUp() override {
        CatalogCacheTestFixture::setUp();

        _shards = setupNShards(2);
    }

    void expectGetDatabaseUnsharded() {
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            DatabaseType db(
                kNss.db().toString(), {"0"}, false, DatabaseVersion(UUID::gen(), Timestamp(1, 1)));
            return std::vector<BSONObj>{db.toBSON()};
        }());
    }

    void expectGetCollectionUnsharded() {
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() { return std::vector<BSONObj>{}; }());
    }

    std::vector<ShardType> _shards;

private:
    static void _assertShardIdsMatch(
        const std::vector<AsyncRequestsSender::Request>& expectedShardIdsFromRequest,
        const std::vector<AsyncRequestsSender::Request>& actualShardIdsFromRequest) {
        BSONArrayBuilder expectedBuilder;
        for (const auto& [shardId, cmdObj] : expectedShardIdsFromRequest) {
            expectedBuilder << shardId;
        }

        BSONArrayBuilder actualBuilder;
        for (const auto& [shardId, cmdObj] : actualShardIdsFromRequest) {
            actualBuilder << shardId;
        }

        ASSERT_BSONOBJ_EQ(expectedBuilder.arr(), actualBuilder.arr());
    }
};

//
// Database is not sharded
//

TEST_F(BuildVersionedRequestsForTargetedShardsTest, ReturnPrimaryShardForUnshardedDatabase) {
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabaseUnsharded();
    expectGetCollectionUnsharded();

    auto cm = future.default_timed_get();

    AsyncRequestsSender::Request expectedRequest{ShardId(_shards[0].getName()), {}};
    runBuildVersionedRequestsExpect(*cm, {}, {}, {}, {}, {expectedRequest});
}

TEST_F(BuildVersionedRequestsForTargetedShardsTest,
       ReturnNothingForUnshardedDatabaseIfPrimaryShardIsSkipped) {
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabaseUnsharded();
    expectGetCollectionUnsharded();

    auto cm = future.default_timed_get();

    runBuildVersionedRequestsExpect(*cm, {ShardId(_shards[0].getName())}, {}, {}, {}, {});
}

}  // namespace mongo
