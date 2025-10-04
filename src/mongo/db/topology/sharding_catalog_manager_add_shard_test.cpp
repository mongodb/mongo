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
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/cluster_parameters/set_cluster_parameter_invocation.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_changelog.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/cluster_identity_loader.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/db/topology/add_shard_gen.h"
#include "mongo/db/topology/topology_change_helpers.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/wire_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using std::vector;
using unittest::assertGet;

const Hours kLongFutureTimeout(8);

class AddShardTest : public ConfigServerTestFixture {
protected:
    /**
     * Performs the test setup steps from the parent class and then configures the config shard and
     * the client name.
     */
    void setUp() override {
        setUpAndInitializeConfigDb();

        auto clusterIdLoader = ClusterIdentityLoader::get(operationContext());
        ASSERT_OK(clusterIdLoader->loadClusterId(
            operationContext(), catalogClient(), repl::ReadConcernLevel::kLocalReadConcern));
        _clusterId = clusterIdLoader->getClusterId();

        // Manually instantiate the ReadWriteConcernDefaults decoration on the service
        ReadWriteConcernDefaults::create(getService(), _lookupMock.getFetchDefaultsFn());
        // Create config.transactions collection
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->initializeIfNeeded(operationContext(), /* term */ 1);
        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());

        // the primary only services to have been set up.
        _skipUpdatingCardinalityParamFP =
            globalFailPointRegistry().find("skipUpdatingClusterCardinalityParameterAfterAddShard");
        _skipUpdatingCardinalityParamFP->setMode(FailPoint::alwaysOn);

