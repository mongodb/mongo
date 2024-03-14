/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include <memory>
#include <system_error>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/server_options.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/sharding_mongos_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/net/hostandport.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::TaskExecutor;
using unittest::assertGet;

BSONObj getReplSecondaryOkMetadata() {
    BSONObjBuilder o;
    ReadPreferenceSetting(ReadPreference::Nearest).toContainingBSON(&o);
    o.append(rpc::kReplSetMetadataFieldName, 1);
    return o.obj();
}

class ClusterIdentityTest : public ShardingTestFixture {
public:
    void setUp() {
        // TODO SERVER-78051: Remove once shards can access the loaded cluster id.
        serverGlobalParams.clusterRole = {
            ClusterRole::ShardServer, ClusterRole::ConfigServer, ClusterRole::RouterServer};

        ShardingTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(configHost);
    }

    void expectConfigVersionLoad(StatusWith<OID> result) {
        onFindCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(configHost, request.target);
            ASSERT_BSONOBJ_EQ(getReplSecondaryOkMetadata(),
                              rpc::TrackingMetadata::removeTrackingData(request.metadata));

            auto opMsg = static_cast<OpMsgRequest>(request);
            auto query = query_request_helper::makeFromFindCommandForTests(opMsg.body);

            ASSERT_EQ(query->getNamespaceOrUUID().nss(), NamespaceString::kConfigVersionNamespace);
            ASSERT_BSONOBJ_EQ(query->getFilter(), BSONObj());
            ASSERT_FALSE(query->getLimit().has_value());

            if (result.isOK()) {
                VersionType version;
                version.setClusterId(result.getValue());

                return StatusWith<std::vector<BSONObj>>{{version.toBSON()}};
            } else {
                return StatusWith<std::vector<BSONObj>>{result.getStatus()};
            }
        });
    }

protected:
    OID clusterId{OID::gen()};
    HostAndPort configHost{"TestHost1"};
};

TEST_F(ClusterIdentityTest, BasicLoadSuccess) {

    // The first time you ask for the cluster ID it will have to be loaded from the config servers.
    auto future = launchAsync([&] {
        auto clusterIdStatus = ClusterIdentityLoader::get(operationContext())
                                   ->loadClusterId(operationContext(),
                                                   catalogClient(),
                                                   repl::ReadConcernLevel::kMajorityReadConcern);
        ASSERT_OK(clusterIdStatus);
        ASSERT_EQUALS(clusterId, ClusterIdentityLoader::get(operationContext())->getClusterId());
    });

    expectConfigVersionLoad(clusterId);

    future.default_timed_get();

    // Subsequent requests for the cluster ID should not require any network traffic as we consult
    // the cached version.
    ASSERT_OK(ClusterIdentityLoader::get(operationContext())
                  ->loadClusterId(operationContext(),
                                  catalogClient(),
                                  repl::ReadConcernLevel::kMajorityReadConcern));
}

TEST_F(ClusterIdentityTest, NoConfigVersionDocument) {
    // If no version document is found on config server loadClusterId will return an error
    auto future = launchAsync([&] {
        ASSERT_EQ(ClusterIdentityLoader::get(operationContext())
                      ->loadClusterId(operationContext(),
                                      catalogClient(),
                                      repl::ReadConcernLevel::kMajorityReadConcern),
                  ErrorCodes::NoMatchingDocument);
    });

    expectConfigVersionLoad(
        Status(ErrorCodes::NoMatchingDocument, "No config version document found"));

    future.default_timed_get();
}

TEST_F(ClusterIdentityTest, MultipleThreadsLoadingSuccess) {
    // Check that multiple threads calling getClusterId at once still results in only one network
    // operation.
    auto future1 = launchAsync([&] {
        auto clusterIdStatus = ClusterIdentityLoader::get(operationContext())
                                   ->loadClusterId(operationContext(),
                                                   catalogClient(),
                                                   repl::ReadConcernLevel::kMajorityReadConcern);
        ASSERT_OK(clusterIdStatus);
        ASSERT_EQUALS(clusterId, ClusterIdentityLoader::get(operationContext())->getClusterId());
    });
    auto future2 = launchAsync([&] {
        auto clusterIdStatus = ClusterIdentityLoader::get(operationContext())
                                   ->loadClusterId(operationContext(),
                                                   catalogClient(),
                                                   repl::ReadConcernLevel::kMajorityReadConcern);
        ASSERT_OK(clusterIdStatus);
        ASSERT_EQUALS(clusterId, ClusterIdentityLoader::get(operationContext())->getClusterId());
    });
    auto future3 = launchAsync([&] {
        auto clusterIdStatus = ClusterIdentityLoader::get(operationContext())
                                   ->loadClusterId(operationContext(),
                                                   catalogClient(),
                                                   repl::ReadConcernLevel::kMajorityReadConcern);
        ASSERT_OK(clusterIdStatus);
        ASSERT_EQUALS(clusterId, ClusterIdentityLoader::get(operationContext())->getClusterId());
    });

    expectConfigVersionLoad(clusterId);

    future1.default_timed_get();
    future2.default_timed_get();
    future3.default_timed_get();
}

TEST_F(ClusterIdentityTest, BasicLoadFailureFollowedBySuccess) {

    // The first time you ask for the cluster ID it will have to be loaded from the config servers.
    auto future = launchAsync([&] {
        auto clusterIdStatus = ClusterIdentityLoader::get(operationContext())
                                   ->loadClusterId(operationContext(),
                                                   catalogClient(),
                                                   repl::ReadConcernLevel::kMajorityReadConcern);
        ASSERT_EQUALS(ErrorCodes::Interrupted, clusterIdStatus);
    });

    expectConfigVersionLoad(Status(ErrorCodes::Interrupted, "interrupted"));

    future.default_timed_get();

    // After a failure to load the cluster ID, subsequent attempts to get the cluster ID should
    // retry loading it.
    future = launchAsync([&] {
        auto clusterIdStatus = ClusterIdentityLoader::get(operationContext())
                                   ->loadClusterId(operationContext(),
                                                   catalogClient(),
                                                   repl::ReadConcernLevel::kMajorityReadConcern);
        ASSERT_OK(clusterIdStatus);
        ASSERT_EQUALS(clusterId, ClusterIdentityLoader::get(operationContext())->getClusterId());
    });

    expectConfigVersionLoad(clusterId);

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
