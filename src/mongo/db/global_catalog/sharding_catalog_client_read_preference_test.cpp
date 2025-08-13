/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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


#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_gen.h"
#include "mongo/db/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"

#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using std::vector;
using unittest::assertGet;

const ShardId kShardId1 = ShardId("FakeShard1");
const ShardId kShardId2 = ShardId("FakeShard2");
const HostAndPort kShardHostAndPort = HostAndPort("FakeShard1Host", 12345);
const ShardType kShard1{kShardId1.toString(), kShardHostAndPort.toString()};
const ShardType kShard2{kShardId2.toString(), kShardHostAndPort.toString()};
const ShardType kConfigShard{"config", kShardHostAndPort.toString()};

class ShardingCatalogClientReadPreferenceCommonTest : public ShardingTestFixture {
public:
    void setUp() override {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(kShardHostAndPort);
    }

protected:
    void setUpShardRegistry(const std::vector<BSONObj>& shards) {
        auto future = launchAsync([this] { shardRegistry()->reload(operationContext()); });

        onFindCommand([&](const RemoteCommandRequest& request) {
            auto opMsg = static_cast<OpMsgRequest>(request);
            auto query = query_request_helper::makeFromFindCommandForTests(opMsg.body);

            ASSERT_EQ(query->getNamespaceOrUUID().nss(),
                      NamespaceString::kConfigsvrShardsNamespace);

            return shards;
        });

        future.default_timed_get();
    }

    void setClusterParameter(bool mustAlwaysUseNearest) {
        ConfigServerReadPreferenceForCatalogQueriesParam updatedParam;
        ClusterServerParameter baseCSP;
        baseCSP.setClusterParameterTime(LogicalTime(Timestamp(Date_t::now())));
        baseCSP.set_id("configServerReadPreferenceForCatalogQueries"_sd);
        updatedParam.setClusterServerParameter(baseCSP);
        updatedParam.setMustAlwaysUseNearest(mustAlwaysUseNearest);
        auto param = ServerParameterSet::getClusterParameterSet()->get(
            "configServerReadPreferenceForCatalogQueries");
        ASSERT_OK(param->set(updatedParam.toBSON(), boost::none));

        _assertClusterParameter(mustAlwaysUseNearest);
    }

    void resetClusterParameter() {
        auto param = ServerParameterSet::getClusterParameterSet()->get(
            "configServerReadPreferenceForCatalogQueries");
        ASSERT_OK(param->reset(boost::none));
    }

