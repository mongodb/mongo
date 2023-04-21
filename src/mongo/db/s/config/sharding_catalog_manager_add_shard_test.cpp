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


#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/set_cluster_parameter_invocation.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/add_shard_cmd_gen.h"
#include "mongo/db/s/add_shard_util.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/idl/cluster_server_parameter_common.h"
#include "mongo/idl/cluster_server_parameter_gen.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/database_version.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
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
        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());
        // Create config.transactions collection
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->onShardingInitialization(operationContext(), true);

        _skipShardingEventNotificationFP =
            globalFailPointRegistry().find("shardingCatalogManagerSkipNotifyClusterOnNewDatabases");
        _skipShardingEventNotificationFP->setMode(FailPoint::alwaysOn);
    }

    void tearDown() override {
        _skipShardingEventNotificationFP->setMode(FailPoint::off);
        TransactionCoordinatorService::get(operationContext())->onStepDown();
        ConfigServerTestFixture::tearDown();
    }

    /**
     * addShard validates the host as a shard. It calls "isMaster" on the host to determine what
     * kind of host it is -- mongos, regular mongod, config mongod -- and whether the replica set
     * details are correct. "isMasterResponse" defines the response of the "isMaster" request and
     * should be a command response BSONObj, or a failed Status.
     *
     * ShardingTestFixture::expectGetShards() should be called before this function, otherwise
     * addShard will never reach the isMaster command -- a find query is called first.
     */
    void expectIsMaster(const HostAndPort& target, StatusWith<BSONObj> isMasterResponse) {
        onCommandForAddShard([&, target, isMasterResponse](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, "admin");
            ASSERT_BSONOBJ_EQ(request.cmdObj, BSON("isMaster" << 1));
            ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(), request.metadata);

            return isMasterResponse;
        });
    }

    void expectListDatabases(const HostAndPort& target, const std::vector<BSONObj>& dbs) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, "admin");
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
            ASSERT_EQ(request.dbname, nss.db());
            ASSERT_BSONOBJ_EQ(request.cmdObj,
                              BSON("drop" << nss.coll() << "writeConcern"
                                          << BSON("w"
                                                  << "majority")));
            ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(), request.metadata);

            return BSON("ok" << 1);
        });
    }

    void expectSetFeatureCompatibilityVersion(const HostAndPort& target,
                                              StatusWith<BSONObj> response,
                                              BSONObj writeConcern) {
        // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
        SetFeatureCompatibilityVersion fcvCmd(multiversion::GenericFCV::kLatest);
        fcvCmd.setFromConfigServer(true);
        fcvCmd.setDbName(DatabaseName::kAdmin);
        const auto setFcvObj = fcvCmd.toBSON(BSON("writeConcern" << writeConcern));

        onCommandForAddShard([&, target, response](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, "admin");
            ASSERT_BSONOBJ_EQ(request.cmdObj, setFcvObj);

            return response;
        });
    }

    void expectRemoveUserWritesCriticalSectionsDocs(const HostAndPort& target) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, NamespaceString::kUserWritesCriticalSectionsNamespace.db());
            ASSERT_BSONOBJ_EQ(
                request.cmdObj,
                BSON("delete" << NamespaceString::kUserWritesCriticalSectionsNamespace.coll()
                              << "bypassDocumentValidation" << false << "ordered" << true
                              << "deletes" << BSON_ARRAY(BSON("q" << BSONObj() << "limit" << 0))
                              << "writeConcern"
                              << BSON("w"
                                      << "majority"
                                      << "wtimeout" << 60000)));
            ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(), request.metadata);

            return BSON("ok" << 1);
        });
    }

    void expectClusterParametersPushRequest(
        const HostAndPort& target,
        const std::vector<boost::optional<TenantId>>& tenantsOnTarget = {boost::none},
        const TenantIdMap<std::vector<BSONObj>>& localClusterParameters = {}) {
        expectClusterParametersRequest(target, true, tenantsOnTarget, localClusterParameters);
    }

    void expectClusterParametersPullRequest(
        const HostAndPort& target,
        const std::vector<boost::optional<TenantId>>& tenantsOnTarget = {boost::none}) {
        expectClusterParametersRequest(target, false, tenantsOnTarget, {});
    }

    void expectClusterParametersRequest(
        const HostAndPort& target,
        bool isPush,
        const std::vector<boost::optional<TenantId>>& tenantsOnTarget,
        const TenantIdMap<std::vector<BSONObj>>& localClusterParameters) {

        std::vector<std::string> dbnamesOnTarget;
        for (const auto& tenantId : tenantsOnTarget) {
            dbnamesOnTarget.push_back(
                DatabaseName::createDatabaseName_forTest(tenantId, DatabaseName::kConfig.db())
                    .toStringWithTenantId());
        }

        if (gMultitenancySupport) {
            // If in multitenancy mode, we expect to run a listDatabasesForAllTenants in order to
            // get tenant list.
            expectListDatabasesForAllTenants(target, tenantsOnTarget);
        }

        if (isPush) {
            expectRemoveClusterParameterDocs(target, dbnamesOnTarget);
            expectInsertClusterParameterDocs(target, localClusterParameters);
        } else {
            expectFindClusterParameterDocs(target, dbnamesOnTarget);
        }
    }

    void checkLocalClusterParametersAfterPull(
        const std::vector<boost::optional<TenantId>>& tenantsOnTarget = {boost::none}) {
        DBDirectClient client(operationContext());
        for (const auto& tenantId : tenantsOnTarget) {
            FindCommandRequest findCmd(NamespaceString::makeClusterParametersNSS(tenantId));
            auto cursor = client.find(std::move(findCmd));
            std::vector<BSONObj> results;
            while (cursor->more()) {
                results.push_back(cursor->next());
            }
            ASSERT_EQ(results.size(), 1);
            ASSERT_EQ(results[0]["_id"].String(), "testStrClusterParameter");
            ASSERT_EQ(results[0]["strData"].String(),
                      DatabaseName::createDatabaseName_forTest(tenantId, DatabaseName::kConfig.db())
                          .toStringWithTenantId());
        }
    }

    void expectListDatabasesForAllTenants(
        const HostAndPort& target, const std::vector<boost::optional<TenantId>>& tenantsOnTarget) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, DatabaseName::kAdmin.db());
            ASSERT_EQ(request.cmdObj["listDatabasesForAllTenants"].Int(), 1);
            BSONArrayBuilder b;
            for (const auto& tenantId : tenantsOnTarget) {
                if (tenantId) {
                    b.append(BSON("name"
                                  << "config"
                                  << "tenantId" << OID(tenantId->toString())));
                } else {
                    b.append(BSON("name"
                                  << "config"));
                }
            }
            return BSON("ok" << 1 << "databases" << b.done());
        });
    }

    void expectRemoveClusterParameterDocs(const HostAndPort& target,
                                          std::vector<std::string> dbnamesOnTarget) {
        int n = dbnamesOnTarget.size();
        while (n-- > 0) {
            onCommandForAddShard([&](const RemoteCommandRequest& request) {
                ASSERT_EQ(request.target, target);
                auto it = std::find(dbnamesOnTarget.begin(), dbnamesOnTarget.end(), request.dbname);
                ASSERT(it != dbnamesOnTarget.end());
                dbnamesOnTarget.erase(it);
                ASSERT_BSONOBJ_EQ(
                    request.cmdObj,
                    BSON("delete" << NamespaceString::kClusterParametersNamespace.coll()
                                  << "bypassDocumentValidation" << false << "ordered" << true
                                  << "deletes" << BSON_ARRAY(BSON("q" << BSONObj() << "limit" << 0))
                                  << "writeConcern"
                                  << BSON("w"
                                          << "majority"
                                          << "wtimeout" << 60000)));
                ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(), request.metadata);

                return BSON("ok" << 1);
            });
        }
    }

    void addLocalClusterParameters(TenantIdMap<std::vector<BSONObj>> localClusterParameters) {
        for (const auto& [tenantId, params] : localClusterParameters) {
            for (auto& param : params) {
                SetClusterParameter setClusterParameterRequest(param);
                setClusterParameterRequest.setDbName(
                    DatabaseName::createDatabaseName_forTest(tenantId, DatabaseName::kAdmin.db()));
                DBDirectClient client(operationContext());
                ClusterParameterDBClientService dbService(client);
                std::unique_ptr<ServerParameterService> parameterService =
                    std::make_unique<ClusterParameterService>();
                SetClusterParameterInvocation invocation{std::move(parameterService), dbService};
                invocation.invoke(operationContext(),
                                  setClusterParameterRequest,
                                  boost::none,
                                  ShardingCatalogClient::kLocalWriteConcern);
            }
        }
    }

    void expectInsertClusterParameterDocs(
        const HostAndPort& target, TenantIdMap<std::vector<BSONObj>> localClusterParameters) {
        int64_t n = std::accumulate(localClusterParameters.begin(),
                                    localClusterParameters.end(),
                                    0,
                                    [](size_t accumulator, const auto& tenantParams) {
                                        return accumulator + tenantParams.second.size();
                                    });
        while (n-- > 0) {
            onCommandForAddShard([&](const RemoteCommandRequest& request) {
                ASSERT_EQ(request.target, target);
                int idx = request.dbname.find('_');
                boost::optional<TenantId> tenantId = boost::none;
                if (idx == OID::kOIDSize * 2) {
                    // tenantId exists
                    tenantId = TenantId(OID(request.dbname.substr(0, OID::kOIDSize * 2)));
                }

                // Check that the parameter and tenant match a parameter and tenant passed in.
                auto it = localClusterParameters.find(tenantId);
                ASSERT(it != localClusterParameters.end());

                auto paramToSet = request.cmdObj["_shardsvrSetClusterParameter"].Obj();
                auto itInner =
                    std::find_if(it->second.begin(), it->second.end(), [&](const BSONObj& obj) {
                        return obj.woCompare(paramToSet) == 0;
                    });
                ASSERT(itInner != it->second.end());
                it->second.erase(itInner);

                ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(), request.metadata);
                return BSON("ok" << 1);
            });
        }
    }

    void expectFindClusterParameterDocs(const HostAndPort& target,
                                        std::vector<std::string> dbnamesOnTarget) {
        int n = dbnamesOnTarget.size();
        while (n-- > 0) {
            onCommandForAddShard([&](const RemoteCommandRequest& request) {
                ASSERT_EQ(request.target, target);
                auto it = std::find(dbnamesOnTarget.begin(), dbnamesOnTarget.end(), request.dbname);
                ASSERT(it != dbnamesOnTarget.end());
                dbnamesOnTarget.erase(it);
                ASSERT_BSONOBJ_EQ(request.cmdObj,
                                  BSON("find" << NamespaceString::kClusterParametersNamespace.coll()
                                              << "maxTimeMS" << 30000 << "readConcern"
                                              << BSON("level"
                                                      << "majority")));
                auto cursorRes =
                    CursorResponse(
                        NamespaceString::createNamespaceString_forTest(
                            request.dbname, NamespaceString::kClusterParametersNamespace.coll()),
                        0,
                        {BSON("_id"
                              << "testStrClusterParameter"
                              << "strData" << request.dbname)});
                return cursorRes.toBSON(CursorResponse::ResponseType::InitialResponse);
            });
        }
    }

    /**
     * Waits for a request for the shardIdentity document to be upserted into a shard from the
     * config server on addShard.
     */
    void expectAddShardCmdReturnSuccess(const HostAndPort& expectedHost,
                                        const std::string& expectedShardName) {
        using namespace add_shard_util;
        // Create the expected upsert shardIdentity command for this shardType.
        auto upsertCmdObj = createShardIdentityUpsertForAddShard(
            createAddShardCmd(operationContext(), expectedShardName),
            ShardingCatalogClient::kMajorityWriteConcern);

        const auto opMsgRequest =
            OpMsgRequest::fromDBAndBody(DatabaseName::kAdmin.db(), upsertCmdObj);
        expectUpdatesReturnSuccess(expectedHost,
                                   NamespaceString(NamespaceString::kServerConfigurationNamespace),
                                   UpdateOp::parse(opMsgRequest));
    }

    void expectAddShardCmdReturnFailure(const HostAndPort& expectedHost,
                                        const std::string& expectedShardName,
                                        const Status& statusToReturn) {
        using namespace add_shard_util;
        // Create the expected upsert shardIdentity command for this shardType.
        auto upsertCmdObj = createShardIdentityUpsertForAddShard(
            createAddShardCmd(operationContext(), expectedShardName),
            ShardingCatalogClient::kMajorityWriteConcern);

        const auto opMsgRequest =
            OpMsgRequest::fromDBAndBody(DatabaseName::kAdmin.db(), upsertCmdObj);
        expectUpdatesReturnFailure(expectedHost,
                                   NamespaceString(NamespaceString::kServerConfigurationNamespace),
                                   UpdateOp::parse(opMsgRequest),
                                   statusToReturn);
    }

    /**
     * Waits for a set of batched updates and ensures that the host, namespace, and updates exactly
     * match what's expected. Responds with a success status.
     */
    void expectUpdatesReturnSuccess(const HostAndPort& expectedHost,
                                    const NamespaceString& expectedNss,
                                    const write_ops::UpdateCommandRequest& expectedUpdateOp) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(expectedHost, request.target);

            // Check that the db name in the request matches the expected db name.
            ASSERT_EQUALS(expectedNss.db(), request.dbname);

            const auto addShardOpMsgRequest =
                OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);

            auto addShardCmd =
                AddShard::parse(IDLParserContext(AddShard::kCommandName), addShardOpMsgRequest);

            const auto& updateOpField = add_shard_util::createShardIdentityUpsertForAddShard(
                addShardCmd, ShardingCatalogClient::kMajorityWriteConcern);

            const auto updateOpMsgRequest =
                OpMsgRequest::fromDBAndBody(request.dbname, updateOpField);

            const auto updateOp = UpdateOp::parse(updateOpMsgRequest);

            ASSERT_EQUALS(expectedNss, expectedUpdateOp.getNamespace());

            const auto& expectedUpdates = expectedUpdateOp.getUpdates();
            const auto& actualUpdates = updateOp.getUpdates();

            ASSERT_EQUALS(expectedUpdates.size(), actualUpdates.size());

            auto itExpected = expectedUpdates.begin();
            auto itActual = actualUpdates.begin();

            for (; itActual != actualUpdates.end(); itActual++, itExpected++) {
                ASSERT_EQ(itExpected->getUpsert(), itActual->getUpsert());
                ASSERT_EQ(itExpected->getMulti(), itActual->getMulti());
                ASSERT_BSONOBJ_EQ(itExpected->getQ(), itActual->getQ());
                ASSERT_BSONOBJ_EQ(itExpected->getU().getUpdateReplacement(),
                                  itActual->getU().getUpdateReplacement());
            }

            BatchedCommandResponse response;
            response.setStatus(Status::OK());
            response.setNModified(1);

            return response.toBSON();
        });
    }

    /**
     * Waits for a set of batched updates and ensures that the host, namespace, and updates exactly
     * match what's expected. Responds with a failure status.
     */
    void expectUpdatesReturnFailure(const HostAndPort& expectedHost,
                                    const NamespaceString& expectedNss,
                                    const write_ops::UpdateCommandRequest& expectedUpdateOp,
                                    const Status& statusToReturn) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(expectedHost, request.target);

            // Check that the db name in the request matches the expected db name.
            ASSERT_EQUALS(expectedNss.db(), request.dbname);

            const auto opMsgRequest = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
            const auto updateOp = UpdateOp::parse(opMsgRequest);
            ASSERT_EQUALS(expectedNss, expectedUpdateOp.getNamespace());

            const auto& expectedUpdates = expectedUpdateOp.getUpdates();
            const auto& actualUpdates = updateOp.getUpdates();

            ASSERT_EQUALS(expectedUpdates.size(), actualUpdates.size());

            auto itExpected = expectedUpdates.begin();
            auto itActual = actualUpdates.begin();

            for (; itActual != actualUpdates.end(); itActual++, itExpected++) {
                ASSERT_EQ(itExpected->getUpsert(), itActual->getUpsert());
                ASSERT_EQ(itExpected->getMulti(), itActual->getMulti());
                ASSERT_BSONOBJ_EQ(itExpected->getQ(), itActual->getQ());
                ASSERT_BSONOBJ_EQ(itExpected->getU().getUpdateReplacement(),
                                  itActual->getU().getUpdateReplacement());
            }

            return statusToReturn;
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
        auto foundDB = catalogClient()->getDatabase(
            operationContext(), expectedDB.getName(), repl::ReadConcernLevel::kMajorityReadConcern);
        ASSERT_EQUALS(expectedDB.getName(), foundDB.getName());
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
            BSON("what"
                 << "addShard"
                 << "details.name" << addedShard.getName()),
            BSONObj(),
            1));
        ASSERT_EQ(1U, response.docs.size());
        auto logEntryBSON = response.docs.front();
        auto logEntry = assertGet(ChangeLogType::fromBSON(logEntryBSON));

        ASSERT_EQUALS(addedShard.getName(), logEntry.getDetails()["name"].String());
        ASSERT_EQUALS(addedShard.getHost(), logEntry.getDetails()["host"].String());
    }

    void forwardAddShardNetwork(Date_t when) {
        networkForAddShard()->enterNetwork();
        networkForAddShard()->runUntil(when);
        networkForAddShard()->exitNetwork();
    }

    OID _clusterId;

    ReadWriteConcernDefaultsLookupMock _lookupMock;

    FailPoint* _skipShardingEventNotificationFP;
};

