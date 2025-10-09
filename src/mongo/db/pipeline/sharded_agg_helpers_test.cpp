/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/sharded_agg_helpers.h"

#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/s/query/exec/sharded_agg_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::sharded_agg_helpers {
namespace {

class ShardedAggHelpersFixture : public ShardedAggTestFixture {
public:
    std::set<ShardId> setupShards(size_t n) {
        std::set<ShardId> allShardIds;
        for (auto&& shard : setupNShards(n)) {
            allShardIds.insert(shard.getName());
        }
        return allShardIds;
    }

    void setChangeStreamVersionToExpCtx(ChangeStreamReaderVersionEnum version) {
        auto&& spec = expCtx()->getChangeStreamSpec().value_or(DocumentSourceChangeStreamSpec());
        spec.setVersion(version);
        expCtx()->setChangeStreamSpec(spec);
    }
};

TEST_F(
    ShardedAggHelpersFixture,
    Given_GeneratesOwnDataOnceAndMergeShardId_When_CallingGetTargetedShards_Then_ReturnsMergeShardId) {
    for (auto&& shardId : setupShards(3)) {
        ASSERT_EQ(getTargetedShards(expCtx(),
                                    PipelineDataSource::kGeneratesOwnDataOnce,
                                    boost::none /* cri */,
                                    BSONObj(),
                                    BSONObj(),
                                    shardId),
                  std::set<ShardId>{shardId});
    }
}

TEST_F(
    ShardedAggHelpersFixture,
    Given_GeneratesOwnDataOnceAndNoMergeShardId_When_CallingGetTargetedShards_Then_ReturnsShardIdFromTheGetDatabaseCall) {
    auto allShardIds = setupShards(3);
    ShardId shardId("1");

    // Mock ShardId returned by the GetDatabase call for the given namespace.
    auto future = launchAsync([&] { expectGetDatabase(kTestAggregateNss, shardId.toString()); });

    ASSERT_EQ(getTargetedShards(expCtx(),
                                PipelineDataSource::kGeneratesOwnDataOnce,
                                boost::none /* cri */,
                                BSONObj(),
                                BSONObj(),
                                boost::none /* mergeShardId */),
              std::set<ShardId>{shardId});

    future.default_timed_get();
}

TEST_F(ShardedAggHelpersFixture,
       Given_ChangeStreamV1_WhenCallingGetTargetedShards_Then_ReturnsAllShards) {
    std::set<ShardId> allShardIds = setupShards(3);
    setChangeStreamVersionToExpCtx(ChangeStreamReaderVersionEnum::kV1);
    ASSERT_EQ(getTargetedShards(expCtx(),
                                PipelineDataSource::kChangeStream,
                                boost::none /* cri */,
                                BSONObj(),
                                BSONObj(),
                                boost::none /* mergeShardId */),
              allShardIds);
}

TEST_F(ShardedAggHelpersFixture,
       Given_ChangeStreamV2_WhenCallingGetTargetedShards_Then_ReturnsNoShards) {
    std::set<ShardId> allShardIds = setupShards(3);
    setChangeStreamVersionToExpCtx(ChangeStreamReaderVersionEnum::kV2);
    ASSERT_EQ(getTargetedShards(expCtx(),
                                PipelineDataSource::kChangeStream,
                                boost::none /* cri */,
                                BSONObj(),
                                BSONObj(),
                                boost::none /* mergeShardId */),
              std::set<ShardId>{});
}

TEST_F(
    ShardedAggHelpersFixture,
    Given_NormalDataSourceAndCollectionlessNss_When_CallingGetTargetedShards_Then_ReturnsAllShards) {
    std::set<ShardId> allShardIds = setupShards(3);
    auto nss = NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kMdbTesting);
    expCtx()->setNamespaceString(nss);
    ASSERT_EQ(getTargetedShards(expCtx(),
                                PipelineDataSource::kNormal,
                                boost::none /* cri */,
                                BSONObj(),
                                BSONObj(),
                                boost::none /* mergeShardId */),
              allShardIds);
}

TEST_F(
    ShardedAggHelpersFixture,
    Given_NormalDataSourceAndUnshardedCollection_When_CallingGetTargetedShards_Then_ReturnsDbPrimaryShard) {
    std::set<ShardId> allShardIds = setupShards(3);

    expCtx()->setNamespaceString(kTestAggregateNss);
    auto cri = makeUnshardedCollectionRoutingInfo(kTestAggregateNss);
    ASSERT_EQ(getTargetedShards(expCtx(),
                                PipelineDataSource::kNormal,
                                cri,
                                BSONObj(),
                                BSONObj(),
                                boost::none /* mergeShardId */),
              std::set<ShardId>{ShardId("0")});
}

