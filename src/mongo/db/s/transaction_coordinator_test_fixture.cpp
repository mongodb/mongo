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


#include "mongo/db/s/transaction_coordinator_test_fixture.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

HostAndPort makeHostAndPort(const ShardId& shardId) {
    return HostAndPort(str::stream() << shardId << ":123");
}

}  // namespace

void TransactionCoordinatorTestFixture::setUp() {
    ShardServerTestFixture::setUp();

    ASSERT_OK(ServerParameterSet::getNodeParameterSet()
                  ->get("logComponentVerbosity")
                  ->setFromString("{transaction: {verbosity: 3}}", boost::none));

    for (const auto& shardId : kThreeShardIdList) {
        auto shardTargeter = RemoteCommandTargeterMock::get(
            uassertStatusOK(shardRegistry()->getShard(operationContext(), shardId))->getTargeter());
        shardTargeter->setFindHostReturnValue(makeHostAndPort(shardId));
    }

    WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());
}

void TransactionCoordinatorTestFixture::tearDown() {
    WaitForMajorityService::get(getServiceContext()).shutDown();
    ShardServerTestFixture::tearDown();
}

std::unique_ptr<ShardingCatalogClient>
TransactionCoordinatorTestFixture::makeShardingCatalogClient() {

    class StaticCatalogClient final : public ShardingCatalogClientMock {
    public:
        StaticCatalogClient(std::vector<ShardId> shardIds) : _shardIds(std::move(shardIds)) {}

        repl::OpTimeWith<std::vector<ShardType>> getAllShards(OperationContext* opCtx,
                                                              repl::ReadConcernLevel readConcern,
                                                              BSONObj filter) override {
            std::vector<ShardType> shardTypes;
            for (const auto& shardId : _shardIds) {
                const ConnectionString cs =
                    ConnectionString::forReplicaSet(shardId.toString(), {makeHostAndPort(shardId)});
                ShardType sType;
                sType.setName(cs.getSetName());
                sType.setHost(cs.toString());
                shardTypes.push_back(std::move(sType));
            };
            return repl::OpTimeWith<std::vector<ShardType>>(shardTypes);
        }

    private:
        const std::vector<ShardId> _shardIds;
    };

    return std::make_unique<StaticCatalogClient>(kThreeShardIdList);
}

void TransactionCoordinatorTestFixture::assertCommandSentAndRespondWith(
    StringData commandName,
    const StatusWith<BSONObj>& response,
    boost::optional<WriteConcernOptions> expectedWriteConcern) {
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ(commandName, request.cmdObj.firstElement().fieldNameStringData());
        if (expectedWriteConcern) {
            ASSERT_BSONOBJ_EQ(
                expectedWriteConcern->toBSON(),
                request.cmdObj.getObjectField(WriteConcernOptions::kWriteConcernField));
        }
        return response;
    });
}

void TransactionCoordinatorTestFixture::advanceClockAndExecuteScheduledTasks() {
    executor::NetworkInterfaceMock::InNetworkGuard guard(network());
    network()->advanceTime(network()->now() + Seconds{1});
}

void TransactionCoordinatorTestFixture::associateClientMetadata(Client* client,
                                                                std::string appName) {
    BSONObjBuilder metadataBuilder;
    ASSERT_OK(ClientMetadata::serializePrivate(std::string("DriverName").insert(0, appName),
                                               std::string("DriverVersion").insert(0, appName),
                                               std::string("OsType").insert(0, appName),
                                               std::string("OsName").insert(0, appName),
                                               std::string("OsArchitecture").insert(0, appName),
                                               std::string("OsVersion").insert(0, appName),
                                               appName,
                                               &metadataBuilder));
    auto clientMetadata = metadataBuilder.obj();
    auto clientMetadataParse = ClientMetadata::parse(clientMetadata["client"]);
    ClientMetadata::setAndFinalize(client, std::move(clientMetadataParse.getValue()));
}
}  // namespace mongo
