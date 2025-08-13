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


#include "mongo/db/sharding_environment/sharding_test_fixture_common.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielders.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <cstddef>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

using executor::NetworkTestEnv;

ShardingTestFixtureCommon::ShardingTestFixtureCommon(
    std::unique_ptr<ScopedGlobalServiceContextForTest> scopedGlobalContext)
    : ServiceContextTest(std::move(scopedGlobalContext)) {}

ShardingTestFixtureCommon::~ShardingTestFixtureCommon() {
    invariant(!_opCtxHolder,
              "ShardingTestFixtureCommon::tearDown() must have been called before destruction");
}

void ShardingTestFixtureCommon::setUp() {
    _opCtxHolder = makeOperationContext();

    ResourceYielderFactory::initialize(getServiceContext());
}

void ShardingTestFixtureCommon::tearDown() {
    _opCtxHolder.reset();
}

void ShardingTestFixtureCommon::shutdownExecutorPool() {
    auto grid = Grid::get(getServiceContext());

    if (!grid || !grid->isInitialized() || !grid->getExecutorPool() || _executorPoolShutDown) {
        return;
    }
    _executorPoolShutDown = true;

    grid->getExecutorPool()->shutdown_forTest();
    for (auto mockNet : {&_mockNetwork, &_mockNetworkForPool}) {
        executor::NetworkInterfaceMock::InNetworkGuard(*mockNet)
            ->drainUnfinishedNetworkOperations();
    }
    grid->getExecutorPool()->join_forTest();
}

OperationContext* ShardingTestFixtureCommon::operationContext() const {
    invariant(_opCtxHolder,
              "ShardingTestFixtureCommon::setUp() must have been called before this method");
    return _opCtxHolder.get();
}

RoutingTableHistoryValueHandle ShardingTestFixtureCommon::makeStandaloneRoutingTableHistory(
    RoutingTableHistory rt) {
    const auto version = rt.getVersion();
    return RoutingTableHistoryValueHandle(
        std::make_shared<RoutingTableHistory>(std::move(rt)),
        ComparableChunkVersion::makeComparableChunkVersion(version));
}

void ShardingTestFixtureCommon::onCommand(NetworkTestEnv::OnCommandFunction func) {
    _networkTestEnv->onCommand(func);
}

void ShardingTestFixtureCommon::onCommands(
    std::vector<executor::NetworkTestEnv::OnCommandFunction> funcs) {
    _networkTestEnv->onCommands(std::move(funcs));
}

void ShardingTestFixtureCommon::onCommandWithMetadata(
    NetworkTestEnv::OnCommandWithMetadataFunction func) {
    _networkTestEnv->onCommandWithMetadata(func);
}

void ShardingTestFixtureCommon::onFindCommand(NetworkTestEnv::OnFindCommandFunction func) {
    _networkTestEnv->onFindCommand(func);
}

void ShardingTestFixtureCommon::onFindWithMetadataCommand(
    NetworkTestEnv::OnFindCommandWithMetadataFunction func) {
    _networkTestEnv->onFindWithMetadataCommand(func);
}

}  // namespace mongo