TEST_F(
    ShardedAggHelpersFixture,
    Given_NormalDataSourceAndUntrackedCollection_When_CallingGetTargetedShards_Then_ReturnsDbPrimaryShard) {
    std::set<ShardId> allShardIds = setupShards(3);

    expCtx()->setNamespaceString(kTestAggregateNss);
    auto cri = makeUntrackedCollectionRoutingInfo(kTestAggregateNss);
    ASSERT_EQ(getTargetedShards(expCtx(),
                                PipelineDataSource::kNormal,
                                cri,
                                BSONObj(),
                                BSONObj(),
                                boost::none /* mergeShardId */),
              std::set<ShardId>{ShardId("0")});
}

TEST_F(
    ShardedAggHelpersFixture,
    Given_NormalDataSourceAndCriWithRoutingInfo_When_CallingGetTargetedShards_Then_ReturnsRelevantShards) {
    std::set<ShardId> allShardIds = setupShards(3);
    auto nss = kTestAggregateNss;
    expCtx()->setNamespaceString(nss);

    loadRoutingTableWithTwoChunksAndTwoShards(nss);
    std::set<ShardId> expectedShardIds{ShardId("0"), ShardId("1")};

    auto catalogCache = Grid::get(getServiceContext())->catalogCache();
    const auto cri =
        uassertStatusOK(catalogCache->getCollectionRoutingInfo(operationContext(), nss));
    ASSERT_EQ(getTargetedShards(expCtx(),
                                PipelineDataSource::kNormal,
                                cri,
                                BSONObj(),
                                BSONObj(),
                                boost::none /* mergeShardId */),
              expectedShardIds);
}

TEST_F(
    ShardedAggHelpersFixture,
    Given_NonChangeStreamPipeline_When_CallingOpenChangeStreamOnConfigsvr_Then_ReturnsNoRemoteCursor) {
    ASSERT_EQ(openChangeStreamCursorOnConfigsvrIfNeeded(
                  expCtx(), PipelineDataSource::kGeneratesOwnDataOnce, Timestamp()),
              boost::none);
    ASSERT_EQ(openChangeStreamCursorOnConfigsvrIfNeeded(
                  expCtx(), PipelineDataSource::kNormal, Timestamp()),
              boost::none);
}

TEST_F(
    ShardedAggHelpersFixture,
    Given_ChangeStreamV2Pipeline_When_CallingOpenChangeStreamOnConfigsvr_Then_ReturnsNoRemoteCursor) {
    setChangeStreamVersionToExpCtx(ChangeStreamReaderVersionEnum::kV2);
    ASSERT_EQ(openChangeStreamCursorOnConfigsvrIfNeeded(
                  expCtx(), PipelineDataSource::kChangeStream, Timestamp()),
              boost::none);
}

TEST_F(
    ShardedAggHelpersFixture,
    Given_ChangeStreamPipelineOverConfigShardsNss_When_CallingOpenChangeStreamOnConfigsvr_Then_ReturnsNoRemoteCursor) {
    expCtx()->setNamespaceString(NamespaceString::kConfigsvrShardsNamespace);
    ASSERT_EQ(openChangeStreamCursorOnConfigsvrIfNeeded(
                  expCtx(), PipelineDataSource::kChangeStream, Timestamp()),
              boost::none);
}

TEST_F(
    ShardedAggHelpersFixture,
    Given_ChangeStreamPipelineOverRegularNss_When_CallingOpenChangeStreamOnConfigsvr_Then_ReturnsCursorOverConfigsvr) {
    expCtx()->setNamespaceString(kTestAggregateNss);

    auto future = launchAsync([&] {
        auto&& configsvrCursor = openChangeStreamCursorOnConfigsvrIfNeeded(
            expCtx(), PipelineDataSource::kChangeStream, Timestamp());
        ASSERT_EQ(configsvrCursor->getCursorResponse().getCursorId(), CursorId(123));
    });

    // Mock response for shard refresh.
    expectGetShards({ShardType("1", "1")});

    // Mock response from the agg request to the configsvr.
    onCommand([this](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ(NamespaceString::kConfigsvrShardsNamespace.coll(),
                  request.cmdObj.firstElement().valueStringData());

        CursorResponse cursorResponse(
            NamespaceString::kConfigsvrShardsNamespace, CursorId(123), {});
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo::sharded_agg_helpers