TEST_F(AddShardTest, CreateShardIdentityUpsertForAddShard) {
    std::string shardName = "shardName";

    BSONObj expectedBSON =
        BSON("update"
             << "system.version"
             << "bypassDocumentValidation" << false << "ordered" << true << "updates"
             << BSON_ARRAY(BSON(
                    "q" << BSON("_id"
                                << "shardIdentity")
                        << "u"
                        << BSON("shardName"
                                << shardName << "clusterId" << _clusterId
                                << "configsvrConnectionString"
                                << replicationCoordinator()->getConfigConnectionString().toString())
                        << "multi" << false << "upsert" << true))
             << "writeConcern"
             << BSON("w"
                     << "majority"
                     << "wtimeout" << 60000));
    auto addShardCmd = add_shard_util::createAddShardCmd(operationContext(), shardName);
    auto actualBSON = add_shard_util::createShardIdentityUpsertForAddShard(
        addShardCmd, ShardingCatalogClient::kMajorityWriteConcern);
    ASSERT_BSONOBJ_EQ(expectedBSON, actualBSON);
}

TEST_F(AddShardTest, StandaloneBasicSuccess) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));


    std::string expectedShardName = "StandaloneShard";

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost("StandaloneHost:12345");
    expectedShard.setState(ShardType::ShardState::kShardAware);

    DatabaseType discoveredDB1(
        "TestDB1", ShardId("StandaloneShard"), DatabaseVersion(UUID::gen(), Timestamp(1, 1)));
    DatabaseType discoveredDB2(
        "TestDB2", ShardId("StandaloneShard"), DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

    auto expectWriteConcern = ShardingCatalogClient::kMajorityWriteConcern;

    auto future = launchAsync([this, expectedShardName, expectWriteConcern] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        opCtx->setWriteConcern(expectWriteConcern);
        auto shardName =
            assertGet(ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     &expectedShardName,
                                     assertGet(ConnectionString::parse("StandaloneHost:12345")),
                                     false));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(
        shardTarget,
        std::vector<BSONObj>{BSON("name"
                                  << "local"
                                  << "sizeOnDisk" << 1000),
                             BSON("name" << discoveredDB1.getName() << "sizeOnDisk" << 2000),
                             BSON("name" << discoveredDB2.getName() << "sizeOnDisk" << 5000)});

    expectCollectionDrop(
        shardTarget, NamespaceString::createNamespaceString_forTest("config", "system.sessions"));

    // The shard receives the _addShard command
    expectAddShardCmdReturnSuccess(shardTarget, expectedShardName);

    // The shard receives a delete op to clear any leftover user_writes_critical_sections doc.
    expectRemoveUserWritesCriticalSectionsDocs(shardTarget);

    // The shard receives a find to pull all cluster parameters from the new shard into this shard.
    expectClusterParametersPullRequest(shardTarget);

    // The shard receives the setFeatureCompatibilityVersion command.
    expectSetFeatureCompatibilityVersion(shardTarget, BSON("ok" << 1), expectWriteConcern.toBSON());

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was properly added to config.shards.
    assertShardExists(expectedShard);

    // Ensure that the databases detected from the shard were properly added to config.database.
    assertDatabaseExists(discoveredDB1);
    assertDatabaseExists(discoveredDB2);

    assertChangeWasLogged(expectedShard);

    checkLocalClusterParametersAfterPull();
}

