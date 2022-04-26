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
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/tenant_cloner_test_fixture.h"
#include "mongo/db/repl/tenant_database_cloner.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace repl {

struct TenantCollectionCloneInfo {
    size_t numDocsInserted{0};
    bool collCreated = false;
};

class TenantDatabaseClonerTest : public TenantClonerTestFixture {
public:
    TenantDatabaseClonerTest() {}

protected:
    void setUp() override {
        TenantClonerTestFixture::setUp();
        _storageInterface.createCollFn = [this](OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const CollectionOptions& options) -> Status {
            const auto collInfo = &_collections[nss];
            collInfo->collCreated = true;
            collInfo->numDocsInserted = 0;
            return Status::OK();
        };
        _storageInterface.createIndexesOnEmptyCollFn =
            [this](OperationContext* opCtx,
                   const NamespaceString& nss,
                   const std::vector<BSONObj>& secondaryIndexSpecs) -> Status {
            return Status::OK();
        };
        _storageInterface.insertDocumentsFn = [this](OperationContext* opCtx,
                                                     const NamespaceStringOrUUID& nsOrUUID,
                                                     const std::vector<InsertStatement>& ops) {
            const auto collInfo = &_collections[nsOrUUID.nss().get()];
            collInfo->numDocsInserted += ops.size();
            return Status::OK();
        };
        _mockClient->setOperationTime(_operationTime);
        _mockServer->setCommandReply("collStats", BSON("size" << 1));
    }

    std::unique_ptr<TenantDatabaseCloner> makeDatabaseCloner(
        TenantMigrationSharedData* sharedData = nullptr) {
        return std::make_unique<TenantDatabaseCloner>(_dbName,
                                                      sharedData ? sharedData : getSharedData(),
                                                      _source,
                                                      _mockClient.get(),
                                                      &_storageInterface,
                                                      _dbWorkThreadPool.get(),
                                                      _tenantId);
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

    std::map<NamespaceString, TenantCollectionCloneInfo> _collections;

    const std::string _dbName = _tenantId + "_testDb";
};

// A database may have no collections. Nothing to do for the tenant database cloner.
TEST_F(TenantDatabaseClonerTest, ListCollectionsReturnedNoCollections) {
    _mockServer->setCommandReply("listCollections", createListCollectionsResponse({}));
    _mockServer->setCommandReply("find", createFindResponse());
    auto cloner = makeDatabaseCloner();

    ASSERT_OK(cloner->run());
    ASSERT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
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
    ASSERT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
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
    ASSERT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
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

TEST_F(TenantDatabaseClonerTest, FirstCollectionListIndexesFailed) {
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const BSONObj idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                         << "_id_");
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
    _mockServer->setCommandReply("count", {createCountResponse(0), createCountResponse(0)});
    _mockServer->setCommandReply("listIndexes",
                                 {BSON("ok" << 0 << "errmsg"
                                            << "fake message"
                                            << "code" << ErrorCodes::CursorNotFound),
                                  createCursorResponse(_dbName + ".b", BSON_ARRAY(idIndexSpec))});
    auto cloner = makeDatabaseCloner();
    auto status = cloner->run();
    ASSERT_NOT_OK(status);

    ASSERT_EQ(status.code(), ErrorCodes::CursorNotFound);
    ASSERT_EQUALS(0u, _collections.size());
}

TEST_F(TenantDatabaseClonerTest, CreateCollections) {
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const BSONObj idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                         << "_id_");
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
    _mockServer->setCommandReply("count", {createCountResponse(0), createCountResponse(0)});
    _mockServer->setCommandReply("listIndexes",
                                 {createCursorResponse(_dbName + ".a", BSON_ARRAY(idIndexSpec)),
                                  createCursorResponse(_dbName + ".b", BSON_ARRAY(idIndexSpec))});
    auto cloner = makeDatabaseCloner();
    auto status = cloner->run();
    ASSERT_OK(status);

    ASSERT_EQUALS(2U, _collections.size());

    auto collInfo = _collections[NamespaceString{_dbName, "a"}];
    ASSERT(collInfo.collCreated);
    ASSERT_EQUALS(0, collInfo.numDocsInserted);

