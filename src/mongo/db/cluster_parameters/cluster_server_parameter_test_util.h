/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_gen.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_test_gen.h"
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
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace cluster_server_parameter_test_util {

constexpr auto kCSPTest = "cspTest"_sd;
constexpr auto kConfigDB = "config"_sd;
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
    static constexpr auto kInitialStrValue = "initialState"_sd;
    static constexpr auto kInitialTenantStrValue = "initialStateTenant"_sd;
    static constexpr auto kDefaultStrValue = ""_sd;

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
                                 StringData strValue,
                                 StringData parameterName = kCSPTest);

}  // namespace cluster_server_parameter_test_util
}  // namespace mongo