TEST_F(AddShardTest, StandaloneBasicPushSuccess) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));

    // Add some cluster parameters to push to the new shard.
    const TenantIdMap<std::vector<BSONObj>>& localClusterParameters = {
        {boost::none,
         {BSON("testIntClusterParameter" << BSON("intData" << 5)),
          BSON("testStrClusterParameter" << BSON("strData"
                                                 << "abc"))}}};
    addLocalClusterParameters(localClusterParameters);

    std::string expectedShardName = "StandaloneShard";

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost("StandaloneHost:12345");
    expectedShard.setState(ShardType::ShardState::kShardAware);

    DatabaseType discoveredDB1(
        "TestDB1", ShardId("StandaloneShard"), DatabaseVersion(UUID::gen(), Timestamp(1, 1)));
    DatabaseType discoveredDB2(
        "TestDB2", ShardId("StandaloneShard"), DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

    auto expectWriteConcern = ShardingCatalogClient::kMajorityWriteConcern;

    auto future = launchAsync([this, expectedShardName, expectWriteConcern] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        opCtx->setWriteConcern(expectWriteConcern);
        auto shardName =
            assertGet(ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     &expectedShardName,
                                     assertGet(ConnectionString::parse("StandaloneHost:12345")),
                                     false));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(
        shardTarget,
        std::vector<BSONObj>{BSON("name"
                                  << "local"
                                  << "sizeOnDisk" << 1000),
                             BSON("name" << discoveredDB1.getName() << "sizeOnDisk" << 2000),
                             BSON("name" << discoveredDB2.getName() << "sizeOnDisk" << 5000)});

    expectCollectionDrop(
        shardTarget, NamespaceString::createNamespaceString_forTest("config", "system.sessions"));

    // The shard receives the _addShard command
    expectAddShardCmdReturnSuccess(shardTarget, expectedShardName);

    // The shard receives a delete op to clear any leftover user_writes_critical_sections doc.
    expectRemoveUserWritesCriticalSectionsDocs(shardTarget);

    // The shard receives a remove to clear its cluster parameters, then inserts to push cluster
    // parameters.
    expectClusterParametersPushRequest(shardTarget, {boost::none}, localClusterParameters);

    // The shard receives the setFeatureCompatibilityVersion command.
    expectSetFeatureCompatibilityVersion(shardTarget, BSON("ok" << 1), expectWriteConcern.toBSON());

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was properly added to config.shards.
    assertShardExists(expectedShard);

    // Ensure that the databases detected from the shard were properly added to config.database.
    assertDatabaseExists(discoveredDB1);
    assertDatabaseExists(discoveredDB2);

    assertChangeWasLogged(expectedShard);
}