    collInfo = _collections[NamespaceString{_dbName, "b"}];
    ASSERT(collInfo.collCreated);
    ASSERT_EQUALS(0, collInfo.numDocsInserted);
}

TEST_F(TenantDatabaseClonerTest, DatabaseAndCollectionStats) {
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const BSONObj idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                         << "_id_");
    const BSONObj extraIndexSpec = BSON("v" << 1 << "key" << BSON("x" << 1) << "name"
                                            << "_extra_");
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
    _mockServer->setCommandReply("count", {createCountResponse(0), createCountResponse(0)});
    _mockServer->setCommandReply(
        "listIndexes",
        {createCursorResponse(_dbName + ".a", BSON_ARRAY(idIndexSpec << extraIndexSpec)),
         createCursorResponse(_dbName + ".b", BSON_ARRAY(idIndexSpec))});
    auto cloner = makeDatabaseCloner();

    auto collClonerBeforeFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto collClonerAfterFailPoint = globalFailPointRegistry().find("hangAfterClonerStage");
    auto timesEntered = collClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'TenantCollectionCloner', stage: 'count', nss: '" + _dbName + ".a'}"));
    collClonerAfterFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'TenantCollectionCloner', stage: 'count', nss: '" + _dbName + ".a'}"));

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_OK(cloner->run());
    });
    // Wait for the failpoint to be reached
    collClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);

    // Collection stats should be set up with namespace.
    auto stats = cloner->getStats();
    ASSERT_EQ(_dbName, stats.dbname);
    ASSERT_EQ(_clock.now(), stats.start);
    ASSERT_EQ(2, stats.collections);
    ASSERT_EQ(0, stats.clonedCollections);
    ASSERT_EQ(2, stats.collectionStats.size());
    ASSERT_EQ(_dbName + ".a", stats.collectionStats[0].ns);
    ASSERT_EQ(_dbName + ".b", stats.collectionStats[1].ns);
    ASSERT_EQ(_clock.now(), stats.collectionStats[0].start);
    ASSERT_EQ(Date_t(), stats.collectionStats[0].end);
    ASSERT_EQ(Date_t(), stats.collectionStats[1].start);
    ASSERT_EQ(0, stats.collectionStats[0].indexes);
    ASSERT_EQ(0, stats.collectionStats[1].indexes);
    _clock.advance(Minutes(1));

    // Move to the next collection
    timesEntered = collClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'TenantCollectionCloner', stage: 'count', nss: '" + _dbName + ".b'}"));
    collClonerAfterFailPoint->setMode(FailPoint::off);

    // Wait for the failpoint to be reached
    collClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);

    stats = cloner->getStats();
    ASSERT_EQ(2, stats.collections);
    ASSERT_EQ(1, stats.clonedCollections);
    ASSERT_EQ(2, stats.collectionStats.size());
    ASSERT_EQ(_dbName + ".a", stats.collectionStats[0].ns);
    ASSERT_EQ(_dbName + ".b", stats.collectionStats[1].ns);
    ASSERT_EQ(2, stats.collectionStats[0].indexes);
    ASSERT_EQ(0, stats.collectionStats[1].indexes);
    ASSERT_EQ(_clock.now(), stats.collectionStats[0].end);
    ASSERT_EQ(_clock.now(), stats.collectionStats[1].start);
    ASSERT_EQ(Date_t(), stats.collectionStats[1].end);
    _clock.advance(Minutes(1));

    // Finish
    collClonerBeforeFailPoint->setMode(FailPoint::off, 0);
    clonerThread.join();

    stats = cloner->getStats();
    ASSERT_EQ(_dbName, stats.dbname);
    ASSERT_EQ(_clock.now(), stats.end);
    ASSERT_EQ(2, stats.collections);
    ASSERT_EQ(2, stats.clonedCollections);
    ASSERT_EQ(2, stats.collectionStats.size());
    ASSERT_EQ(_dbName + ".a", stats.collectionStats[0].ns);
    ASSERT_EQ(_dbName + ".b", stats.collectionStats[1].ns);
    ASSERT_EQ(2, stats.collectionStats[0].indexes);
    ASSERT_EQ(1, stats.collectionStats[1].indexes);
    ASSERT_EQ(_clock.now(), stats.collectionStats[1].end);
}

