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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/s/sharding_test_fixture_common.h"

#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {

using executor::NetworkTestEnv;
using executor::RemoteCommandRequest;
using unittest::assertGet;

ShardingTestFixtureCommon::ShardingTestFixtureCommon() {
    auto service = getServiceContext();
    service->registerClientObserver(
        std::make_unique<LockerNoopClientObserverWithReplacementPolicy>());
}

ShardingTestFixtureCommon::~ShardingTestFixtureCommon() {
    invariant(!_opCtxHolder,
              "ShardingTestFixtureCommon::tearDown() must have been called before destruction");
}

void ShardingTestFixtureCommon::setUp() {
    _opCtxHolder = makeOperationContext();
}

void ShardingTestFixtureCommon::tearDown() {
    _opCtxHolder.reset();
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

void ShardingTestFixtureCommon::expectConfigCollectionCreate(const HostAndPort& configHost,
                                                             StringData collName,
                                                             int cappedSize,
                                                             const BSONObj& response) {
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BSONObj expectedCreateCmd =
            BSON("create" << collName << "capped" << true << "size" << cappedSize << "writeConcern"
                          << BSON("w"
                                  << "majority"
                                  << "wtimeout" << 60000)
                          << "maxTimeMS" << 30000);
        ASSERT_BSONOBJ_EQ(expectedCreateCmd, request.cmdObj);

        return response;
    });
}

void ShardingTestFixtureCommon::expectConfigCollectionInsert(const HostAndPort& configHost,
                                                             StringData collName,
                                                             Date_t timestamp,
                                                             const std::string& what,
                                                             const std::string& ns,
                                                             const BSONObj& detail) {
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS(NamespaceString::kConfigDb, request.dbname);

        const auto opMsg = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
        const auto batchRequest(BatchedCommandRequest::parseInsert(opMsg));
        const auto& insertReq(batchRequest.getInsertRequest());

        ASSERT_EQ(NamespaceString::kConfigDb, insertReq.getNamespace().db());
        ASSERT_EQ(collName, insertReq.getNamespace().coll());

        const auto& inserts = insertReq.getDocuments();
        ASSERT_EQUALS(1U, inserts.size());

        const ChangeLogType& actualChangeLog = assertGet(ChangeLogType::fromBSON(inserts.front()));

        ASSERT_EQUALS(operationContext()->getClient()->clientAddress(true),
                      actualChangeLog.getClientAddr());
        ASSERT_BSONOBJ_EQ(detail, actualChangeLog.getDetails());
        ASSERT_EQUALS(ns, actualChangeLog.getNS());
        const std::string expectedServer = str::stream() << network()->getHostName() << ":27017";
        ASSERT_EQUALS(expectedServer, actualChangeLog.getServer());
        ASSERT_EQUALS(timestamp, actualChangeLog.getTime());
        ASSERT_EQUALS(what, actualChangeLog.getWhat());

        // Handle changeId specially because there's no way to know what OID was generated
        std::string changeId = actualChangeLog.getChangeId();
        size_t firstDash = changeId.find("-");
        size_t lastDash = changeId.rfind("-");

        const std::string serverPiece = changeId.substr(0, firstDash);
        const std::string timePiece = changeId.substr(firstDash + 1, lastDash - firstDash - 1);
        const std::string oidPiece = changeId.substr(lastDash + 1);

        const std::string expectedServerPiece = str::stream()
            << Grid::get(operationContext())->getNetwork()->getHostName() << ":27017";
        ASSERT_EQUALS(expectedServerPiece, serverPiece);
        ASSERT_EQUALS(timestamp.toString(), timePiece);

        OID generatedOID;
        // Just make sure this doesn't throws and assume the OID is valid
        generatedOID.init(oidPiece);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());

        return response.toBSON();
    });
}

}  // namespace mongo
