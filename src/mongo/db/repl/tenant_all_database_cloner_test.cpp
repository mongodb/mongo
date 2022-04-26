/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/tenant_all_database_cloner.h"
#include "mongo/db/repl/tenant_cloner_test_fixture.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace repl {

class TenantAllDatabaseClonerTest : public TenantClonerTestFixture {
public:
    TenantAllDatabaseClonerTest() {}

protected:
    std::unique_ptr<TenantAllDatabaseCloner> makeAllDatabaseCloner(
        TenantMigrationSharedData* sharedData = nullptr) {
        return std::make_unique<TenantAllDatabaseCloner>(sharedData ? sharedData : getSharedData(),
                                                         _source,
                                                         _mockClient.get(),
                                                         &_storageInterface,
                                                         _dbWorkThreadPool.get(),
                                                         _tenantId);
    }

    std::vector<std::string> getDatabasesFromCloner(TenantAllDatabaseCloner* cloner) {
        return cloner->_databases;
    }

    BSONObj createFindResponse(ErrorCodes::Error code = ErrorCodes::OK) {
        BSONObjBuilder bob;
        if (code != ErrorCodes::OK) {
            bob.append("ok", 0);
            bob.append("code", code);
        } else {
            bob.append("ok", 1);
        }
        return bob.obj();
    }

    const std::string _tenantDbA = _tenantId + "_a";
    const std::string _tenantDbAAB = _tenantId + "_aab";
    const std::string _tenantDbABC = _tenantId + "_abc";
    const std::string _tenantDbB = _tenantId + "_b";
};

TEST_F(TenantAllDatabaseClonerTest, FailsOnListDatabases) {
    Status expectedResult{ErrorCodes::BadValue, "foo"};
    _mockServer->setCommandReply("listDatabases", expectedResult);
    _mockServer->setCommandReply("find", createFindResponse());
    auto cloner = makeAllDatabaseCloner();

    auto result = cloner->run();

    ASSERT_EQ(result, expectedResult);
}

TEST_F(TenantAllDatabaseClonerTest, DatabasesSortedByNameOne) {
    // Passed in as b -> aab -> abc -> a.
    _mockServer->setCommandReply("listDatabases",
                                 fromjson("{ok:1, databases:[{name:'" + _tenantDbB + "'}, {name:'" +
                                          _tenantDbAAB + "'}, {name:'" + _tenantDbABC +
                                          "'}, {name:'" + _tenantDbA + "'}]}"));

    _mockServer->setCommandReply("find", createFindResponse());
    auto cloner = makeAllDatabaseCloner();
    cloner->setStopAfterStage_forTest("listDatabases");

    ASSERT_OK(cloner->run());

    auto databases = getDatabasesFromCloner(cloner.get());
    ASSERT_EQUALS(4u, databases.size());
    ASSERT_EQUALS(_tenantDbA, databases[0]);
    ASSERT_EQUALS(_tenantDbAAB, databases[1]);
    ASSERT_EQUALS(_tenantDbABC, databases[2]);
    ASSERT_EQUALS(_tenantDbB, databases[3]);
}

TEST_F(TenantAllDatabaseClonerTest, DatabasesSortedByNameTwo) {
    // Passed in as a -> aab -> abc -> b.
    _mockServer->setCommandReply("listDatabases",
                                 fromjson("{ok:1, databases:[{name:'" + _tenantDbA + "'}, {name:'" +
                                          _tenantDbAAB + "'}, {name:'" + _tenantDbABC +
                                          "'}, {name:'" + _tenantDbB + "'}]}"));

    _mockServer->setCommandReply("find", createFindResponse());
    auto cloner = makeAllDatabaseCloner();
    cloner->setStopAfterStage_forTest("listDatabases");

    ASSERT_OK(cloner->run());

    auto databases = getDatabasesFromCloner(cloner.get());
    ASSERT_EQUALS(4u, databases.size());
    ASSERT_EQUALS(_tenantDbA, databases[0]);
    ASSERT_EQUALS(_tenantDbAAB, databases[1]);
    ASSERT_EQUALS(_tenantDbABC, databases[2]);
    ASSERT_EQUALS(_tenantDbB, databases[3]);
}