TEST_F(TenantDatabaseClonerTest, TenantCollectionsAlreadyExist) {
    auto uuid = UUID::gen();

    CollectionOptions options;
    options.uuid = uuid;
    ASSERT_OK(createCollection(NamespaceString(_dbName, "a"), options));

    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listExistingCollections");

    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid" << uuid))};
    _mockServer->setCommandReply("listCollections", createListCollectionsResponse(sourceInfos));
    _mockServer->setCommandReply("find", createFindResponse());

    ASSERT_NOT_OK(cloner->run());
    ASSERT_NOT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
}

TEST_F(TenantDatabaseClonerTest, ResumingFromLastClonedCollection) {
    // Test that database cloner correctly resume from the last cloned collection.
    std::vector<UUID> uuid;
    uuid.push_back(UUID::gen());
    uuid.push_back(UUID::gen());
    std::sort(uuid.begin(), uuid.end());

    auto aNss = NamespaceString(_dbName, "a");
    auto bNss = NamespaceString(_dbName, "b");
    CollectionOptions options;
    options.uuid = uuid[0];
    ASSERT_OK(createCollection(aNss, options));
    options.uuid = uuid[1];
    ASSERT_OK(createCollection(bNss, options));

    long long sizeOfOneCollection = 0;
    {
        // Insert documents into collections.
        auto storage = StorageInterface::get(serviceContext);
        auto opCtx = cc().makeOperationContext();

        ASSERT_OK(storage->insertDocument(
            opCtx.get(), aNss, {BSON("_id" << 0 << "a" << 1001), Timestamp(0)}, 0));
        ASSERT_OK(storage->insertDocument(opCtx.get(),
                                          bNss,
                                          {BSON("_id" << 0 << "a"
                                                      << "hello"),
                                           Timestamp(0)},
                                          0));

        auto swSize = storage->getCollectionSize(opCtx.get(), aNss);
        ASSERT_OK(swSize.getStatus());
        sizeOfOneCollection = swSize.getValue();
    }

    TenantMigrationSharedData resumingSharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeDatabaseCloner(&resumingSharedData);
    cloner->setStopAfterStage_forTest("listExistingCollections");

    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid"
                                                                      << uuid[0])),
                                              BSON("name"
                                                   << "b"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid"
                                                                      << uuid[1]))};
    _mockServer->setCommandReply("listCollections", createListCollectionsResponse(sourceInfos));
    _mockServer->setCommandReply("find", createFindResponse());

    ASSERT_OK(cloner->run());
    ASSERT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
    auto collections = getCollectionsFromCloner(cloner.get());

    ASSERT_EQUALS(1U, collections.size());
    ASSERT_EQ(NamespaceString(_dbName, "b"), collections[0].first);
    ASSERT_BSONOBJ_EQ(BSON("uuid" << uuid[1]), collections[0].second.toBSON());

    auto stats = cloner->getStats();
    ASSERT_EQUALS(1, stats.clonedCollectionsBeforeFailover);
    ASSERT_EQUALS(sizeOfOneCollection, stats.approxTotalBytesCopied);
}

