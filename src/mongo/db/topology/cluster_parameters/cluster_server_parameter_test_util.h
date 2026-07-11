// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_gen.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_test_gen.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace cluster_server_parameter_test_util {
using namespace std::literals::string_view_literals;

constexpr auto kCSPTest = "cspTest"sv;
constexpr auto kConfigDB = "config"sv;
const auto kNilCPT = LogicalTime::kUninitialized;

class ClusterServerParameterTestBase : public ServiceContextMongoDTest {
public:
    void setUp() override {
        gMultitenancySupport = true;
        // Set up mongod.
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();
        auto opCtx = cc().makeOperationContext();
        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceMock>());

        // Set up ReplicationCoordinator and create oplog.
        repl::ReplicationCoordinator::set(
            service,
            std::make_unique<repl::ReplicationCoordinatorMock>(service, createReplSettings()));
        repl::createOplog(opCtx.get());

        // Set up the ChangeStreamOptionsManager so that it can be retrieved/set.
        ChangeStreamOptionsManager::create(service);

        // Initialize the query settings.
        query_settings::QuerySettingsService::initializeForTest(service);

        // Ensure that we are primary.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    }

    static constexpr auto kInitialIntValue = 123;
    static constexpr auto kInitialTenantIntValue = 456;
    static constexpr auto kDefaultIntValue = 42;
    static constexpr auto kInitialStrValue = "initialState"sv;
    static constexpr auto kInitialTenantStrValue = "initialStateTenant"sv;
    static constexpr auto kDefaultStrValue = ""sv;

    static const TenantId kTenantId;

private:
    static repl::ReplSettings createReplSettings() {
        repl::ReplSettings settings;
        settings.setOplogSizeBytes(5 * 1024 * 1024);
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }
};

void upsert(BSONObj doc, const boost::optional<TenantId>& tenantId = boost::none);
void remove(const boost::optional<TenantId>& tenantId = boost::none);
BSONObj makeClusterParametersDoc(const LogicalTime& cpTime,
                                 int intValue,
                                 std::string_view strValue,
                                 std::string_view parameterName = kCSPTest);

}  // namespace cluster_server_parameter_test_util
}  // namespace mongo