TEST_F(AddShardTest, StandaloneMultitenantPullSuccess) {
    gMultitenancySupport = true;
    ScopeGuard guard([] { gMultitenancySupport = false; });
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));


    std::string expectedShardName = "StandaloneShard";

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost("StandaloneHost:12345");
    expectedShard.setState(ShardType::ShardState::kShardAware);

    DatabaseType discoveredDB1(
        "TestDB1", ShardId("StandaloneShard"), DatabaseVersion(UUID::gen(), Timestamp(1, 1)));
    DatabaseType discoveredDB2(
        "TestDB2", ShardId("StandaloneShard"), DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

    auto expectWriteConcern = ShardingCatalogClient::kMajorityWriteConcern;

    auto future = launchAsync([this, expectedShardName, expectWriteConcern] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        opCtx->setWriteConcern(expectWriteConcern);
        auto shardName =
            assertGet(ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     &expectedShardName,
                                     assertGet(ConnectionString::parse("StandaloneHost:12345")),
                                     false));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(
        shardTarget,
        std::vector<BSONObj>{BSON("name"
                                  << "local"
                                  << "sizeOnDisk" << 1000),
                             BSON("name" << discoveredDB1.getName() << "sizeOnDisk" << 2000),
                             BSON("name" << discoveredDB2.getName() << "sizeOnDisk" << 5000)});

    expectCollectionDrop(
        shardTarget, NamespaceString::createNamespaceString_forTest("config", "system.sessions"));

    // The shard receives the _addShard command
    expectAddShardCmdReturnSuccess(shardTarget, expectedShardName);

    // The shard receives a delete op to clear any leftover user_writes_critical_sections doc.
    expectRemoveUserWritesCriticalSectionsDocs(shardTarget);

    std::vector<boost::optional<TenantId>> tenantsOnTarget = {
        boost::none,
        TenantId(OID("123456789012345678901234")),
        TenantId(OID("123456789012345678901235"))};

    // The shard receives a listDatabases to enumerate all tenants, then a find per tenant to pull
    // all cluster parameters. We supply a set of tenants which will be returned when finding all
    // tenants.
    expectClusterParametersPullRequest(shardTarget, tenantsOnTarget);

    // The shard receives the setFeatureCompatibilityVersion command.
    expectSetFeatureCompatibilityVersion(shardTarget, BSON("ok" << 1), expectWriteConcern.toBSON());

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was properly added to config.shards.
    assertShardExists(expectedShard);

    // Ensure that the databases detected from the shard were properly added to config.database.
    assertDatabaseExists(discoveredDB1);
    assertDatabaseExists(discoveredDB2);

    assertChangeWasLogged(expectedShard);

    checkLocalClusterParametersAfterPull(tenantsOnTarget);
}