        _skipBlockingDDLCoordinatorsDuringAddAndRemoveShardFP =
            globalFailPointRegistry().find("skipBlockingDDLCoordinatorsDuringAddAndRemoveShard");
        _skipBlockingDDLCoordinatorsDuringAddAndRemoveShardFP->setMode(FailPoint::alwaysOn);
    }

    void tearDown() override {
        _skipUpdatingCardinalityParamFP->setMode(FailPoint::off);
        _skipBlockingDDLCoordinatorsDuringAddAndRemoveShardFP->setMode(FailPoint::off);
        WaitForMajorityService::get(getServiceContext()).shutDown();
        TransactionCoordinatorService::get(operationContext())->interrupt();
        ConfigServerTestFixture::tearDown();
    }

    /**
     * addShard validates the host as a shard. It calls "hello" on the host to determine what
     * kind of host it is -- mongos, regular mongod, config mongod -- and whether the replica set
     * details are correct. "helloResponse" defines the response of the "hello" request and
     * should be a command response BSONObj, or a failed Status.
     *
     * ShardingTestFixture::expectGetShards() should be called before this function, otherwise
     * addShard will never reach the "hello" command -- a find query is called first.
     */
    void expectHello(const HostAndPort& target, StatusWith<BSONObj> helloResponse) {
        onCommandForAddShard([&, target, helloResponse](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, DatabaseName::kAdmin);
            ASSERT_BSONOBJ_EQ(request.cmdObj, BSON("hello" << 1));
            ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(), request.metadata);

            return helloResponse;
        });
    }

    void expectListDatabases(const HostAndPort& target, const std::vector<BSONObj>& dbs) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, DatabaseName::kAdmin);
            ASSERT_BSONOBJ_EQ(request.cmdObj, BSON("listDatabases" << 1 << "nameOnly" << true));
            ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(), request.metadata);

            BSONArrayBuilder arr;
            for (const auto& db : dbs) {
                arr.append(db);
            }

            return BSON("ok" << 1 << "databases" << arr.obj());
        });
    }

    void expectCollectionDrop(const HostAndPort& target, const NamespaceString& nss) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, nss.dbName());
            ASSERT_BSONOBJ_EQ(
                request.cmdObj,
                BSON("drop" << nss.coll() << "writeConcern" << BSON("w" << "majority")));
            ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(), request.metadata);

            return BSON("ok" << 1);
        });
    }

    void expectSetFeatureCompatibilityVersion(const HostAndPort& target,
                                              StatusWith<BSONObj> response,
                                              WriteConcernOptions writeConcern) {
        // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
        SetFeatureCompatibilityVersion fcvCmd(multiversion::GenericFCV::kLatest);
        fcvCmd.setFromConfigServer(true);
        fcvCmd.setDbName(DatabaseName::kAdmin);
        fcvCmd.setWriteConcern(writeConcern);
        const auto setFcvObj = fcvCmd.toBSON();

        onCommandForAddShard([&, target, response](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, DatabaseName::kAdmin);
            ASSERT_BSONOBJ_EQ(request.cmdObj, setFcvObj);

            return response;
        });
    }

    void expectRemoveUserWritesCriticalSectionsDocs(const HostAndPort& target) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname,
                      NamespaceString::kUserWritesCriticalSectionsNamespace.dbName());
            ASSERT_BSONOBJ_EQ(request.cmdObj,
                              BSON("find"
                                   << NamespaceString::kUserWritesCriticalSectionsNamespace.coll()
                                   << "maxTimeMS" << 60000 << "readConcern"
                                   << BSON("level" << "majority")));

            auto cursorRes = CursorResponse(
                NamespaceString::createNamespaceString_forTest(
                    request.dbname, NamespaceString::kUserWritesCriticalSectionsNamespace.coll()),
                0,
                {
                    BSON("_id" << "doc1"),
                    BSON("_id" << "doc2"),
                });
            return cursorRes.toBSON(CursorResponse::ResponseType::InitialResponse);
        });

        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname,
                      NamespaceString::kUserWritesCriticalSectionsNamespace.dbName());
            ASSERT_BSONOBJ_EQ(
                request.cmdObj,
                BSON("delete" << NamespaceString::kUserWritesCriticalSectionsNamespace.coll()
                              << "bypassDocumentValidation" << false << "ordered" << true
                              << "deletes"
                              << BSON_ARRAY(BSON("q" << BSON("_id" << "doc1") << "limit" << 1)
                                            << BSON("q" << BSON("_id" << "doc2") << "limit" << 1))
                              << "writeConcern"
                              << BSON("w" << "majority"
                                          << "wtimeout" << 60000)));
            ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(), request.metadata);

            return BSON("ok" << 1);
        });
    }

    void expectClusterParametersPullRequest(
        const HostAndPort& target,
        const std::vector<boost::optional<TenantId>>& tenantsOnTarget = {boost::none}) {
        std::vector<DatabaseName> dbnamesOnTarget;
        for (const auto& tenantId : tenantsOnTarget) {
            dbnamesOnTarget.push_back(DatabaseName::createDatabaseName_forTest(
                tenantId, DatabaseName::kConfig.db(omitTenant)));
        }

        int n = dbnamesOnTarget.size();
        auto serializationCtx = SerializationContext::stateCommandReply();
        serializationCtx.setPrefixState(false);
        while (n-- > 0) {
            onCommandForAddShard([&](const RemoteCommandRequest& request) {
                ASSERT_EQ(request.target, target);
                auto it = std::find(dbnamesOnTarget.begin(), dbnamesOnTarget.end(), request.dbname);
                ASSERT(it != dbnamesOnTarget.end());
                dbnamesOnTarget.erase(it);
                ASSERT_BSONOBJ_EQ(request.cmdObj,
                                  BSON("find" << NamespaceString::kClusterParametersNamespace.coll()
                                              << "maxTimeMS" << 60000 << "readConcern"
                                              << BSON("level" << "majority")));
                auto cursorRes = CursorResponse(
                    NamespaceString::createNamespaceString_forTest(
                        request.dbname, NamespaceString::kClusterParametersNamespace.coll()),
                    0,
                    {BSON("_id" << "testStrClusterParameter"
                                << "strData" << request.dbname.toStringWithTenantId_forTest())});
                return cursorRes.toBSON(CursorResponse::ResponseType::InitialResponse,
                                        serializationCtx);
            });
        }
    }

    void checkLocalClusterParametersAfterPull(
        const std::vector<boost::optional<TenantId>>& tenantsOnTarget = {boost::none}) {
        DBDirectClient client(operationContext());
        for (const auto& tenantId : tenantsOnTarget) {
            auth::ValidatedTenancyScopeGuard::runAsTenant(operationContext(), tenantId, [&]() {
                FindCommandRequest findCmd(NamespaceString::makeClusterParametersNSS(tenantId));
                auto cursor = client.find(std::move(findCmd));
                std::vector<BSONObj> results;
                while (cursor->more()) {
                    results.push_back(cursor->next());
                }
                ASSERT_EQ(results.size(), 1);
                ASSERT_EQ(results[0]["_id"].String(), "testStrClusterParameter");
                ASSERT_EQ(results[0]["strData"].String(),
                          DatabaseName::createDatabaseName_forTest(
                              tenantId, DatabaseName::kConfig.db(omitTenant))
                              .toStringWithTenantId_forTest());
            });
        }
    }

    void expectClusterTimeKeysPullRequest(const HostAndPort& target) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_BSONOBJ_EQ(request.cmdObj,
                              BSON("find" << NamespaceString::kKeysCollectionNamespace.coll()
                                          << "maxTimeMS" << 60000 << "readConcern"
                                          << BSON("level" << "local")));

            KeysCollectionDocument key(1);
            key.setKeysCollectionDocumentBase(
                {"dummy", TimeProofService::generateRandomKey(), LogicalTime(Timestamp(105, 0))});
            auto cursorRes = CursorResponse(
                NamespaceString::createNamespaceString_forTest(
                    request.dbname, NamespaceString::kKeysCollectionNamespace.coll()),
                0,
                {key.toBSON()});
            return cursorRes.toBSON(CursorResponse::ResponseType::InitialResponse);
        });
    }

    /**
     * Waits for a request for the shardIdentity document to be upserted into a shard from the
     * config server on addShard.
     */
    void expectAddShardCmdReturnSuccess(const HostAndPort& expectedHost,
                                        const std::string& expectedShardName) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(expectedHost, request.target);

            const auto expectedShardIdentity =
                topology_change_helpers::createShardIdentity(operationContext(), expectedShardName);

            const auto addShardOpMsgRequest = static_cast<OpMsgRequest>(request);
            const auto addShardCmd = ShardsvrAddShard::parse(
                addShardOpMsgRequest, IDLParserContext(ShardsvrAddShard::kCommandName));

            ASSERT_EQ(
                0,
                expectedShardIdentity.toBSON().woCompare(addShardCmd.getShardIdentity().toBSON()));

            BatchedCommandResponse response;
            response.setStatus(Status::OK());
            response.setNModified(1);

            return response.toBSON();
        });
    }

    /**
     * Asserts that a document exists in the config server's config.shards collection corresponding
     * to 'expectedShard'.
     */
    void assertShardExists(const ShardType& expectedShard) {
        auto foundShard = assertGet(getShardDoc(operationContext(), expectedShard.getName()));

        ASSERT_EQUALS(expectedShard.getName(), foundShard.getName());
        ASSERT_EQUALS(expectedShard.getHost(), foundShard.getHost());
        ASSERT_EQUALS(expectedShard.getDraining(), foundShard.getDraining());
        ASSERT_EQUALS((int)expectedShard.getState(), (int)foundShard.getState());
        ASSERT_TRUE(foundShard.getTags().empty());
    }

    /**
     * Asserts that a document exists in the config server's config.databases collection
     * corresponding to 'expectedDB'.
     */
    void assertDatabaseExists(const DatabaseType& expectedDB) {
        auto foundDB = catalogClient()->getDatabase(operationContext(),
                                                    expectedDB.getDbName(),
                                                    repl::ReadConcernLevel::kMajorityReadConcern);
        ASSERT_EQUALS(expectedDB.getDbName(), foundDB.getDbName());
        ASSERT_EQUALS(expectedDB.getPrimary(), foundDB.getPrimary());
    }

    /**
     * Asserts that a document exists in the config server's config.changelog collection
     * describing the addShard request for 'addedShard'.
     */
    void assertChangeWasLogged(const ShardType& addedShard) {
        auto response = assertGet(getConfigShard()->exhaustiveFindOnConfig(
            operationContext(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            NamespaceString::createNamespaceString_forTest("config.changelog"),
            BSON("what" << "addShard"
                        << "details.name" << addedShard.getName()),
            BSONObj(),
            1));
        ASSERT_EQ(1U, response.docs.size());
        auto logEntryBSON = response.docs.front();
        auto logEntry = assertGet(ChangeLogType::fromBSON(logEntryBSON));

        ASSERT_EQUALS(addedShard.getName(), logEntry.getDetails()["name"].String());
        ASSERT_EQUALS(addedShard.getHost(), logEntry.getDetails()["host"].String());
    }

    // TODO (SERVER-100309): move into SuccessfullyAddConfigShard test once 9.0 becomes LastLTS.
    void runSuccessfulConfigShardTest(bool expectDropSessionsCollection) {
        std::unique_ptr<RemoteCommandTargeterMock> targeter(
            std::make_unique<RemoteCommandTargeterMock>());
        ConnectionString connString =
            assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
        targeter->setConnectionStringReturnValue(connString);
        HostAndPort shardTarget = connString.getServers().front();
        targeter->setFindHostReturnValue(shardTarget);
        targeterFactory()->addTargeterToReturn(connString, std::move(targeter));

        std::string expectedShardName = "mySet";

        // The shard doc inserted into the config.shards collection on the config server.
        ShardType expectedShard;
        expectedShard.setName(expectedShardName);
        expectedShard.setHost(connString.toString());
        expectedShard.setState(ShardType::ShardState::kShardAware);

        DatabaseType discoveredDB(DatabaseName::createDatabaseName_forTest(boost::none, "shardDB"),
                                  ShardId(expectedShardName),
                                  DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

        auto future = launchAsync([this, &expectedShardName, &connString] {
            ThreadClient tc(getServiceContext()->getService());
            auto opCtx = Client::getCurrent()->makeOperationContext();
            auto shardName = assertGet(ShardingCatalogManager::get(opCtx.get())
                                           ->addShard(opCtx.get(),
                                                      FixedFCVRegion(opCtx.get()),
                                                      nullptr,
                                                      connString,
                                                      true /* isConfigShard */));
            ASSERT_EQUALS(expectedShardName, shardName);
        });

        BSONArrayBuilder hosts;
        hosts.append("host1:12345");
        hosts.append("host2:12345");
        BSONObj commandResponse = BSON("ok" << 1 << "isWritablePrimary" << true << "setName"
                                            << "mySet"
                                            << "hosts" << hosts.arr() << "maxWireVersion"
                                            << WireVersion::LATEST_WIRE_VERSION);
        expectHello(shardTarget, commandResponse);
        expectHello(shardTarget, commandResponse);

        // Get databases list from new shard
        expectListDatabases(
            shardTarget,
            std::vector<BSONObj>{BSON("name" << discoveredDB.getDbName().toString_forTest())});

        if (expectDropSessionsCollection) {
            expectCollectionDrop(shardTarget, NamespaceString::kLogicalSessionsNamespace);
        }

        // Should not run _addShard command, touch user_writes_critical_sections, setParameter,
        // setFCV

        // Wait for the addShard to complete before checking the config database
        future.timed_get(kLongFutureTimeout);

        // Ensure that the shard document was properly added to config.shards.
        assertShardExists(expectedShard);

        // Ensure that the databases detected from the shard were properly added to config.database.
        assertDatabaseExists(discoveredDB);

        assertChangeWasLogged(expectedShard);
    }

    OID _clusterId;

    ReadWriteConcernDefaultsLookupMock _lookupMock;

    FailPoint* _skipShardingEventNotificationFP;
    FailPoint* _skipUpdatingCardinalityParamFP;
    FailPoint* _skipBlockingDDLCoordinatorsDuringAddAndRemoveShardFP;
};

TEST_F(AddShardTest, AddSCCCConnectionStringAsShard) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    auto invalidConn = ConnectionString("host1:12345,host2:12345,host3:12345",
                                        ConnectionString::ConnectionType::kInvalid);
    targeter->setConnectionStringReturnValue(invalidConn);

    auto future = launchAsync([this, invalidConn] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        const std::string shardName("StandaloneShard");
        auto status =
            ShardingCatalogManager::get(opCtx.get())
                ->addShard(
                    opCtx.get(), FixedFCVRegion(opCtx.get()), &shardName, invalidConn, false);
        ASSERT_EQUALS(ErrorCodes::BadValue, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "Invalid connection string");
    });

    future.timed_get(kLongFutureTimeout);
}