TEST_F(TenantAllDatabaseClonerTest, DatabaseStats) {
    _mockServer->setCommandReply("listDatabases",
                                 fromjson("{ok:1, databases:[{name:'" + _tenantDbA + "'}, {name:'" +
                                          _tenantDbAAB + "'}, {name:'" + _tenantDbABC + "'}]}"));
    _mockServer->setCommandReply("find", createFindResponse());

    // Make the DatabaseCloner do nothing
    _mockServer->setCommandReply("listCollections", createCursorResponse("admin.$cmd", {}));
    auto cloner = makeAllDatabaseCloner();

    // Set up the DatabaseCloner to pause so we can check stats.
    // We need to use two fail points to do this because fail points cannot have their data
    // modified atomically.
    auto dbClonerBeforeFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto dbClonerAfterFailPoint = globalFailPointRegistry().find("hangAfterClonerStage");
    auto timesEntered = dbClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'TenantDatabaseCloner', stage: 'listCollections', database: '" +
                 _tenantDbA + "'}"));
    dbClonerAfterFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'TenantDatabaseCloner', stage: 'listCollections', database: '" +
                 _tenantDbA + "'}"));

    _clock.advance(Minutes(1));
    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_OK(cloner->run());
    });

    // Wait for the failpoint to be reached
    dbClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);

    auto databases = getDatabasesFromCloner(cloner.get());
    ASSERT_EQUALS(3u, databases.size());
    ASSERT_EQUALS(_tenantDbA, databases[0]);
    ASSERT_EQUALS(_tenantDbAAB, databases[1]);
    ASSERT_EQUALS(_tenantDbABC, databases[2]);

    auto stats = cloner->getStats();
    ASSERT_EQUALS(0, stats.databasesCloned);
    ASSERT_EQUALS(3, stats.databaseStats.size());
    ASSERT_EQUALS(_tenantDbA, stats.databaseStats[0].dbname);
    ASSERT_EQUALS(_tenantDbAAB, stats.databaseStats[1].dbname);
    ASSERT_EQUALS(_tenantDbABC, stats.databaseStats[2].dbname);
    ASSERT_EQUALS(_clock.now(), stats.databaseStats[0].start);
    ASSERT_EQUALS(Date_t(), stats.databaseStats[0].end);
    ASSERT_EQUALS(Date_t(), stats.databaseStats[1].start);
    ASSERT_EQUALS(Date_t(), stats.databaseStats[1].end);
    ASSERT_EQUALS(Date_t(), stats.databaseStats[2].start);
    ASSERT_EQUALS(Date_t(), stats.databaseStats[2].end);
    _clock.advance(Minutes(1));

    // Allow the cloner to move to the next DB.
    timesEntered = dbClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'TenantDatabaseCloner', stage: 'listCollections', database: '" +
                 _tenantDbAAB + "'}"));
    dbClonerAfterFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'TenantDatabaseCloner', stage: 'listCollections', database: '" +
                 _tenantDbAAB + "'}"));

    // Wait for the failpoint to be reached.
    dbClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);

    stats = cloner->getStats();
    ASSERT_EQUALS(1, stats.databasesCloned);
    ASSERT_EQUALS(3, stats.databaseStats.size());
    ASSERT_EQUALS(_tenantDbA, stats.databaseStats[0].dbname);
    ASSERT_EQUALS(_tenantDbAAB, stats.databaseStats[1].dbname);
    ASSERT_EQUALS(_tenantDbABC, stats.databaseStats[2].dbname);
    ASSERT_EQUALS(_clock.now(), stats.databaseStats[0].end);
    ASSERT_EQUALS(_clock.now(), stats.databaseStats[1].start);
    ASSERT_EQUALS(Date_t(), stats.databaseStats[1].end);
    ASSERT_EQUALS(Date_t(), stats.databaseStats[2].start);
    ASSERT_EQUALS(Date_t(), stats.databaseStats[2].end);
    _clock.advance(Minutes(1));

    // Allow the cloner to move to the last DB.
    timesEntered = dbClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'TenantDatabaseCloner', stage: 'listCollections', database: '" +
                 _tenantDbABC + "'}"));
    dbClonerAfterFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'TenantDatabaseCloner', stage: 'listCollections', database: '" +
                 _tenantDbABC + "'}"));

    // Wait for the failpoint to be reached.
    dbClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);

    stats = cloner->getStats();
    ASSERT_EQUALS(2, stats.databasesCloned);
    ASSERT_EQUALS(3, stats.databaseStats.size());
    ASSERT_EQUALS(_tenantDbA, stats.databaseStats[0].dbname);
    ASSERT_EQUALS(_tenantDbAAB, stats.databaseStats[1].dbname);
    ASSERT_EQUALS(_tenantDbABC, stats.databaseStats[2].dbname);
    ASSERT_EQUALS(_clock.now(), stats.databaseStats[1].end);
    ASSERT_EQUALS(_clock.now(), stats.databaseStats[2].start);
    ASSERT_EQUALS(Date_t(), stats.databaseStats[2].end);
    _clock.advance(Minutes(1));

    // Allow the cloner to finish
    dbClonerBeforeFailPoint->setMode(FailPoint::off, 0);
    dbClonerAfterFailPoint->setMode(FailPoint::off, 0);
    clonerThread.join();

    stats = cloner->getStats();
    ASSERT_EQUALS(3, stats.databasesCloned);
    ASSERT_EQUALS(_tenantDbA, stats.databaseStats[0].dbname);
    ASSERT_EQUALS(_tenantDbAAB, stats.databaseStats[1].dbname);
    ASSERT_EQUALS(_tenantDbABC, stats.databaseStats[2].dbname);
    ASSERT_EQUALS(_clock.now(), stats.databaseStats[2].end);
}