TEST_F(AddShardTest, StandaloneMultitenantPushSuccess) {
    gMultitenancySupport = true;
    ScopeGuard guard([] { gMultitenancySupport = false; });
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));

    // Add cluster params for multiple tenants to push to the new shard.
    const TenantIdMap<std::vector<BSONObj>>& localClusterParameters = {
        {boost::none,
         {BSON("testIntClusterParameter" << BSON("intData" << 5)),
          BSON("testStrClusterParameter" << BSON("strData"
                                                 << "abc"))}},
        {TenantId(OID("123456789012345678901234")),
         {BSON("testIntClusterParameter" << BSON("intData" << 8)),
          BSON("testStrClusterParameter" << BSON("strData"
                                                 << "def")),
          BSON("testBoolClusterParameter" << BSON("boolData" << true))}},
        {TenantId(OID("123456789012345678901235")),
         {BSON("testIntClusterParameter" << BSON("intData" << 10))}}};
    addLocalClusterParameters(localClusterParameters);

    std::string expectedShardName = "StandaloneShard";

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost("StandaloneHost:12345");
    expectedShard.setState(ShardType::ShardState::kShardAware);

    DatabaseType discoveredDB1(
        "TestDB1", ShardId("StandaloneShard"), DatabaseVersion(UUID::gen(), Timestamp(1, 1)));
    DatabaseType discoveredDB2(
        "TestDB2", ShardId("StandaloneShard"), DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

    auto expectWriteConcern = ShardingCatalogClient::kMajorityWriteConcern;

    auto future = launchAsync([this, expectedShardName, expectWriteConcern] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        opCtx->setWriteConcern(expectWriteConcern);
        auto shardName =
            assertGet(ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     &expectedShardName,
                                     assertGet(ConnectionString::parse("StandaloneHost:12345")),
                                     false));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(
        shardTarget,
        std::vector<BSONObj>{BSON("name"
                                  << "local"
                                  << "sizeOnDisk" << 1000),
                             BSON("name" << discoveredDB1.getName() << "sizeOnDisk" << 2000),
                             BSON("name" << discoveredDB2.getName() << "sizeOnDisk" << 5000)});

    expectCollectionDrop(
        shardTarget, NamespaceString::createNamespaceString_forTest("config", "system.sessions"));

    // The shard receives the _addShard command
    expectAddShardCmdReturnSuccess(shardTarget, expectedShardName);

    // The shard receives a delete op to clear any leftover user_writes_critical_sections doc.
    expectRemoveUserWritesCriticalSectionsDocs(shardTarget);

    // The shard receives a listDatabases to enumerate all tenants, then a remove per tenant to
    // clear its cluster parameters, then inserts to push all tenants' cluster parameters. We supply
    // a set of tenants which will be returned when finding all tenants.
    expectClusterParametersPushRequest(shardTarget,
                                       {boost::none,
                                        TenantId(OID("123456789012345678901234")),
                                        TenantId(OID("123456789012345678901235"))},
                                       localClusterParameters);

    // The shard receives the setFeatureCompatibilityVersion command.
    expectSetFeatureCompatibilityVersion(shardTarget, BSON("ok" << 1), expectWriteConcern.toBSON());

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was properly added to config.shards.
    assertShardExists(expectedShard);

    // Ensure that the databases detected from the shard were properly added to config.database.
    assertDatabaseExists(discoveredDB1);
    assertDatabaseExists(discoveredDB2);

    assertChangeWasLogged(expectedShard);
}

