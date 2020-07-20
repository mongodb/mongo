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

#include "mongo/platform/basic.h"

#include "mongo/db/clientcursor.h"
#include "mongo/db/repl/cloner_test_fixture.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/tenant_database_cloner.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace repl {

struct CollectionCloneInfo {
    std::shared_ptr<CollectionMockStats> stats = std::make_shared<CollectionMockStats>();
    CollectionBulkLoaderMock* loader = nullptr;
};

class TenantDatabaseClonerTest : public ClonerTestFixture {
public:
    TenantDatabaseClonerTest() {}

protected:
    void setUp() override {
        ClonerTestFixture::setUp();
        _storageInterface.createCollectionForBulkFn =
            [this](const NamespaceString& nss,
                   const CollectionOptions& options,
                   const BSONObj& idIndexSpec,
                   const std::vector<BSONObj>& secondaryIndexSpecs)
            -> StatusWith<std::unique_ptr<CollectionBulkLoaderMock>> {
            const auto collInfo = &_collections[nss];

            auto localLoader = std::make_unique<CollectionBulkLoaderMock>(collInfo->stats);
            auto status = localLoader->init(secondaryIndexSpecs);
            if (!status.isOK())
                return status;
            collInfo->loader = localLoader.get();

            return std::move(localLoader);
        };
        setInitialSyncId();
        _mockClient->setOperationTime(_operationTime);
    }

    std::unique_ptr<TenantDatabaseCloner> makeDatabaseCloner() {
        return std::make_unique<TenantDatabaseCloner>(_dbName,
                                                      _sharedData.get(),
                                                      _source,
                                                      _mockClient.get(),
                                                      &_storageInterface,
                                                      _dbWorkThreadPool.get());
    }

    BSONObj createListCollectionsResponse(const std::vector<BSONObj>& collections) {
        auto ns = _dbName + "$cmd.listCollections";
        BSONObjBuilder bob;
        {
            BSONObjBuilder cursorBob(bob.subobjStart("cursor"));
            cursorBob.append("id", CursorId(0));
            cursorBob.append("ns", ns);
            {
                BSONArrayBuilder batchBob(cursorBob.subarrayStart("firstBatch"));
                for (const auto& coll : collections) {
                    batchBob.append(coll);
                }
            }
        }
        bob.append("ok", 1);
        bob.append("operationTime", _operationTime);
        return bob.obj();
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

    std::vector<std::pair<NamespaceString, CollectionOptions>> getCollectionsFromCloner(
        TenantDatabaseCloner* cloner) {
        return cloner->_collections;
    }

    std::map<NamespaceString, CollectionCloneInfo> _collections;

    static std::string _dbName;
    static Timestamp _operationTime;
};

/* static */
std::string TenantDatabaseClonerTest::_dbName = "testDb";
Timestamp TenantDatabaseClonerTest::_operationTime = Timestamp(12345, 42);

// A database may have no collections. Nothing to do for the tenant database cloner.
TEST_F(TenantDatabaseClonerTest, ListCollectionsReturnedNoCollections) {
    _mockServer->setCommandReply("listCollections", createListCollectionsResponse({}));
    _mockServer->setCommandReply("find", createFindResponse());
    auto cloner = makeDatabaseCloner();

    ASSERT_OK(cloner->run());
    ASSERT_OK(_sharedData->getInitialSyncStatus(WithLock::withoutLock()));
    ASSERT(getCollectionsFromCloner(cloner.get()).empty());
}

TEST_F(TenantDatabaseClonerTest, ListCollections) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid" << uuid1)),
                                              BSON(
                                                  "name"
                                                  << "b"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid2))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    _mockServer->setCommandReply("find", createFindResponse());

    ASSERT_OK(cloner->run());
    ASSERT_OK(_sharedData->getInitialSyncStatus(WithLock::withoutLock()));
    auto collections = getCollectionsFromCloner(cloner.get());

    ASSERT_EQUALS(2U, collections.size());
    ASSERT_EQ(NamespaceString(_dbName, "a"), collections[0].first);
    ASSERT_BSONOBJ_EQ(BSON("uuid" << uuid1), collections[0].second.toBSON());
    ASSERT_EQ(NamespaceString(_dbName, "b"), collections[1].first);
    ASSERT_BSONOBJ_EQ(BSON("uuid" << uuid2), collections[1].second.toBSON());
}