TEST_F(AddShardTest, EmptyShardName) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    std::string expectedShardName = "";

    auto future = launchAsync([this, expectedShardName] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     &expectedShardName,
                                     assertGet(ConnectionString::parse("StandaloneHost:12345")),
                                     false);
        ASSERT_EQUALS(ErrorCodes::BadValue, status);
        ASSERT_EQUALS("shard name cannot be empty", status.getStatus().reason());
    });

    future.timed_get(kLongFutureTimeout);
}

// Host is unreachable, cannot verify host.
TEST_F(AddShardTest, UnreachableHost) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, &expectedShardName, &shardTarget] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     &expectedShardName,
                                     ConnectionString(shardTarget),
                                     false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "host unreachable");
    });

    Status hostUnreachableStatus = Status(ErrorCodes::HostUnreachable, "host unreachable");
    expectHello(shardTarget, hostUnreachableStatus);

    future.timed_get(kLongFutureTimeout);
}

// Cannot add mongos as a shard.
TEST_F(AddShardTest, AddMongosAsShard) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, &expectedShardName, &shardTarget] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     &expectedShardName,
                                     ConnectionString(shardTarget),
                                     false);
        ASSERT_EQUALS(ErrorCodes::IllegalOperation, status);
    });

    expectHello(shardTarget, BSON("msg" << "isdbgrid"));

    future.timed_get(kLongFutureTimeout);
}