TEST_F(TenantDatabaseClonerTest, LastClonedCollectionDeleted_AllGreater) {
    // Test that we correctly resume from next collection whose UUID compared greater than the last
    // cloned collection if the last cloned collection is dropped. This tests the case when all
    // collections in the latest listCollections result are compared greater than the last cloned
    // collection.
    std::vector<UUID> uuid;
    uuid.push_back(UUID::gen());
    uuid.push_back(UUID::gen());
    uuid.push_back(UUID::gen());
    std::sort(uuid.begin(), uuid.end());

    auto aNss = NamespaceString(_dbName, "a");
    CollectionOptions options;
    options.uuid = uuid[0];
    ASSERT_OK(createCollection(aNss, options));

    long long sizeANss = 0;
    {
        // Insert documents into collections.
        auto storage = StorageInterface::get(serviceContext);
        auto opCtx = cc().makeOperationContext();

        ASSERT_OK(storage->insertDocument(
            opCtx.get(), aNss, {BSON("_id" << 0 << "a" << 1001), Timestamp(0)}, 0));

        auto swSize = storage->getCollectionSize(opCtx.get(), aNss);
        ASSERT_OK(swSize.getStatus());
        sizeANss = swSize.getValue();
    }

    TenantMigrationSharedData resumingSharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeDatabaseCloner(&resumingSharedData);
    cloner->setStopAfterStage_forTest("listExistingCollections");

    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "b"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid"
                                                                      << uuid[1])),
                                              BSON("name"
                                                   << "c"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid"
                                                                      << uuid[2]))};
    _mockServer->setCommandReply("listCollections", createListCollectionsResponse(sourceInfos));
    _mockServer->setCommandReply("find", createFindResponse());

    ASSERT_OK(cloner->run());
    ASSERT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
    auto collections = getCollectionsFromCloner(cloner.get());

    ASSERT_EQUALS(2U, collections.size());
    ASSERT_EQ(NamespaceString(_dbName, "b"), collections[0].first);
    ASSERT_BSONOBJ_EQ(BSON("uuid" << uuid[1]), collections[0].second.toBSON());
    ASSERT_EQ(NamespaceString(_dbName, "c"), collections[1].first);
    ASSERT_BSONOBJ_EQ(BSON("uuid" << uuid[2]), collections[1].second.toBSON());

    auto stats = cloner->getStats();
    ASSERT_EQUALS(1, stats.clonedCollectionsBeforeFailover);
    ASSERT_EQUALS(sizeANss, stats.approxTotalBytesCopied);
}

TEST_F(TenantDatabaseClonerTest, LastClonedCollectionDeleted_SomeGreater) {
    // Test that we correctly resume from next collection whose UUID compared greater than the last
    // cloned collection if the last cloned collection is dropped. This tests the case when some but
    // not all collections in the latest listCollections result are compared greater than the last
    // cloned collection.
    std::vector<UUID> uuid;
    uuid.push_back(UUID::gen());
    uuid.push_back(UUID::gen());
    uuid.push_back(UUID::gen());
    std::sort(uuid.begin(), uuid.end());

    auto aNss = NamespaceString(_dbName, "a");
    auto bNss = NamespaceString(_dbName, "b");
    CollectionOptions options;
    options.uuid = uuid[0];
    ASSERT_OK(createCollection(aNss, options));
    options.uuid = uuid[1];
    ASSERT_OK(createCollection(bNss, options));

    long long ANssBNssSize = 0;
    {
        // Insert some documents into both collections.
        auto storage = StorageInterface::get(serviceContext);
        auto opCtx = cc().makeOperationContext();

        ASSERT_OK(storage->insertDocument(
            opCtx.get(), aNss, {BSON("_id" << 0 << "a" << 1001), Timestamp(0)}, 0));
        ASSERT_OK(storage->insertDocument(opCtx.get(),
                                          bNss,
                                          {BSON("_id" << 0 << "a"
                                                      << "hello"),
                                           Timestamp(0)},
                                          0));

        auto swSizeANss = storage->getCollectionSize(opCtx.get(), aNss);
        ASSERT_OK(swSizeANss.getStatus());
        ANssBNssSize = swSizeANss.getValue();

        auto swSizeBNss = storage->getCollectionSize(opCtx.get(), bNss);
        ASSERT_OK(swSizeBNss.getStatus());
        ANssBNssSize += swSizeBNss.getValue();
    }

    TenantMigrationSharedData resumingSharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeDatabaseCloner(&resumingSharedData);
    cloner->setStopAfterStage_forTest("listExistingCollections");

    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid"
                                                                      << uuid[0])),
                                              BSON("name"
                                                   << "c"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid"
                                                                      << uuid[2]))};
    _mockServer->setCommandReply("listCollections", createListCollectionsResponse(sourceInfos));
    _mockServer->setCommandReply("find", createFindResponse());

    ASSERT_OK(cloner->run());
    ASSERT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
    auto collections = getCollectionsFromCloner(cloner.get());

    ASSERT_EQUALS(1U, collections.size());
    ASSERT_EQ(NamespaceString(_dbName, "c"), collections[0].first);
    ASSERT_BSONOBJ_EQ(BSON("uuid" << uuid[2]), collections[0].second.toBSON());

    auto stats = cloner->getStats();
    ASSERT_EQUALS(2, stats.clonedCollectionsBeforeFailover);
    ASSERT_EQUALS(ANssBNssSize, stats.approxTotalBytesCopied);
}