TEST_F(TenantAllDatabaseClonerTest, FailsOnListCollectionsOnOnlyDatabase) {
    _mockServer->setCommandReply("listDatabases",
                                 fromjson("{ok:1, databases:[{name:'" + _tenantDbA + "'}]}"));
    _mockServer->setCommandReply("find", createFindResponse());
    _mockServer->setCommandReply("listCollections", Status{ErrorCodes::NoSuchKey, "fake"});

    auto cloner = makeAllDatabaseCloner();
    ASSERT_NOT_OK(cloner->run());
}

TEST_F(TenantAllDatabaseClonerTest, ClonesDatabasesForTenant) {
    auto listDatabasesReply =
        "{ok:1, databases:[{name:'" + _tenantDbA + "'}, {name:'" + _tenantDbAAB + "'}]}";
    _mockServer->setCommandReply("listDatabases", fromjson(listDatabasesReply));
    _mockServer->setCommandReply("find", createFindResponse());

    auto cloner = makeAllDatabaseCloner();
    cloner->setStopAfterStage_forTest("listDatabases");

    ASSERT_OK(cloner->run());

    auto databases = getDatabasesFromCloner(cloner.get());
    ASSERT_EQUALS(2u, databases.size());
    ASSERT_EQUALS(_tenantDbA, databases[0]);
    ASSERT_EQUALS(_tenantDbAAB, databases[1]);
}

TEST_F(TenantAllDatabaseClonerTest, ListDatabasesMajorityReadFailsWithSpecificError) {
    auto listDatabasesReply =
        "{ok:1, databases:[{name:'" + _tenantDbA + "'}, {name:'" + _tenantDbAAB + "'}]}";
    _mockServer->setCommandReply("listDatabases", fromjson(listDatabasesReply));
    _mockServer->setCommandReply("find", createFindResponse(ErrorCodes::OperationFailed));

    auto cloner = makeAllDatabaseCloner();
    cloner->setStopAfterStage_forTest("listDatabases");

    auto status = cloner->run();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status.code());
}

TEST_F(TenantAllDatabaseClonerTest, ListCollectionsRemoteUnreachableBeforeMajorityFind) {
    auto listDatabasesReply =
        "{ok:1, databases:[{name:'" + _tenantDbA + "'}, {name:'" + _tenantDbAAB + "'}]}";
    _mockServer->setCommandReply("listDatabases", fromjson(listDatabasesReply));

    auto cloner = makeAllDatabaseCloner();
    cloner->setStopAfterStage_forTest("listDatabases");

    auto clonerOperationTimeFP =
        globalFailPointRegistry().find("tenantAllDatabaseClonerHangAfterGettingOperationTime");
    auto timesEntered = clonerOperationTimeFP->setMode(FailPoint::alwaysOn, 0);

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_NOT_OK(cloner->run());
    });

    // Wait for the failpoint to be reached
    clonerOperationTimeFP->waitForTimesEntered(timesEntered + 1);
    _mockServer->shutdown();

    // Finish test
    clonerOperationTimeFP->setMode(FailPoint::off, 0);
    clonerThread.join();
}