    void assertReadPreferenceForCatalogQuery(ReadPreference expectedReadPreference) {
        DatabaseType dbt(DatabaseName::createDatabaseName_forTest(boost::none, "db"),
                         kShardId1,
                         DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

        auto future = launchAsync([&] {
            return assertGet(catalogClient()->getDatabasesForShard(operationContext(), kShardId1));
        });

        onFindCommand([this, dbt, expectedReadPreference](const RemoteCommandRequest& request) {
            auto opMsg = static_cast<OpMsgRequest>(request);
            auto query = query_request_helper::makeFromFindCommandForTests(opMsg.body);

            BSONObjBuilder o;
            ReadPreferenceSetting(expectedReadPreference).toContainingBSON(&o);
            o.append(rpc::kReplSetMetadataFieldName, 1);

            ASSERT_BSONOBJ_EQ(o.obj(), request.metadata);
            ASSERT_EQ(NamespaceString::kConfigDatabasesNamespace,
                      query->getNamespaceOrUUID().nss());

            return vector<BSONObj>{dbt.toBSON()};
        });

        const auto& actualDbNames = future.default_timed_get();
        ASSERT_EQ(1, actualDbNames.size());
        ASSERT_EQ(dbt.getDbName(), actualDbNames[0]);
    }

private:
    void _assertClusterParameter(bool expectedMustAlwaysUseNearest) {
        auto* param =
            ServerParameterSet::getClusterParameterSet()
                ->getIfExists<
                    ClusterParameterWithStorage<ConfigServerReadPreferenceForCatalogQueriesParam>>(
                    "configServerReadPreferenceForCatalogQueries");
        ASSERT(param != nullptr);

        ASSERT(expectedMustAlwaysUseNearest ==
               param->getValue(boost::none).getMustAlwaysUseNearest());
    }
};

class ShardingCatalogClientReadPreferenceWithConfigShardTest
    : public ShardingCatalogClientReadPreferenceCommonTest {
public:
    void setUp() override {
        ShardingCatalogClientReadPreferenceCommonTest::setUp();

        setUpShardRegistry({kConfigShard.toBSON(), kShard1.toBSON(), kShard2.toBSON()});

        // Validate that the shard registry has cached data and it thinks the cluster does not have
        // a config shard.
        auto clusterHasConfigShard = shardRegistry()->cachedClusterHasConfigShard();
        ASSERT_NE(boost::none, clusterHasConfigShard);
        ASSERT(*clusterHasConfigShard);
    }
};

class ShardingCatalogClientReadPreferenceWithoutConfigShardTest
    : public ShardingCatalogClientReadPreferenceCommonTest {
public:
    void setUp() override {
        ShardingCatalogClientReadPreferenceCommonTest::setUp();

        setUpShardRegistry({kShard1.toBSON(), kShard2.toBSON()});

        // Validate that the shard registry has cached data and it thinks the cluster does not have
        // a config shard.
        auto clusterHasConfigShard = shardRegistry()->cachedClusterHasConfigShard();
        ASSERT_NE(boost::none, clusterHasConfigShard);
        ASSERT_FALSE(*clusterHasConfigShard);
    }
};

class ShardingCatalogClientReadPreferenceWithNoCachedDataInShardRegistry
    : public ShardingCatalogClientReadPreferenceCommonTest {
public:
    void setUp() override {
        ShardingCatalogClientReadPreferenceCommonTest::setUp();

        // Validate that the shard registry has no cached data, therefore, it is not able to know if
        // the cluster has a config shard or not.
        auto clusterHasConfigShard = shardRegistry()->cachedClusterHasConfigShard();
        ASSERT_EQ(boost::none, clusterHasConfigShard);
    }
};

TEST_F(ShardingCatalogClientReadPreferenceWithNoCachedDataInShardRegistry,
       CatalogQueriesUsePrimaryPreferredReadPreferenceNoCached) {
    setClusterParameter(false /* mustAlwaysUseNearest */);

    assertReadPreferenceForCatalogQuery(ReadPreference::PrimaryPreferred);
}

TEST_F(ShardingCatalogClientReadPreferenceWithoutConfigShardTest,
       CatalogQueriesUseNearestReadPreference) {
    setClusterParameter(false /* mustAlwaysUseNearest */);

    assertReadPreferenceForCatalogQuery(ReadPreference::Nearest);
}

TEST_F(ShardingCatalogClientReadPreferenceWithConfigShardTest,
       CatalogQueriesUsePrimaryPreferredReadPreference) {
    setClusterParameter(false /* mustAlwaysUseNearest */);

    assertReadPreferenceForCatalogQuery(ReadPreference::PrimaryPreferred);
}

TEST_F(ShardingCatalogClientReadPreferenceWithConfigShardTest,
       ClusterServerParameterForcesNearestReadPreference) {
    setClusterParameter(true /* mustAlwaysUseNearest */);

    assertReadPreferenceForCatalogQuery(ReadPreference::Nearest);
}

TEST_F(ShardingCatalogClientReadPreferenceWithConfigShardTest,
       CatalogQueriesUsePrimaryPreferredReadPreferenceWithNoClusterParameter) {
    resetClusterParameter();

    assertReadPreferenceForCatalogQuery(ReadPreference::PrimaryPreferred);
}

TEST_F(ShardingCatalogClientReadPreferenceWithNoCachedDataInShardRegistry,
       CatalogQueriesUsePrimaryPreferredReadPreferenceNoCachedWithNoClusterParameter) {
    resetClusterParameter();

    assertReadPreferenceForCatalogQuery(ReadPreference::PrimaryPreferred);
}

}  // namespace
}  // namespace mongo