// The listCollections command may return new fields in later versions; we do not want that
// to cause upgrade/downgrade issues.
TEST_F(TenantDatabaseClonerTest, ListCollectionsAllowsExtraneousFields) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   // The "flavor" field is not really found in
                                                   // listCollections.
                                                   << "flavor"
                                                   << "raspberry"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid" << uuid1)),
                                              BSON("name"
                                                   << "b"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj()
                                                   << "info"
                                                   // The "comet" field is not really found in
                                                   // listCollections.
                                                   << BSON("readOnly" << false << "uuid" << uuid2
                                                                      << "comet"
                                                                      << "2l_Borisov"))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    _mockServer->setCommandReply("find", createFindResponse());

    ASSERT_OK(cloner->run());
    ASSERT_OK(_sharedData->getInitialSyncStatus(WithLock::withoutLock()));
    auto collections = getCollectionsFromCloner(cloner.get());

    ASSERT_EQUALS(2U, collections.size());
    ASSERT_EQ(NamespaceString(_dbName, "a"), collections[0].first);
    ASSERT_BSONOBJ_EQ(BSON("uuid" << uuid1), collections[0].second.toBSON());
    ASSERT_EQ(NamespaceString(_dbName, "b"), collections[1].first);
    ASSERT_BSONOBJ_EQ(BSON("uuid" << uuid2), collections[1].second.toBSON());
}

TEST_F(TenantDatabaseClonerTest, ListCollectionsFailsOnDuplicateNames) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid" << uuid1)),
                                              BSON(
                                                  "name"
                                                  << "a"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid2))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    _mockServer->setCommandReply("find", createFindResponse());

    auto status = cloner->run();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(4881604, status.code());
}

TEST_F(TenantDatabaseClonerTest, ListCollectionsFailsOnMissingNameField) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid" << uuid1)),
                                              BSON(
                                                  "name"
                                                  << "b"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid2))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    _mockServer->setCommandReply("find", createFindResponse());

    auto status = cloner->run();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
}

TEST_F(TenantDatabaseClonerTest, ListCollectionsFailsOnMissingOptions) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"),
                                              BSON(
                                                  "name"
                                                  << "a"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid1))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    _mockServer->setCommandReply("find", createFindResponse());

    auto status = cloner->run();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
}

TEST_F(TenantDatabaseClonerTest, ListCollectionsFailsOnMissingUUID) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false)),
                                              BSON(
                                                  "name"
                                                  << "b"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid1))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    _mockServer->setCommandReply("find", createFindResponse());

    auto status = cloner->run();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
}

TEST_F(TenantDatabaseClonerTest, ListCollectionsFailsOnInvalidCollectionOptions) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const std::vector<BSONObj>
        sourceInfos = {BSON("name"
                            << "a"
                            << "type"
                            << "collection"
                            << "options" << BSONObj() << "info"
                            << BSON("readOnly" << false << "uuid" << uuid1)),
                       BSON("name"
                            << "b"
                            << "type"
                            << "collection"
                            // "storageEngine" is not an integer collection option.
                            << "options" << BSON("storageEngine" << 1) << "info"
                            << BSON("readOnly" << false << "uuid" << uuid2))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    _mockServer->setCommandReply("find", createFindResponse());

    auto status = cloner->run();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
}

TEST_F(TenantDatabaseClonerTest, ListCollectionsMajorityReadFailsWithSpecificError) {

    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid" << uuid1)),
                                              BSON(
                                                  "name"
                                                  << "b"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid2))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    _mockServer->setCommandReply("find", createFindResponse(ErrorCodes::OperationFailed));

    auto status = cloner->run();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status.code());
}

TEST_F(TenantDatabaseClonerTest, ListCollectionsRemoteUnreachableBeforeMajorityFind) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid" << uuid1)),
                                              BSON(
                                                  "name"
                                                  << "b"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid2))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));

    auto clonerOperationTimeFP =
        globalFailPointRegistry().find("tenantDatabaseClonerHangAfterGettingOperationTime");
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

TEST_F(TenantDatabaseClonerTest, ListCollectionsRecordsCorrectOperationTime) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid" << uuid1)),
                                              BSON(
                                                  "name"
                                                  << "b"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid2))};

    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    _mockServer->setCommandReply("find", createFindResponse());

    auto clonerOperationTimeFP =
        globalFailPointRegistry().find("tenantDatabaseClonerHangAfterGettingOperationTime");
    auto timesEntered = clonerOperationTimeFP->setMode(FailPoint::alwaysOn, 0);

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
}

// TODO(SERVER-48845): Restore the below tests.