// A replica set name was found for the host but no name was provided with the host.
TEST_F(AddShardTest, AddReplicaSetShardAsStandalone) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget = HostAndPort("host1:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));
    std::string expectedShardName = "Standalone";

    auto future = launchAsync([this, expectedShardName, shardTarget] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     &expectedShardName,
                                     ConnectionString(shardTarget),
                                     false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "use replica set url format");
    });

    BSONObj commandResponse = BSON("ok" << 1 << "isWritablePrimary" << true << "setName"
                                        << "myOtherSet"
                                        << "maxWireVersion" << WireVersion::LATEST_WIRE_VERSION);
    expectHello(shardTarget, commandResponse);

    future.timed_get(kLongFutureTimeout);
}

// A replica set name was provided with the host but no name was found for the host.
TEST_F(AddShardTest, AddStandaloneHostShardAsReplicaSet) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setConnectionStringReturnValue(connString);
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, connString] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     &expectedShardName,
                                     connString,
                                     false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "host did not return a set name");
    });

    BSONObj commandResponse = BSON("ok" << 1 << "isWritablePrimary" << true << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectHello(shardTarget, commandResponse);

    future.timed_get(kLongFutureTimeout);
}

// Provided replica set name does not match found replica set name.
TEST_F(AddShardTest, ReplicaSetMistmatchedReplicaSetName) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, connString] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     &expectedShardName,
                                     connString,
                                     false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "does not match the actual set name");
    });

    BSONObj commandResponse = BSON("ok" << 1 << "isWritablePrimary" << true << "setName"
                                        << "myOtherSet"
                                        << "maxWireVersion" << WireVersion::LATEST_WIRE_VERSION);
    expectHello(shardTarget, commandResponse);

    future.timed_get(kLongFutureTimeout);
}