TEST_F(TenantAllDatabaseClonerTest, ListDatabasesRecordsCorrectOperationTime) {
    auto listDatabasesReply =
        "{ok:1, databases:[{name:'" + _tenantDbA + "'}, {name:'" + _tenantDbAAB + "'}]}";
    _mockServer->setCommandReply("listDatabases", fromjson(listDatabasesReply));
    _mockServer->setCommandReply("find", createFindResponse());

    auto clonerOperationTimeFP =
        globalFailPointRegistry().find("tenantAllDatabaseClonerHangAfterGettingOperationTime");
    auto timesEntered = clonerOperationTimeFP->setMode(FailPoint::alwaysOn, 0);

    auto cloner = makeAllDatabaseCloner();
    cloner->setStopAfterStage_forTest("listDatabases");

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_OK(cloner->run());
    });

    // Wait for the failpoint to be reached
    clonerOperationTimeFP->waitForTimesEntered(timesEntered + 1);
    ASSERT_EQUALS(_operationTime, cloner->getOperationTime_forTest());

    // Finish test
    clonerOperationTimeFP->setMode(FailPoint::off, 0);
    clonerThread.join();

    auto databases = getDatabasesFromCloner(cloner.get());
    ASSERT_EQUALS(2u, databases.size());
    ASSERT_EQUALS(_tenantDbA, databases[0]);
    ASSERT_EQUALS(_tenantDbAAB, databases[1]);
}

TEST_F(TenantAllDatabaseClonerTest, TenantDatabasesAlreadyExist) {
    // Test that cloner should fail if tenant databases already exist on the recipient prior to
    // starting cloning phase of the migration.
    ASSERT_OK(createCollection(NamespaceString(_tenantDbA, "coll"), CollectionOptions()));

    auto listDatabasesReply =
        "{ok:1, databases:[{name:'" + _tenantDbA + "'}, {name:'" + _tenantDbAAB + "'}]}";
    _mockServer->setCommandReply("listDatabases", fromjson(listDatabasesReply));
    _mockServer->setCommandReply("find", createFindResponse());

    auto cloner = makeAllDatabaseCloner();

    ASSERT_NOT_OK(cloner->run());
}

TEST_F(TenantAllDatabaseClonerTest, ResumingFromLastClonedDb) {
    // Test that all databases cloner correctly resumes from the last cloned database.
    auto nssDbA = NamespaceString(_tenantDbA, "coll");
    auto nssDbAAb = NamespaceString(_tenantDbAAB, "coll");
    ASSERT_OK(createCollection(nssDbA, CollectionOptions()));
    ASSERT_OK(createCollection(nssDbAAb, CollectionOptions()));

    long long sizeOfDbA = 0;
    {
        // Insert some documents into both collections.
        auto storage = StorageInterface::get(serviceContext);
        auto opCtx = cc().makeOperationContext();

        ASSERT_OK(storage->insertDocument(
            opCtx.get(), nssDbA, {BSON("_id" << 0 << "a" << 1001), Timestamp(0)}, 0));
        ASSERT_OK(storage->insertDocument(
            opCtx.get(), nssDbAAb, {BSON("_id" << 0 << "a" << 1001), Timestamp(0)}, 0));

        auto swSizeofDbA = storage->getCollectionSize(opCtx.get(), nssDbA);
        ASSERT_OK(swSizeofDbA.getStatus());
        sizeOfDbA = swSizeofDbA.getValue();
    }

    auto listDatabasesReply =
        "{ok:1, databases:[{name:'" + _tenantDbA + "'}, {name:'" + _tenantDbAAB + "'}]}";
    _mockServer->setCommandReply("listDatabases", fromjson(listDatabasesReply));
    _mockServer->setCommandReply("find", createFindResponse());
    _mockServer->setCommandReply("dbStats", fromjson("{ok:1, dataSize: 30}"));

    TenantMigrationSharedData resumingSharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeAllDatabaseCloner(&resumingSharedData);
    cloner->setStopAfterStage_forTest("initializeStatsStage");

    ASSERT_OK(cloner->run());

    auto databases = getDatabasesFromCloner(cloner.get());
    ASSERT_EQUALS(1u, databases.size());
    ASSERT_EQUALS(_tenantDbAAB, databases[0]);

    auto stats = cloner->getStats();
    ASSERT_EQUALS(1, stats.databasesClonedBeforeFailover);

    ASSERT_EQUALS(sizeOfDbA, stats.approxTotalBytesCopied);
    ASSERT_LESS_THAN(stats.approxTotalBytesCopied, stats.approxTotalDataSize);
}