TEST_F(TenantDatabaseClonerTest, LastClonedCollectionDeleted_AllLess) {
    // Test that we correctly resume from next collection whose UUID compared greater than the last
    // cloned collection if the last cloned collection is dropped. This tests the case when all
    // collections in the latest listCollections result are compared less than the last cloned
    // collection.
    std::vector<UUID> uuid;
    uuid.push_back(UUID::gen());
    uuid.push_back(UUID::gen());
    uuid.push_back(UUID::gen());
    std::sort(uuid.begin(), uuid.end());

    auto aNss = NamespaceString(_dbName, "a");
    auto bNss = NamespaceString(_dbName, "b");
    auto cNss = NamespaceString(_dbName, "c");
    CollectionOptions options;
    options.uuid = uuid[0];
    ASSERT_OK(createCollection(aNss, options));
    options.uuid = uuid[1];
    ASSERT_OK(createCollection(bNss, options));
    options.uuid = uuid[2];
    ASSERT_OK(createCollection(cNss, options));

    long long sizeOfAllColls = 0;
    {
        // Insert some documents into all three collections.
        auto storage = StorageInterface::get(serviceContext);
        auto opCtx = cc().makeOperationContext();

        ASSERT_OK(storage->insertDocument(
            opCtx.get(), aNss, {BSON("_id" << 0 << "a" << 1001), Timestamp(0)}, 0));
        ASSERT_OK(storage->insertDocument(opCtx.get(),
                                          bNss,
                                          {BSON("_id" << 0 << "a"
                                                      << "hello"),
                                           Timestamp(0)},
                                          0));
        ASSERT_OK(storage->insertDocument(opCtx.get(),
                                          cNss,
                                          {BSON("_id" << 0 << "a"
                                                      << "goodbye"),
                                           Timestamp(0)},
                                          0));

        auto swSizeANss = storage->getCollectionSize(opCtx.get(), aNss);
        ASSERT_OK(swSizeANss.getStatus());
        sizeOfAllColls = swSizeANss.getValue();

        auto swSizeBNss = storage->getCollectionSize(opCtx.get(), bNss);
        ASSERT_OK(swSizeBNss.getStatus());
        sizeOfAllColls += swSizeBNss.getValue();

        auto swSizeCNss = storage->getCollectionSize(opCtx.get(), cNss);
        ASSERT_OK(swSizeCNss.getStatus());
        sizeOfAllColls += swSizeCNss.getValue();
    }

    TenantMigrationSharedData resumingSharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeDatabaseCloner(&resumingSharedData);
    cloner->setStopAfterStage_forTest("listExistingCollections");

    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid"
                                                                      << uuid[0])),
                                              BSON("name"
                                                   << "b"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid"
                                                                      << uuid[1]))};
    _mockServer->setCommandReply("listCollections", createListCollectionsResponse(sourceInfos));
    _mockServer->setCommandReply("find", createFindResponse());

    ASSERT_OK(cloner->run());
    ASSERT_OK(getSharedData()->getStatus(WithLock::withoutLock()));
    auto collections = getCollectionsFromCloner(cloner.get());

    // Nothing to clone.
    ASSERT_EQUALS(0U, collections.size());

    auto stats = cloner->getStats();
    ASSERT_EQUALS(3, stats.clonedCollectionsBeforeFailover);
    ASSERT_EQUALS(sizeOfAllColls, stats.approxTotalBytesCopied);
}

}  // namespace repl
}  // namespace mongo