TEST_F(AddShardTest, StandaloneGenerateName) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));

    ShardType existingShard;
    existingShard.setName("shard0005");
    existingShard.setHost("existingHost:12345");
    existingShard.setState(ShardType::ShardState::kShardAware);

    // Add a pre-existing shard so when generating a name for the new shard it will have to go
    // higher than the existing one.
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    NamespaceString::kConfigsvrShardsNamespace,
                                                    existingShard.toBSON(),
                                                    ShardingCatalogClient::kMajorityWriteConcern));
    assertShardExists(existingShard);

    std::string expectedShardName = "shard0006";

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost(shardTarget.toString());
    expectedShard.setState(ShardType::ShardState::kShardAware);

    DatabaseType discoveredDB1(
        "TestDB1", ShardId(expectedShardName), DatabaseVersion(UUID::gen(), Timestamp(1, 1)));
    DatabaseType discoveredDB2(
        "TestDB2", ShardId(expectedShardName), DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

    auto future = launchAsync([this, &expectedShardName, &shardTarget] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName =
            assertGet(ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), nullptr, ConnectionString(shardTarget), false));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(
        shardTarget,
        std::vector<BSONObj>{BSON("name"
                                  << "local"
                                  << "sizeOnDisk" << 1000),
                             BSON("name" << discoveredDB1.getName() << "sizeOnDisk" << 2000),
                             BSON("name" << discoveredDB2.getName() << "sizeOnDisk" << 5000)});

    expectCollectionDrop(
        shardTarget, NamespaceString::createNamespaceString_forTest("config", "system.sessions"));

    // The shard receives the _addShard command
    expectAddShardCmdReturnSuccess(shardTarget, expectedShardName);

    // The shard receives a delete op to clear any leftover user_writes_critical_sections doc.
    expectRemoveUserWritesCriticalSectionsDocs(shardTarget);

    // The shard receives a find to pull all cluster parameters.
    expectClusterParametersPushRequest(shardTarget);

    // The shard receives the setFeatureCompatibilityVersion command
    expectSetFeatureCompatibilityVersion(
        shardTarget, BSON("ok" << 1), operationContext()->getWriteConcern().toBSON());

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was properly added to config.shards.
    assertShardExists(expectedShard);

    // Ensure that the databases detected from the shard were properly added to config.database.
    assertDatabaseExists(discoveredDB1);
    assertDatabaseExists(discoveredDB2);

    assertChangeWasLogged(expectedShard);
}