// TEST_F(TenantDatabaseClonerTest, FirstCollectionListIndexesFailed) {
//     auto uuid1 = UUID::gen();
//     auto uuid2 = UUID::gen();
//     const BSONObj idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
//                                          << "_id_");
//     const std::vector<BSONObj> sourceInfos = {BSON("name"
//                                                    << "a"
//                                                    << "type"
//                                                    << "collection"
//                                                    << "options" << BSONObj() << "info"
//                                                    << BSON("readOnly" << false << "uuid" <<
//                                                    uuid1)),
//                                               BSON(
//                                                   "name"
//                                                   << "b"
//                                                   << "type"
//                                                   << "collection"
//                                                   << "options" << BSONObj() << "info"
//                                                   << BSON("readOnly" << false << "uuid" <<
//                                                   uuid2))};
//     _mockServer->setCommandReply("listCollections",
//                                  createListCollectionsResponse({sourceInfos[0],
//                                  sourceInfos[1]}));
//     _mockServer->setCommandReply("find", createFindResponse());
//     _mockServer->setCommandReply("count", {createCountResponse(0), createCountResponse(0)});
//     _mockServer->setCommandReply("listIndexes",
//                                  {BSON("ok" << 0 << "errmsg"
//                                             << "fake message"
//                                             << "code" << ErrorCodes::CursorNotFound),
//                                   createCursorResponse(_dbName + ".b",
//                                   BSON_ARRAY(idIndexSpec))});
//     auto cloner = makeDatabaseCloner();
//     auto status = cloner->run();
//     ASSERT_NOT_OK(status);

//     ASSERT_EQ(status.code(), ErrorCodes::InitialSyncFailure);
//     ASSERT_EQUALS(0u, _collections.size());
// }

// TEST_F(TenantDatabaseClonerTest, CreateCollections) {
//     auto uuid1 = UUID::gen();
//     auto uuid2 = UUID::gen();
//     const BSONObj idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
//                                          << "_id_");
//     const std::vector<BSONObj> sourceInfos = {BSON("name"
//                                                    << "a"
//                                                    << "type"
//                                                    << "collection"
//                                                    << "options" << BSONObj() << "info"
//                                                    << BSON("readOnly" << false << "uuid" <<
//                                                    uuid1)),
//                                               BSON(
//                                                   "name"
//                                                   << "b"
//                                                   << "type"
//                                                   << "collection"
//                                                   << "options" << BSONObj() << "info"
//                                                   << BSON("readOnly" << false << "uuid" <<
//                                                   uuid2))};
//     _mockServer->setCommandReply("listCollections",
//                                  createListCollectionsResponse({sourceInfos[0],
//                                  sourceInfos[1]}));
//     _mockServer->setCommandReply("find", createFindResponse());
//     _mockServer->setCommandReply("count", {createCountResponse(0), createCountResponse(0)});
//     _mockServer->setCommandReply("listIndexes",
//                                  {createCursorResponse(_dbName + ".a", BSON_ARRAY(idIndexSpec)),
//                                   createCursorResponse(_dbName + ".b",
//                                   BSON_ARRAY(idIndexSpec))});
//     auto cloner = makeDatabaseCloner();
//     auto status = cloner->run();
//     ASSERT_OK(status);

//     ASSERT_EQUALS(2U, _collections.size());

//     auto collInfo = _collections[NamespaceString{_dbName, "a"}];
//     auto stats = *collInfo.stats;
//     ASSERT_EQUALS(0, stats.insertCount);
//     ASSERT(stats.commitCalled);

//     collInfo = _collections[NamespaceString{_dbName, "b"}];
//     stats = *collInfo.stats;
//     ASSERT_EQUALS(0, stats.insertCount);
//     ASSERT(stats.commitCalled);
// }

// TEST_F(TenantDatabaseClonerTest, DatabaseAndCollectionStats) {
//     auto uuid1 = UUID::gen();
//     auto uuid2 = UUID::gen();
//     const BSONObj idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
//                                          << "_id_");
//     const BSONObj extraIndexSpec = BSON("v" << 1 << "key" << BSON("x" << 1) << "name"
//                                             << "_extra_");
//     const std::vector<BSONObj> sourceInfos = {BSON("name"
//                                                    << "a"
//                                                    << "type"
//                                                    << "collection"
//                                                    << "options" << BSONObj() << "info"
//                                                    << BSON("readOnly" << false << "uuid" <<
//                                                    uuid1)),
//                                               BSON(
//                                                   "name"
//                                                   << "b"
//                                                   << "type"
//                                                   << "collection"
//                                                   << "options" << BSONObj() << "info"
//                                                   << BSON("readOnly" << false << "uuid" <<
//                                                   uuid2))};
//     _mockServer->setCommandReply("listCollections",
//                                  createListCollectionsResponse({sourceInfos[0],
//                                  sourceInfos[1]}));
//     _mockServer->setCommandReply("find", createFindResponse());
//     _mockServer->setCommandReply("count", {createCountResponse(0), createCountResponse(0)});
//     _mockServer->setCommandReply(
//         "listIndexes",
//         {createCursorResponse(_dbName + ".a", BSON_ARRAY(idIndexSpec << extraIndexSpec)),
//          createCursorResponse(_dbName + ".b", BSON_ARRAY(idIndexSpec))});
//     auto cloner = makeDatabaseCloner();

