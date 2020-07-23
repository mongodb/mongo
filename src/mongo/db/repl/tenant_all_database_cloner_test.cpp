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

#include "mongo/db/repl/cloner_test_fixture.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/tenant_all_database_cloner.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace repl {

class TenantAllDatabaseClonerTest : public ClonerTestFixture {
public:
    TenantAllDatabaseClonerTest() {}

protected:
    void setUp() override {
        ClonerTestFixture::setUp();
        _mockClient->setOperationTime(_operationTime);
    }

    std::unique_ptr<TenantAllDatabaseCloner> makeAllDatabaseCloner() {
        return std::make_unique<TenantAllDatabaseCloner>(_sharedData.get(),
                                                         _source,
                                                         _mockClient.get(),
                                                         &_storageInterface,
                                                         _dbWorkThreadPool.get(),
                                                         _databasePrefix);
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

    static Timestamp _operationTime;
    static std::string _databasePrefix;
    static std::string _tenantDbA;
    static std::string _tenantDbAAB;
    static std::string _tenantDbABC;
    static std::string _tenantDbB;
};

/* static */
Timestamp TenantAllDatabaseClonerTest::_operationTime = Timestamp(12345, 67);
std::string TenantAllDatabaseClonerTest::_databasePrefix = "tenant42";
std::string TenantAllDatabaseClonerTest::_tenantDbA = _databasePrefix + "_a";
std::string TenantAllDatabaseClonerTest::_tenantDbAAB = _databasePrefix + "_aab";
std::string TenantAllDatabaseClonerTest::_tenantDbABC = _databasePrefix + "_abc";
std::string TenantAllDatabaseClonerTest::_tenantDbB = _databasePrefix + "_b";

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

}  // namespace repl
}  // namespace mongo