// Cannot add config server as a shard.
TEST_F(AddShardTest, ShardIsCSRSConfigServer) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("config/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, connString] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     &expectedShardName,
                                     connString,
                                     false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(),
                               "as a shard since it is a config server");
    });

    BSONObj commandResponse =
        BSON("ok" << 1 << "isWritablePrimary" << true << "setName"
                  << "config"
                  << "configsvr" << true << "maxWireVersion" << WireVersion::LATEST_WIRE_VERSION);
    expectHello(shardTarget, commandResponse);

    future.timed_get(kLongFutureTimeout);
}

// One of the hosts is not part of the found replica set.
TEST_F(AddShardTest, ReplicaSetMissingHostsProvidedInSeedList) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, connString] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     &expectedShardName,
                                     connString,
                                     false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(),
                               "host2:12345 does not belong to replica set");
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "isWritablePrimary" << true << "setName"
                                        << "mySet"
                                        << "hosts" << hosts.arr() << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectHello(shardTarget, commandResponse);

    future.timed_get(kLongFutureTimeout);
}

// Cannot add a shard with the shard name "config".
TEST_F(AddShardTest, AddShardWithNameConfigFails) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "config";

    auto future = launchAsync([this, expectedShardName, connString] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     &expectedShardName,
                                     connString,
                                     false);
        ASSERT_EQUALS(ErrorCodes::BadValue, status);
        ASSERT_EQUALS(status.getStatus().reason(),
                      "use of shard replica set with name 'config' is not allowed");
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "isWritablePrimary" << true << "setName"
                                        << "mySet"
                                        << "hosts" << hosts.arr() << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectHello(shardTarget, commandResponse);

    future.timed_get(kLongFutureTimeout);
}

TEST_F(AddShardTest, ShardContainsExistingDatabase) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "mySet";

    DatabaseType existingDB(DatabaseName::createDatabaseName_forTest(boost::none, "existing"),
                            ShardId("existingShard"),
                            DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

    // Add a pre-existing database.
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    NamespaceString::kConfigDatabasesNamespace,
                                                    existingDB.toBSON(),
                                                    defaultMajorityWriteConcernDoNotUse()));
    assertDatabaseExists(existingDB);


    auto future = launchAsync([this, expectedShardName, connString] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     &expectedShardName,
                                     connString,
                                     false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(
            status.getStatus().reason(),
            "because a local database 'existing' exists in another existingShard");
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "isWritablePrimary" << true << "setName"
                                        << "mySet"
                                        << "hosts" << hosts.arr() << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectHello(shardTarget, commandResponse);

    expectListDatabases(shardTarget, {BSON("name" << existingDB.getDbName().toString_forTest())});

    future.timed_get(kLongFutureTimeout);
}