//     auto collClonerBeforeFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
//     auto collClonerAfterFailPoint = globalFailPointRegistry().find("hangAfterClonerStage");
//     auto timesEntered = collClonerBeforeFailPoint->setMode(
//         FailPoint::alwaysOn,
//         0,
//         fromjson("{cloner: 'CollectionCloner', stage: 'count', nss: '" + _dbName + ".a'}"));
//     collClonerAfterFailPoint->setMode(
//         FailPoint::alwaysOn,
//         0,
//         fromjson("{cloner: 'CollectionCloner', stage: 'count', nss: '" + _dbName + ".a'}"));

//     // Run the cloner in a separate thread.
//     stdx::thread clonerThread([&] {
//         Client::initThread("ClonerRunner");
//         ASSERT_OK(cloner->run());
//     });
//     // Wait for the failpoint to be reached
//     collClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);

//     // Collection stats should be set up with namespace.
//     auto stats = cloner->getStats();
//     ASSERT_EQ(_dbName, stats.dbname);
//     ASSERT_EQ(_clock.now(), stats.start);
//     ASSERT_EQ(2, stats.collections);
//     ASSERT_EQ(0, stats.clonedCollections);
//     ASSERT_EQ(2, stats.collectionStats.size());
//     ASSERT_EQ(_dbName + ".a", stats.collectionStats[0].ns);
//     ASSERT_EQ(_dbName + ".b", stats.collectionStats[1].ns);
//     ASSERT_EQ(_clock.now(), stats.collectionStats[0].start);
//     ASSERT_EQ(Date_t(), stats.collectionStats[0].end);
//     ASSERT_EQ(Date_t(), stats.collectionStats[1].start);
//     ASSERT_EQ(0, stats.collectionStats[0].indexes);
//     ASSERT_EQ(0, stats.collectionStats[1].indexes);
//     _clock.advance(Minutes(1));

//     // Move to the next collection
//     timesEntered = collClonerBeforeFailPoint->setMode(
//         FailPoint::alwaysOn,
//         0,
//         fromjson("{cloner: 'CollectionCloner', stage: 'count', nss: '" + _dbName + ".b'}"));
//     collClonerAfterFailPoint->setMode(FailPoint::off);

//     // Wait for the failpoint to be reached
//     collClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);

//     stats = cloner->getStats();
//     ASSERT_EQ(2, stats.collections);
//     ASSERT_EQ(1, stats.clonedCollections);
//     ASSERT_EQ(2, stats.collectionStats.size());
//     ASSERT_EQ(_dbName + ".a", stats.collectionStats[0].ns);
//     ASSERT_EQ(_dbName + ".b", stats.collectionStats[1].ns);
//     ASSERT_EQ(2, stats.collectionStats[0].indexes);
//     ASSERT_EQ(0, stats.collectionStats[1].indexes);
//     ASSERT_EQ(_clock.now(), stats.collectionStats[0].end);
//     ASSERT_EQ(_clock.now(), stats.collectionStats[1].start);
//     ASSERT_EQ(Date_t(), stats.collectionStats[1].end);
//     _clock.advance(Minutes(1));

//     // Finish
//     collClonerBeforeFailPoint->setMode(FailPoint::off, 0);
//     clonerThread.join();

//     stats = cloner->getStats();
//     ASSERT_EQ(_dbName, stats.dbname);
//     ASSERT_EQ(_clock.now(), stats.end);
//     ASSERT_EQ(2, stats.collections);
//     ASSERT_EQ(2, stats.clonedCollections);
//     ASSERT_EQ(2, stats.collectionStats.size());
//     ASSERT_EQ(_dbName + ".a", stats.collectionStats[0].ns);
//     ASSERT_EQ(_dbName + ".b", stats.collectionStats[1].ns);
//     ASSERT_EQ(2, stats.collectionStats[0].indexes);
//     ASSERT_EQ(1, stats.collectionStats[1].indexes);
//     ASSERT_EQ(_clock.now(), stats.collectionStats[1].end);
// }

}  // namespace repl
}  // namespace mongo
