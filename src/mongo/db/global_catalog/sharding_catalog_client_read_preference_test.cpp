// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_gen.h"
#include "mongo/db/topology/cluster_parameters/sharding_cluster_parameters_gen.h"
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
using namespace std::literals::string_view_literals;

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
        baseCSP.set_id("configServerReadPreferenceForCatalogQueries"sv);
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

    void assertReadPreferenceForGetAllDBs(
        ReadPreference expectedReadPreference,
        const boost::optional<ReadPreferenceSetting>& readPrefOverride = boost::none) {
        DatabaseType dbt(DatabaseName::createDatabaseName_forTest(boost::none, "db"),
                         kShardId1,
                         DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

        auto future = launchAsync([&] {
            return catalogClient()->getAllDBs(
                operationContext(), repl::ReadConcernArgs::kSnapshot, readPrefOverride);
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

        const auto& actualDbs = future.default_timed_get();
        ASSERT_EQ(1, actualDbs.size());
        ASSERT_EQ(dbt.getDbName(), actualDbs[0].getDbName());
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

TEST_F(ShardingCatalogClientReadPreferenceWithConfigShardTest,
       GetAllDBsOverridesPrimaryOnlyReadPreference) {
    setClusterParameter(false /* mustAlwaysUseNearest */);

    // With a config shard the default would be PrimaryPreferred, but the explicit override
    // should force PrimaryOnly.
    assertReadPreferenceForGetAllDBs(ReadPreference::PrimaryOnly,
                                     ReadPreferenceSetting{ReadPreference::PrimaryOnly});
}

TEST_F(ShardingCatalogClientReadPreferenceWithConfigShardTest,
       GetAllDBsUsesDefaultReadPreferenceWhenNoOverride) {
    setClusterParameter(false /* mustAlwaysUseNearest */);

    // Without an override, getAllDBs should use the default PrimaryPreferred for config shard.
    assertReadPreferenceForGetAllDBs(ReadPreference::PrimaryPreferred);
}

TEST_F(ShardingCatalogClientReadPreferenceWithoutConfigShardTest,
       GetAllDBsOverridesPrimaryOnlyReadPreference) {
    setClusterParameter(false /* mustAlwaysUseNearest */);

    // Without a config shard the default would be Nearest, but the explicit override
    // should force PrimaryOnly.
    assertReadPreferenceForGetAllDBs(ReadPreference::PrimaryOnly,
                                     ReadPreferenceSetting{ReadPreference::PrimaryOnly});
}

}  // namespace
}  // namespace mongo
