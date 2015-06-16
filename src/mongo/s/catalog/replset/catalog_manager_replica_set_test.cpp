/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <chrono>
#include <future>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set_test_fixture.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_update_request.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

    using executor::NetworkInterfaceMock;
    using executor::TaskExecutor;
    using std::async;
    using std::string;
    using std::vector;
    using unittest::assertGet;

    static const std::chrono::seconds kFutureTimeout{5};

    TEST_F(CatalogManagerReplSetTestFixture, GetCollectionExisting) {
        RemoteCommandTargeterMock* targeter =
            RemoteCommandTargeterMock::get(shardRegistry()->findIfExists("config")->getTargeter());
        targeter->setFindHostReturnValue(HostAndPort("TestHost1"));

        CollectionType expectedColl;
        expectedColl.setNs(NamespaceString("TestDB.TestNS"));
        expectedColl.setKeyPattern(BSON("KeyName" << 1));
        expectedColl.setUpdatedAt(Date_t());
        expectedColl.setEpoch(OID::gen());

        auto future = async(std::launch::async, [this, &expectedColl] {
            return assertGet(catalogManager()->getCollection(expectedColl.getNs()));
        });

        onFindCommand([&expectedColl](const RemoteCommandRequest& request) {
            const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQ(nss.toString(), CollectionType::ConfigNS);

            auto query = assertGet(LiteParsedQuery::fromFindCommand(nss, request.cmdObj, false));

            // Ensure the query is correct
            ASSERT_EQ(query->ns(), CollectionType::ConfigNS);
            ASSERT_EQ(query->getFilter(), BSON(CollectionType::fullNs(expectedColl.getNs())));

            return vector<BSONObj>{ expectedColl.toBSON() };
        });

        // Now wait for the getCollection call to return
        const auto& actualColl = future.get();
        ASSERT_EQ(expectedColl.toBSON(), actualColl.toBSON());
    }

    TEST_F(CatalogManagerReplSetTestFixture, GetCollectionNotExisting) {
        RemoteCommandTargeterMock* targeter =
            RemoteCommandTargeterMock::get(shardRegistry()->findIfExists("config")->getTargeter());
        targeter->setFindHostReturnValue(HostAndPort("TestHost1"));

        auto future = async(std::launch::async, [this] {
            auto status = catalogManager()->getCollection("NonExistent");
            ASSERT_EQUALS(status.getStatus(), ErrorCodes::NamespaceNotFound);
        });

        onFindCommand([](const RemoteCommandRequest& request) {
            return vector<BSONObj>{ };
        });

        // Now wait for the getCollection call to return
        future.get();
    }

    TEST_F(CatalogManagerReplSetTestFixture, GetDatabaseExisting) {
        RemoteCommandTargeterMock* targeter =
            RemoteCommandTargeterMock::get(shardRegistry()->findIfExists("config")->getTargeter());
        targeter->setFindHostReturnValue(HostAndPort("TestHost1"));

        DatabaseType expectedDb;
        expectedDb.setName("bigdata");
        expectedDb.setPrimary("shard0000");
        expectedDb.setSharded(true);

        auto future = async(std::launch::async, [this, &expectedDb] {
            return assertGet(catalogManager()->getDatabase(expectedDb.getName()));
        });

        onFindCommand([&expectedDb](const RemoteCommandRequest& request) {
            const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQ(nss.toString(), DatabaseType::ConfigNS);

            auto query = assertGet(LiteParsedQuery::fromFindCommand(nss, request.cmdObj, false));

            ASSERT_EQ(query->ns(), DatabaseType::ConfigNS);
            ASSERT_EQ(query->getFilter(), BSON(DatabaseType::name(expectedDb.getName())));

            return vector<BSONObj>{ expectedDb.toBSON() };
        });

        const auto& actualDb = future.get();
        ASSERT_EQ(expectedDb.toBSON(), actualDb.toBSON());
    }

    TEST_F(CatalogManagerReplSetTestFixture, GetDatabaseNotExisting) {
        RemoteCommandTargeterMock* targeter =
            RemoteCommandTargeterMock::get(shardRegistry()->findIfExists("config")->getTargeter());
        targeter->setFindHostReturnValue(HostAndPort("TestHost1"));

        auto future = async(std::launch::async, [this] {
            auto dbResult = catalogManager()->getDatabase("NonExistent");
            ASSERT_EQ(dbResult.getStatus(), ErrorCodes::NamespaceNotFound);
        });

        onFindCommand([](const RemoteCommandRequest& request) {
            return vector<BSONObj>{ };
        });

        future.get();
    }

    TEST_F(CatalogManagerReplSetTestFixture, UpdateCollection) {
        RemoteCommandTargeterMock* targeter =
            RemoteCommandTargeterMock::get(shardRegistry()->findIfExists("config")->getTargeter());
        targeter->setFindHostReturnValue(HostAndPort("TestHost1"));

        CollectionType collection;
        collection.setNs(NamespaceString("db.coll"));
        collection.setUpdatedAt(network()->now());
        collection.setUnique(true);
        collection.setEpoch(OID::gen());
        collection.setKeyPattern(KeyPattern(BSON("_id" << 1)));

        auto future = async(std::launch::async, [this, collection] {
            auto status = catalogManager()->updateCollection(collection.getNs().toString(),
                                                             collection);
            ASSERT_OK(status);
        });

        onCommand([collection](const RemoteCommandRequest& request) {
            ASSERT_EQUALS("config", request.dbname);

            BatchedUpdateRequest actualBatchedUpdate;
            std::string errmsg;
            ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.cmdObj, &errmsg));
            ASSERT_EQUALS(CollectionType::ConfigNS, actualBatchedUpdate.getCollName());
            auto updates = actualBatchedUpdate.getUpdates();
            ASSERT_EQUALS(1U, updates.size());
            auto update = updates.front();

            ASSERT_TRUE(update->getUpsert());
            ASSERT_FALSE(update->getMulti());
            ASSERT_EQUALS(update->getQuery(),
                          BSON(CollectionType::fullNs(collection.getNs().toString())));
            ASSERT_EQUALS(update->getUpdateExpr(), collection.toBSON());

            BatchedCommandResponse response;
            response.setOk(true);
            response.setNModified(1);

            return response.toBSON();
        });

        // Now wait for the updateCollection call to return
        future.wait_for(kFutureTimeout);
    }

    TEST_F(CatalogManagerReplSetTestFixture, UpdateCollectionNotMaster) {
        RemoteCommandTargeterMock* targeter =
            RemoteCommandTargeterMock::get(shardRegistry()->findIfExists("config")->getTargeter());
        targeter->setFindHostReturnValue(HostAndPort("TestHost1"));

        CollectionType collection;
        collection.setNs(NamespaceString("db.coll"));
        collection.setUpdatedAt(network()->now());
        collection.setUnique(true);
        collection.setEpoch(OID::gen());
        collection.setKeyPattern(KeyPattern(BSON("_id" << 1)));

        auto future = async(std::launch::async, [this, collection] {
            auto status = catalogManager()->updateCollection(collection.getNs().toString(),
                                                             collection);
            ASSERT_EQUALS(ErrorCodes::NotMaster, status);
        });

        for (int i = 0; i < 3; ++i) {
            onCommand([](const RemoteCommandRequest& request) {
                BatchedCommandResponse response;
                response.setOk(false);
                response.setErrCode(ErrorCodes::NotMaster);
                response.setErrMessage("not master");

                return response.toBSON();
            });
        }

        // Now wait for the updateCollection call to return
        future.wait_for(kFutureTimeout);
    }

    TEST_F(CatalogManagerReplSetTestFixture, UpdateCollectionNotMasterRetrySuccess) {
        RemoteCommandTargeterMock* targeter =
            RemoteCommandTargeterMock::get(shardRegistry()->findIfExists("config")->getTargeter());
        HostAndPort host1("TestHost1");
        HostAndPort host2("TestHost2");
        targeter->setFindHostReturnValue(host1);

        CollectionType collection;
        collection.setNs(NamespaceString("db.coll"));
        collection.setUpdatedAt(network()->now());
        collection.setUnique(true);
        collection.setEpoch(OID::gen());
        collection.setKeyPattern(KeyPattern(BSON("_id" << 1)));

        auto future = async(std::launch::async, [this, collection] {
            auto status = catalogManager()->updateCollection(collection.getNs().toString(),
                                                             collection);
            ASSERT_OK(status);
        });

        onCommand([host1, host2, targeter](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(host1, request.target);

            BatchedCommandResponse response;
            response.setOk(false);
            response.setErrCode(ErrorCodes::NotMaster);
            response.setErrMessage("not master");

            // Ensure that when the catalog manager tries to retarget after getting the
            // NotMaster response, it will get back a new target.
            targeter->setFindHostReturnValue(host2);
            return response.toBSON();
        });

        onCommand([host2, collection](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(host2, request.target);

            BatchedUpdateRequest actualBatchedUpdate;
            std::string errmsg;
            ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.cmdObj, &errmsg));
            ASSERT_EQUALS(CollectionType::ConfigNS, actualBatchedUpdate.getCollName());
            auto updates = actualBatchedUpdate.getUpdates();
            ASSERT_EQUALS(1U, updates.size());
            auto update = updates.front();

            ASSERT_TRUE(update->getUpsert());
            ASSERT_FALSE(update->getMulti());
            ASSERT_EQUALS(update->getQuery(),
                          BSON(CollectionType::fullNs(collection.getNs().toString())));
            ASSERT_EQUALS(update->getUpdateExpr(), collection.toBSON());

            BatchedCommandResponse response;
            response.setOk(true);
            response.setNModified(1);

            return response.toBSON();
        });

        // Now wait for the updateCollection call to return
        future.wait_for(kFutureTimeout);
    }

} // namespace
} // namespace mongo