TEST_F(TenantAllDatabaseClonerTest, LastClonedDbDeleted_AllGreater) {
    // Test that we correctly resume from next database compared greater than the last cloned
    // database if the last cloned database is dropped. This tests the case when all databases in
    // the latest listDatabases result are compared greater than the last cloned database.
    auto nssDbA = NamespaceString(_tenantDbA, "coll");
    ASSERT_OK(createCollection(nssDbA, CollectionOptions()));

    long long size = 0;
    {
        // Insert document into the collection.
        auto storage = StorageInterface::get(serviceContext);
        auto opCtx = cc().makeOperationContext();

        ASSERT_OK(storage->insertDocument(
            opCtx.get(), nssDbA, {BSON("_id" << 0 << "a_field" << 1001), Timestamp(0)}, 0));
        auto swSize = storage->getCollectionSize(opCtx.get(), nssDbA);
        ASSERT_OK(swSize.getStatus());
        size = swSize.getValue();
    }

    auto listDatabasesReply =
        "{ok:1, databases:[{name:'" + _tenantDbAAB + "'}, {name:'" + _tenantDbABC + "'}]}";
    _mockServer->setCommandReply("listDatabases", fromjson(listDatabasesReply));
    _mockServer->setCommandReply("find", createFindResponse());
    _mockServer->setCommandReply("dbStats", fromjson("{ok:1, dataSize: 30}"));

    TenantMigrationSharedData resumingSharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeAllDatabaseCloner(&resumingSharedData);
    cloner->setStopAfterStage_forTest("initializeStatsStage");

    ASSERT_OK(cloner->run());

    auto databases = getDatabasesFromCloner(cloner.get());
    ASSERT_EQUALS(2u, databases.size());
    ASSERT_EQUALS(_tenantDbAAB, databases[0]);
    ASSERT_EQUALS(_tenantDbABC, databases[1]);

    auto stats = cloner->getStats();
    ASSERT_EQUALS(1, stats.databasesClonedBeforeFailover);
    ASSERT_EQUALS(size, stats.approxTotalBytesCopied);
    ASSERT_LESS_THAN(stats.approxTotalBytesCopied, stats.approxTotalDataSize);
}

TEST_F(TenantAllDatabaseClonerTest, LastClonedDbDeleted_SomeGreater) {
    // Test that we correctly resume from next database compared greater than the last cloned
    // database if the last cloned database is dropped. This tests the case when some but not all
    // databases in the latest listDatabases result are compared greater than the last cloned
    // database.
    auto nssDbA = NamespaceString(_tenantDbA, "coll");
    auto nssDbAAb = NamespaceString(_tenantDbAAB, "coll");
    ASSERT_OK(createCollection(nssDbA, CollectionOptions()));
    ASSERT_OK(createCollection(nssDbAAb, CollectionOptions()));

    long long size = 0;
    {
        // Insert some documents into both collections.
        auto storage = StorageInterface::get(serviceContext);
        auto opCtx = cc().makeOperationContext();

        ASSERT_OK(storage->insertDocument(
            opCtx.get(), nssDbA, {BSON("_id" << 0 << "a" << 1001), Timestamp(0)}, 0));
        ASSERT_OK(storage->insertDocument(opCtx.get(),
                                          nssDbAAb,
                                          {BSON("_id" << 0 << "a"
                                                      << "hello"),
                                           Timestamp(0)},
                                          0));

        auto swSizeDbA = storage->getCollectionSize(opCtx.get(), nssDbA);
        ASSERT_OK(swSizeDbA.getStatus());
        size = swSizeDbA.getValue();

        auto swSizeDbAAb = storage->getCollectionSize(opCtx.get(), nssDbAAb);
        ASSERT_OK(swSizeDbAAb.getStatus());
        size += swSizeDbAAb.getValue();
    }

    auto listDatabasesReply =
        "{ok:1, databases:[{name:'" + _tenantDbA + "'}, {name:'" + _tenantDbABC + "'}]}";
    _mockServer->setCommandReply("listDatabases", fromjson(listDatabasesReply));
    _mockServer->setCommandReply("find", createFindResponse());
    _mockServer->setCommandReply("dbStats", fromjson("{ok:1, dataSize: 30}"));

    TenantMigrationSharedData resumingSharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeAllDatabaseCloner(&resumingSharedData);
    cloner->setStopAfterStage_forTest("initializeStatsStage");

    ASSERT_OK(cloner->run());

    auto databases = getDatabasesFromCloner(cloner.get());
    ASSERT_EQUALS(1u, databases.size());
    ASSERT_EQUALS(_tenantDbABC, databases[0]);

    auto stats = cloner->getStats();
    ASSERT_EQUALS(2, cloner->getStats().databasesClonedBeforeFailover);
    ASSERT_EQUALS(size, stats.approxTotalBytesCopied);
    ASSERT_LESS_THAN(stats.approxTotalBytesCopied, stats.approxTotalDataSize);
}