TEST_F(AddShardTest, AddSCCCConnectionStringAsShard) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        std::make_unique<RemoteCommandTargeterMock>());
    auto invalidConn = ConnectionString("host1:12345,host2:12345,host3:12345",
                                        ConnectionString::ConnectionType::kInvalid);
    targeter->setConnectionStringReturnValue(invalidConn);

    auto future = launchAsync([this, invalidConn] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        const std::string shardName("StandaloneShard");
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), &shardName, invalidConn, false);
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
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
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
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status =
            ShardingCatalogManager::get(opCtx.get())
                ->addShard(opCtx.get(), &expectedShardName, ConnectionString(shardTarget), false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "host unreachable");
    });

    Status hostUnreachableStatus = Status(ErrorCodes::HostUnreachable, "host unreachable");
    expectIsMaster(shardTarget, hostUnreachableStatus);

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
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status =
            ShardingCatalogManager::get(opCtx.get())
                ->addShard(opCtx.get(), &expectedShardName, ConnectionString(shardTarget), false);
        ASSERT_EQUALS(ErrorCodes::IllegalOperation, status);
    });

    expectIsMaster(shardTarget,
                   BSON("msg"
                        << "isdbgrid"));

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
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status =
            ShardingCatalogManager::get(opCtx.get())
                ->addShard(opCtx.get(), &expectedShardName, ConnectionString(shardTarget), false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "use replica set url format");
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "myOtherSet"
                                        << "maxWireVersion" << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

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
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), &expectedShardName, connString, false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "host did not return a set name");
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

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
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), &expectedShardName, connString, false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "does not match the actual set name");
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "myOtherSet"
                                        << "maxWireVersion" << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

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
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), &expectedShardName, connString, false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(),
                               "as a shard since it is a config server");
    });

    BSONObj commandResponse =
        BSON("ok" << 1 << "ismaster" << true << "setName"
                  << "config"
                  << "configsvr" << true << "maxWireVersion" << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

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
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), &expectedShardName, connString, false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(),
                               "host2:12345 does not belong to replica set");
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts" << hosts.arr() << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

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
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), &expectedShardName, connString, false);
        ASSERT_EQUALS(ErrorCodes::BadValue, status);
        ASSERT_EQUALS(status.getStatus().reason(),
                      "use of shard replica set with name 'config' is not allowed");
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts" << hosts.arr() << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

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

    DatabaseType existingDB(
        "existing", ShardId("existingShard"), DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

    // Add a pre-existing database.
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    NamespaceString::kConfigDatabasesNamespace,
                                                    existingDB.toBSON(),
                                                    ShardingCatalogClient::kMajorityWriteConcern));
    assertDatabaseExists(existingDB);


    auto future = launchAsync([this, expectedShardName, connString] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto status = ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), &expectedShardName, connString, false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(
            status.getStatus().reason(),
            "because a local database 'existing' exists in another existingShard");
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts" << hosts.arr() << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

    expectListDatabases(shardTarget, {BSON("name" << existingDB.getName())});

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

    DatabaseType discoveredDB(
        "shardDB", ShardId(expectedShardName), DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

    auto future = launchAsync([this, &expectedShardName, &connString] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName = assertGet(ShardingCatalogManager::get(opCtx.get())
                                       ->addShard(opCtx.get(), nullptr, connString, false));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts" << hosts.arr() << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(shardTarget, std::vector<BSONObj>{BSON("name" << discoveredDB.getName())});

    expectCollectionDrop(
        shardTarget, NamespaceString::createNamespaceString_forTest("config", "system.sessions"));

    // The shard receives the _addShard command
    expectAddShardCmdReturnSuccess(shardTarget, expectedShardName);

    // The shard receives a delete op to clear any leftover user_writes_critical_sections doc.
    expectRemoveUserWritesCriticalSectionsDocs(shardTarget);

    // The shard receives a find to pull all cluster parameters.
    expectClusterParametersPullRequest(shardTarget);

    // The shard receives the setFeatureCompatibilityVersion command.
    expectSetFeatureCompatibilityVersion(
        shardTarget, BSON("ok" << 1), operationContext()->getWriteConcern().toBSON());

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was properly added to config.shards.
    assertShardExists(expectedShard);

    // Ensure that the databases detected from the shard were properly added to config.database.
    assertDatabaseExists(discoveredDB);

    assertChangeWasLogged(expectedShard);

    checkLocalClusterParametersAfterPull();
}

TEST_F(AddShardTest, SuccessfullyAddConfigShard) {
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

    DatabaseType discoveredDB(
        "shardDB", ShardId(expectedShardName), DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

    auto future = launchAsync([this, &expectedShardName, &connString] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName =
            assertGet(ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), nullptr, connString, true /* isConfigShard */));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts" << hosts.arr() << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(shardTarget, std::vector<BSONObj>{BSON("name" << discoveredDB.getName())});

    expectCollectionDrop(shardTarget, NamespaceString("config", "system.sessions"));

    // Should not run _addShard command, touch user_writes_critical_sections, setParameter, setFCV

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was properly added to config.shards.
    assertShardExists(expectedShard);

    // Ensure that the databases detected from the shard were properly added to config.database.
    assertDatabaseExists(discoveredDB);

    assertChangeWasLogged(expectedShard);
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

    DatabaseType discoveredDB(
        "shardDB", ShardId(expectedShardName), DatabaseVersion(UUID::gen(), Timestamp(1, 1)));

    auto future = launchAsync([this, &expectedShardName, &seedString] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName = assertGet(ShardingCatalogManager::get(opCtx.get())
                                       ->addShard(opCtx.get(), nullptr, seedString, false));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts" << hosts.arr() << "maxWireVersion"
                                        << WireVersion::LATEST_WIRE_VERSION);
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(shardTarget, std::vector<BSONObj>{BSON("name" << discoveredDB.getName())});

    expectCollectionDrop(
        shardTarget, NamespaceString::createNamespaceString_forTest("config", "system.sessions"));

    // The shard receives the _addShard command
    expectAddShardCmdReturnSuccess(shardTarget, expectedShardName);

    // The shard receives a delete op to clear any leftover user_writes_critical_sections doc.
    expectRemoveUserWritesCriticalSectionsDocs(shardTarget);

    // The shard receives a find to pull all cluster parameters.
    expectClusterParametersPullRequest(shardTarget);

    // The shard receives the setFeatureCompatibilityVersion command.
    expectSetFeatureCompatibilityVersion(
        shardTarget, BSON("ok" << 1), operationContext()->getWriteConcern().toBSON());

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
                                                    ShardingCatalogClient::kMajorityWriteConcern));
    assertShardExists(existingShard);

    // Adding the same standalone host with a different shard name should fail.
    std::string differentName = "anotherShardName";
    auto future1 = launchAsync([&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(
            ErrorCodes::IllegalOperation,
            ShardingCatalogManager::get(opCtx.get())
                ->addShard(opCtx.get(), &differentName, ConnectionString(shardTarget), false));
    });
    future1.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding the same standalone host but as part of a replica set should fail.
    // Ensures that even if the user changed the standalone shard to a single-node replica set, you
    // can't change the sharded cluster's notion of the shard from standalone to replica set just
    // by calling addShard.
    auto future3 = launchAsync([&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
                                     nullptr,
                                     ConnectionString::forReplicaSet("mySet", {shardTarget}),
                                     false));
    });
    future3.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding the same standalone host with the same options should succeed.
    auto future4 = launchAsync([&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName = assertGet(
            ShardingCatalogManager::get(opCtx.get())
                ->addShard(opCtx.get(), &existingShardName, ConnectionString(shardTarget), false));
        ASSERT_EQUALS(existingShardName, shardName);
    });
    future4.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding the same standalone host with the same options (without explicitly specifying the
    // shard name) should succeed.
    auto future5 = launchAsync([&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName =
            assertGet(ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), nullptr, ConnectionString(shardTarget), false));
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
                                                    ShardingCatalogClient::kMajorityWriteConcern));
    assertShardExists(existingShard);
    // Adding the same connection string with a different shard name should fail.
    std::string differentName = "anotherShardName";
    auto future1 = launchAsync([&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), &differentName, connString, false));
    });
    future1.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding a different connection string with the same shard name should fail.
    ConnectionString otherHostConnString2 =
        assertGet(ConnectionString::parse("mySet1/host2:12345"));
    auto future2 = launchAsync([&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), &existingShardName, otherHostConnString2, false));
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
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), nullptr, ConnectionString(shardTarget), false));
    });
    future3.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding a connecting string with the same hosts but a different replica set name should fail.
    // Ensures that even if you manually change the shard's replica set name somehow, you can't
    // change the replica set name the sharded cluster knows for it just by calling addShard again.
    std::string differentSetName = "differentSet";
    auto future4 = launchAsync([&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(),
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
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName =
            assertGet(ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), &existingShardName, connString, false));
        ASSERT_EQUALS(existingShardName, shardName);
    });
    future5.timed_get(kLongFutureTimeout);

    // Adding the same host with the same options (without explicitly specifying the shard name)
    // should succeed.
    auto future6 = launchAsync([&] {
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName = assertGet(ShardingCatalogManager::get(opCtx.get())
                                       ->addShard(opCtx.get(), nullptr, connString, false));
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
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), nullptr, otherHostConnString, false));
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
                                                    ShardingCatalogClient::kMajorityWriteConcern));
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
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), nullptr, otherHostConnString, false));
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
        ThreadClient tc(getServiceContext());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto shardName =
            assertGet(ShardingCatalogManager::get(opCtx.get())
                          ->addShard(opCtx.get(), &existingShardName, otherHostConnString1, false));
        ASSERT_EQUALS(existingShardName, shardName);
    });
    future2.timed_get(kLongFutureTimeout);
    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);
}
}  // namespace
}  // namespace mongo