TEST_F(AddShardTest, SuccessfullyAddReplicaSet) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);
    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));

    std::string expectedShardName = "mySet";

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost(connString.toString());
    expectedShard.setState(ShardType::ShardState::kShardAware);

    DatabaseType discoveredDB(DatabaseName::createDatabaseName_forTest(boost::none, "shardDB"),
                              ShardId(expectedShardName),
                              DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

    auto future = launchAsync([this, &expectedShardName, &connString] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName = assertGet(
            ShardingCatalogManager::get(opCtx.get())
                ->addShard(opCtx.get(), FixedFCVRegion(opCtx.get()), nullptr, connString, false));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "isWritablePrimary" << true << "setName"
                                        << "mySet"
                                        << "hosts" << hosts.arr() << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectHello(shardTarget, commandResponse);
    expectHello(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(
        shardTarget,
        std::vector<BSONObj>{BSON("name" << discoveredDB.getDbName().toString_forTest())});

    expectCollectionDrop(shardTarget, NamespaceString::kLogicalSessionsNamespace);

    // The shard receives a find to pull all clusterTime keys from the new shard.
    expectClusterTimeKeysPullRequest(shardTarget);

    // The shard receives the _addShard command
    expectAddShardCmdReturnSuccess(shardTarget, expectedShardName);

    // The shard receives a delete op to clear any leftover user_writes_critical_sections doc.
    expectRemoveUserWritesCriticalSectionsDocs(shardTarget);

    // The shard receives a find to pull all cluster parameters.
    expectClusterParametersPullRequest(shardTarget);

    // The shard receives the setFeatureCompatibilityVersion command.
    expectSetFeatureCompatibilityVersion(
        shardTarget, BSON("ok" << 1), operationContext()->getWriteConcern());

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was properly added to config.shards.
    assertShardExists(expectedShard);

    // Ensure that the databases detected from the shard were properly added to config.database.
    assertDatabaseExists(discoveredDB);

    assertChangeWasLogged(expectedShard);

    checkLocalClusterParametersAfterPull();
}

// TODO (SERVER-100309): remove once 9.0 becomes last LTS.
TEST_F(AddShardTest, SuccessfullyAddConfigShardOldPath) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagSessionsCollectionCoordinatorOnConfigServer", true);
    runSuccessfulConfigShardTest(false);
}

TEST_F(AddShardTest, SuccessfullyAddConfigShard) {
    runSuccessfulConfigShardTest(true);
}

TEST_F(AddShardTest, ReplicaSetExtraHostsDiscovered) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    ConnectionString seedString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    ConnectionString fullConnString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345,host3:12345"));
    targeter->setConnectionStringReturnValue(fullConnString);
    HostAndPort shardTarget = seedString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);
    targeterFactory()->addTargeterToReturn(seedString, std::move(targeter));

    std::string expectedShardName = "mySet";

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost(fullConnString.toString());
    expectedShard.setState(ShardType::ShardState::kShardAware);

    DatabaseType discoveredDB(DatabaseName::createDatabaseName_forTest(boost::none, "shardDB"),
                              ShardId(expectedShardName),
                              DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

    auto future = launchAsync([this, &expectedShardName, &seedString] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName = assertGet(
            ShardingCatalogManager::get(opCtx.get())
                ->addShard(opCtx.get(), FixedFCVRegion(opCtx.get()), nullptr, seedString, false));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "isWritablePrimary" << true << "setName"
                                        << "mySet"
                                        << "hosts" << hosts.arr() << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectHello(shardTarget, commandResponse);
    expectHello(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(
        shardTarget,
        std::vector<BSONObj>{BSON("name" << discoveredDB.getDbName().toString_forTest())});

    expectCollectionDrop(shardTarget, NamespaceString::kLogicalSessionsNamespace);

    // The shard receives a find to pull all clusterTime keys from the new shard.
    expectClusterTimeKeysPullRequest(shardTarget);

    // The shard receives the _addShard command
    expectAddShardCmdReturnSuccess(shardTarget, expectedShardName);

    // The shard receives a delete op to clear any leftover user_writes_critical_sections doc.
    expectRemoveUserWritesCriticalSectionsDocs(shardTarget);

    // The shard receives a find to pull all cluster parameters.
    expectClusterParametersPullRequest(shardTarget);

    // The shard receives the setFeatureCompatibilityVersion command.
    expectSetFeatureCompatibilityVersion(
        shardTarget, BSON("ok" << 1), operationContext()->getWriteConcern());

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was properly added to config.shards.
    assertShardExists(expectedShard);

    // Ensure that the databases detected from the shard were properly added to config.database.
    assertDatabaseExists(discoveredDB);

    // The changelog entry uses whatever connection string is passed to addShard, even if addShard
    // discovered additional hosts.
    expectedShard.setHost(seedString.toString());
    assertChangeWasLogged(expectedShard);

    checkLocalClusterParametersAfterPull();
}

// Tests both that trying to add a shard with the same host as an existing shard but with different
// options fails, and that adding a shard with the same host as an existing shard with the *same*
// options succeeds.
TEST_F(AddShardTest, AddExistingShardStandalone) {
    HostAndPort shardTarget("StandaloneHost:12345");
    std::unique_ptr<RemoteCommandTargeterMock> standaloneTargeter(
        std::make_unique<RemoteCommandTargeterMock>());
    standaloneTargeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    standaloneTargeter->setFindHostReturnValue(shardTarget);
    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget),
                                           std::move(standaloneTargeter));

    std::unique_ptr<RemoteCommandTargeterMock> replsetTargeter(
        std::make_unique<RemoteCommandTargeterMock>());
    replsetTargeter->setConnectionStringReturnValue(
        ConnectionString::forReplicaSet("mySet", {shardTarget}));
    replsetTargeter->setFindHostReturnValue(shardTarget);
    targeterFactory()->addTargeterToReturn(ConnectionString::forReplicaSet("mySet", {shardTarget}),
                                           std::move(replsetTargeter));

    std::string existingShardName = "myShard";
    ShardType existingShard;
    existingShard.setName(existingShardName);
    existingShard.setHost(shardTarget.toString());
    existingShard.setState(ShardType::ShardState::kShardAware);

    // Make sure the shard already exists.
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    NamespaceString::kConfigsvrShardsNamespace,
                                                    existingShard.toBSON(),
                                                    defaultMajorityWriteConcernDoNotUse()));
    assertShardExists(existingShard);

    // Adding the same standalone host with a different shard name should fail.
    std::string differentName = "anotherShardName";
    auto future1 = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     &differentName,
                                     ConnectionString(shardTarget),
                                     false));
    });
    future1.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding the same standalone host but as part of a replica set should fail.
    // Ensures that even if the user changed the standalone shard to a single-node replica set, you
    // can't change the sharded cluster's notion of the shard from standalone to replica set just
    // by calling addShard.
    auto future3 = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     nullptr,
                                     ConnectionString::forReplicaSet("mySet", {shardTarget}),
                                     false));
    });
    future3.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding the same standalone host with the same options should succeed.
    auto future4 = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName = assertGet(ShardingCatalogManager::get(opCtx.get())
                                       ->addShard(opCtx.get(),
                                                  FixedFCVRegion(opCtx.get()),
                                                  &existingShardName,
                                                  ConnectionString(shardTarget),
                                                  false));
        ASSERT_EQUALS(existingShardName, shardName);
    });
    future4.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding the same standalone host with the same options (without explicitly specifying the
    // shard name) should succeed.
    auto future5 = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName = assertGet(ShardingCatalogManager::get(opCtx.get())
                                       ->addShard(opCtx.get(),
                                                  FixedFCVRegion(opCtx.get()),
                                                  nullptr,
                                                  ConnectionString(shardTarget),
                                                  false));
        ASSERT_EQUALS(existingShardName, shardName);
    });
    future5.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);
}