TEST_F(TenantAllDatabaseClonerTest, LastClonedDbDeleted_AllLess) {
    // Test that we correctly resume from next database compared greater than the last cloned
    // database if the last cloned database is dropped. This tests the case when all databases in
    // the latest listDatabases result are compared less than the last cloned database.
    auto nssDbA = NamespaceString(_tenantDbA, "coll");
    auto nssDbAAb = NamespaceString(_tenantDbAAB, "coll");
    auto nssDbABC = NamespaceString(_tenantDbABC, "coll");

    ASSERT_OK(createCollection(nssDbA, CollectionOptions()));
    ASSERT_OK(createCollection(nssDbAAb, CollectionOptions()));
    ASSERT_OK(createCollection(nssDbABC, CollectionOptions()));

    long long size = 0;
    {
        // Insert some documents into all three collections.
        auto storage = StorageInterface::get(serviceContext);
        auto opCtx = cc().makeOperationContext();

        ASSERT_OK(storage->insertDocument(
            opCtx.get(), nssDbA, {BSON("_id" << 0 << "a" << 1001), Timestamp(0)}, 0));
        ASSERT_OK(storage->insertDocument(opCtx.get(),
                                          nssDbAAb,
                                          {BSON("_id" << 0 << "a"
                                                      << "hello"),
                                           Timestamp(0)},
                                          0));
        ASSERT_OK(storage->insertDocument(opCtx.get(),
                                          nssDbABC,
                                          {BSON("_id" << 0 << "a"
                                                      << "goodbye"),
                                           Timestamp(0)},
                                          0));

        auto swSizeDbA = storage->getCollectionSize(opCtx.get(), nssDbA);
        ASSERT_OK(swSizeDbA.getStatus());
        size = swSizeDbA.getValue();

        auto swSizeDbAAb = storage->getCollectionSize(opCtx.get(), nssDbAAb);
        ASSERT_OK(swSizeDbAAb.getStatus());
        size += swSizeDbAAb.getValue();

        auto swSizeDbABC = storage->getCollectionSize(opCtx.get(), nssDbABC);
        ASSERT_OK(swSizeDbABC.getStatus());
        size += swSizeDbABC.getValue();
    }

    auto listDatabasesReply =
        "{ok:1, databases:[{name:'" + _tenantDbA + "'}, {name:'" + _tenantDbAAB + "'}]}";
    _mockServer->setCommandReply("listDatabases", fromjson(listDatabasesReply));
    _mockServer->setCommandReply("find", createFindResponse());
    _mockServer->setCommandReply("dbStats", fromjson("{ok:1, dataSize: 30}"));

    TenantMigrationSharedData resumingSharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeAllDatabaseCloner(&resumingSharedData);
    cloner->setStopAfterStage_forTest("initializeStatsStage");

    ASSERT_OK(cloner->run());

    // Nothing to clone.
    auto databases = getDatabasesFromCloner(cloner.get());
    ASSERT_EQUALS(0u, databases.size());

    auto stats = cloner->getStats();
    ASSERT_EQUALS(3, stats.databasesClonedBeforeFailover);
    ASSERT_EQUALS(size, stats.approxTotalBytesCopied);
    ASSERT_EQUALS(stats.approxTotalBytesCopied, stats.approxTotalDataSize);
}

}  // namespace repl
}  // namespace mongo