// Tests both that trying to add a shard with the same replica set as an existing shard but with
// different options fails, and that adding a shard with the same replica set as an existing shard
// with the *same* options succeeds.
TEST_F(AddShardTest, AddExistingShardReplicaSet) {
    std::unique_ptr<RemoteCommandTargeterMock> replsetTargeter(
        std::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString = assertGet(ConnectionString::parse("mySet/host1:12345"));
    replsetTargeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    replsetTargeter->setFindHostReturnValue(shardTarget);
    targeterFactory()->addTargeterToReturn(connString, std::move(replsetTargeter));

    std::string existingShardName = "myShard";
    ShardType existingShard;
    existingShard.setName(existingShardName);
    existingShard.setHost(connString.toString());
    existingShard.setState(ShardType::ShardState::kShardAware);

    // Make sure the shard already exists.
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    NamespaceString::kConfigsvrShardsNamespace,
                                                    existingShard.toBSON(),
                                                    defaultMajorityWriteConcernDoNotUse()));
    assertShardExists(existingShard);
    // Adding the same connection string with a different shard name should fail.
    std::string differentName = "anotherShardName";
    auto future1 = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(
            ErrorCodes::IllegalOperation,
            ShardingCatalogManager::get(opCtx.get())
                ->addShard(
                    opCtx.get(), FixedFCVRegion(opCtx.get()), &differentName, connString, false));
    });
    future1.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding a different connection string with the same shard name should fail.
    ConnectionString otherHostConnString2 =
        assertGet(ConnectionString::parse("mySet1/host2:12345"));
    auto future2 = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     &existingShardName,
                                     otherHostConnString2,
                                     false));
    });
    future2.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding a connecting string with a host of an existing shard but using a different connection
    // string type should fail.
    // Ensures that even if the user changed the replica set shard to a standalone, you can't change
    // the sharded cluster's notion of the shard from replica set to standalone just by calling
    // addShard.
    auto future3 = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     nullptr,
                                     ConnectionString(shardTarget),
                                     false));
    });
    future3.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding a connecting string with the same hosts but a different replica set name should fail.
    // Ensures that even if you manually change the shard's replica set name somehow, you can't
    // change the replica set name the sharded cluster knows for it just by calling addShard again.
    std::string differentSetName = "differentSet";
    auto future4 = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     FixedFCVRegion(opCtx.get()),
                                     nullptr,
                                     ConnectionString::forReplicaSet(differentSetName,
                                                                     connString.getServers()),
                                     false));
    });
    future4.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding the same host with the same options should succeed.
    auto future5 = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName = assertGet(ShardingCatalogManager::get(opCtx.get())
                                       ->addShard(opCtx.get(),
                                                  FixedFCVRegion(opCtx.get()),
                                                  &existingShardName,
                                                  connString,
                                                  false));
        ASSERT_EQUALS(existingShardName, shardName);
    });
    future5.timed_get(kLongFutureTimeout);

    // Adding the same host with the same options (without explicitly specifying the shard name)
    // should succeed.
    auto future6 = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName = assertGet(
            ShardingCatalogManager::get(opCtx.get())
                ->addShard(opCtx.get(), FixedFCVRegion(opCtx.get()), nullptr, connString, false));
        ASSERT_EQUALS(existingShardName, shardName);
    });
    future6.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding the same replica set but different host membership (but otherwise the same options)
    // should fail.
    auto otherHost = connString.getServers().back();
    ConnectionString otherHostConnString = assertGet(ConnectionString::parse("mySet/host2:12345"));
    {
        // Add a targeter for the different seed string this addShard request will use.
        std::unique_ptr<RemoteCommandTargeterMock> otherHostTargeter(
            std::make_unique<RemoteCommandTargeterMock>());
        otherHostTargeter->setConnectionStringReturnValue(otherHostConnString);
        otherHostTargeter->setFindHostReturnValue(otherHost);
        targeterFactory()->addTargeterToReturn(otherHostConnString, std::move(otherHostTargeter));
    }
    auto future7 = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(
            ErrorCodes::IllegalOperation,
            ShardingCatalogManager::get(opCtx.get())
                ->addShard(
                    opCtx.get(), FixedFCVRegion(opCtx.get()), nullptr, otherHostConnString, false));
    });
    future7.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);
}

// Tests both that trying to add a shard with a different replica set as an existing shard but with
// overlapping hosts fails, and that adding a shard with the same replica set as an existing shard
// with overlapping hosts succeeds.
TEST_F(AddShardTest, AddShardWithOverlappingHosts) {
    std::unique_ptr<RemoteCommandTargeterMock> replsetTargeter(
        std::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345,host3:12345"));
    replsetTargeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    replsetTargeter->setFindHostReturnValue(shardTarget);
    targeterFactory()->addTargeterToReturn(connString, std::move(replsetTargeter));

    std::string existingShardName = "myShard";
    ShardType existingShard;
    existingShard.setName(existingShardName);
    existingShard.setHost(connString.toString());
    existingShard.setState(ShardType::ShardState::kShardAware);

    // Make sure the shard already exists.
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    NamespaceString::kConfigsvrShardsNamespace,
                                                    existingShard.toBSON(),
                                                    defaultMajorityWriteConcernDoNotUse()));
    assertShardExists(existingShard);

    // Adding a shard with a different replica set name but with some common hosts should fail.
    auto otherHost = connString.getServers().front();
    ConnectionString otherHostConnString =
        assertGet(ConnectionString::parse("mySet1/host1:12345,host2:12345,host4:12345"));
    {
        // Add a targeter for the different seed string this addShard request will use.
        std::unique_ptr<RemoteCommandTargeterMock> otherHostTargeter(
            std::make_unique<RemoteCommandTargeterMock>());
        otherHostTargeter->setConnectionStringReturnValue(otherHostConnString);
        otherHostTargeter->setFindHostReturnValue(otherHost);
        targeterFactory()->addTargeterToReturn(otherHostConnString, std::move(otherHostTargeter));
    }
    auto future1 = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(
            ErrorCodes::IllegalOperation,
            ShardingCatalogManager::get(opCtx.get())
                ->addShard(
                    opCtx.get(), FixedFCVRegion(opCtx.get()), nullptr, otherHostConnString, false));
    });
    future1.timed_get(kLongFutureTimeout);
    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding a shard with the same replica set name and some common hosts should pass.
    ConnectionString otherHostConnString1 =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345,host4:12345"));
    {
        // Add a targeter for the different seed string this addShard request will use.
        std::unique_ptr<RemoteCommandTargeterMock> otherHostTargeter(
            std::make_unique<RemoteCommandTargeterMock>());
        otherHostTargeter->setConnectionStringReturnValue(otherHostConnString1);
        otherHostTargeter->setFindHostReturnValue(otherHost);
        targeterFactory()->addTargeterToReturn(otherHostConnString1, std::move(otherHostTargeter));
    }
    auto future2 = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName = assertGet(ShardingCatalogManager::get(opCtx.get())
                                       ->addShard(opCtx.get(),
                                                  FixedFCVRegion(opCtx.get()),
                                                  &existingShardName,
                                                  otherHostConnString1,
                                                  false));
        ASSERT_EQUALS(existingShardName, shardName);
    });
    future2.timed_get(kLongFutureTimeout);
    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);
}

}  // namespace
}  // namespace mongo
