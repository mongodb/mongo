/**
 *    Copyright 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include <list>
#include <memory>
#include <utility>

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/base_cloner_test_fixture.h"
#include "mongo/db/repl/database_cloner.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

const std::string dbname("db");

class DatabaseClonerTest : public BaseClonerTest {
public:
    DatabaseClonerTest();
    void collectionWork(const Status& status, const NamespaceString& sourceNss);
    void clear() override;
    BaseCloner* getCloner() const override;

protected:
    void setUp() override;
    void tearDown() override;

    std::list<std::pair<Status, NamespaceString>> collectionWorkResults;
    std::unique_ptr<DatabaseCloner> databaseCloner;
};

DatabaseClonerTest::DatabaseClonerTest() : collectionWorkResults(), databaseCloner() {}

void DatabaseClonerTest::collectionWork(const Status& status, const NamespaceString& srcNss) {
    collectionWorkResults.emplace_back(status, srcNss);
}

void DatabaseClonerTest::setUp() {
    BaseClonerTest::setUp();
    collectionWorkResults.clear();
    databaseCloner.reset(new DatabaseCloner(
        &getReplExecutor(),
        target,
        dbname,
        BSONObj(),
        DatabaseCloner::ListCollectionsPredicateFn(),
        storageInterface.get(),
        stdx::bind(&DatabaseClonerTest::collectionWork,
                   this,
                   stdx::placeholders::_1,
                   stdx::placeholders::_2),
        stdx::bind(&DatabaseClonerTest::setStatus, this, stdx::placeholders::_1)));
}

void DatabaseClonerTest::tearDown() {
    BaseClonerTest::tearDown();
    databaseCloner.reset();
    collectionWorkResults.clear();
}

void DatabaseClonerTest::clear() {}

BaseCloner* DatabaseClonerTest::getCloner() const {
    return databaseCloner.get();
}

TEST_F(DatabaseClonerTest, InvalidConstruction) {
    ReplicationExecutor& executor = getReplExecutor();

    const BSONObj filter;
    DatabaseCloner::ListCollectionsPredicateFn pred;
    CollectionCloner::StorageInterface* si = storageInterface.get();
    namespace stdxph = stdx::placeholders;
    const DatabaseCloner::CollectionCallbackFn ccb =
        stdx::bind(&DatabaseClonerTest::collectionWork, this, stdxph::_1, stdxph::_2);

    const auto& cb = [](const Status&) { FAIL("should not reach here"); };

    // Null executor.
    ASSERT_THROWS(DatabaseCloner(nullptr, target, dbname, filter, pred, si, ccb, cb),
                  UserException);

    // Empty database name
    ASSERT_THROWS(DatabaseCloner(&executor, target, "", filter, pred, si, ccb, cb), UserException);

    // Callback function cannot be null.
    {
        DatabaseCloner::CallbackFn ncb;
        ASSERT_THROWS(DatabaseCloner(&executor, target, dbname, filter, pred, si, ccb, ncb),
                      UserException);
    }

    // Storage interface cannot be null.
    {
        CollectionCloner::StorageInterface* nsi = nullptr;
        ASSERT_THROWS(DatabaseCloner(&executor, target, dbname, filter, pred, nsi, ccb, cb),
                      UserException);
    }

    // CollectionCallbackFn function cannot be null.
    {
        DatabaseCloner::CollectionCallbackFn nccb;
        ASSERT_THROWS(DatabaseCloner(&executor, target, dbname, filter, pred, si, nccb, cb),
                      UserException);
    }
}

TEST_F(DatabaseClonerTest, ClonerLifeCycle) {
    testLifeCycle();
}

TEST_F(DatabaseClonerTest, FirstRemoteCommandWithoutFilter) {
    ASSERT_OK(databaseCloner->start());

    auto net = getNet();
    ASSERT_TRUE(net->hasReadyRequests());
    NetworkOperationIterator noi = net->getNextReadyRequest();
    auto&& noiRequest = noi->getRequest();
    ASSERT_EQUALS(nss.db().toString(), noiRequest.dbname);
    ASSERT_EQUALS("listCollections", std::string(noiRequest.cmdObj.firstElementFieldName()));
    ASSERT_EQUALS(1, noiRequest.cmdObj.firstElement().numberInt());
    ASSERT_FALSE(noiRequest.cmdObj.hasField("filter"));
    ASSERT_FALSE(net->hasReadyRequests());
    ASSERT_TRUE(databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, FirstRemoteCommandWithFilter) {
    const BSONObj listCollectionsFilter = BSON("name"
                                               << "coll");
    databaseCloner.reset(new DatabaseCloner(
        &getReplExecutor(),
        target,
        dbname,
        listCollectionsFilter,
        DatabaseCloner::ListCollectionsPredicateFn(),
        storageInterface.get(),
        stdx::bind(&DatabaseClonerTest::collectionWork,
                   this,
                   stdx::placeholders::_1,
                   stdx::placeholders::_2),
        stdx::bind(&DatabaseClonerTest::setStatus, this, stdx::placeholders::_1)));
    ASSERT_OK(databaseCloner->start());

    auto net = getNet();
    ASSERT_TRUE(net->hasReadyRequests());
    NetworkOperationIterator noi = net->getNextReadyRequest();
    auto&& noiRequest = noi->getRequest();
    ASSERT_EQUALS(nss.db().toString(), noiRequest.dbname);
    ASSERT_EQUALS("listCollections", std::string(noiRequest.cmdObj.firstElementFieldName()));
    ASSERT_EQUALS(1, noiRequest.cmdObj.firstElement().numberInt());
    BSONElement filterElement = noiRequest.cmdObj.getField("filter");
    ASSERT_TRUE(filterElement.isABSONObj());
    ASSERT_EQUALS(listCollectionsFilter, filterElement.Obj());
    ASSERT_FALSE(net->hasReadyRequests());
    ASSERT_TRUE(databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, InvalidListCollectionsFilter) {
    ASSERT_OK(databaseCloner->start());

    processNetworkResponse(BSON("ok" << 0 << "errmsg"
                                     << "unknown operator"
                                     << "code"
                                     << ErrorCodes::BadValue));

    ASSERT_EQUALS(ErrorCodes::BadValue, getStatus().code());
    ASSERT_FALSE(databaseCloner->isActive());
}

// A database may have no collections. Nothing to do for the database cloner.
TEST_F(DatabaseClonerTest, ListCollectionsReturnedNoCollections) {
    ASSERT_OK(databaseCloner->start());

    // Keep going even if initial batch is empty.
    processNetworkResponse(createListCollectionsResponse(1, BSONArray()));

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(databaseCloner->isActive());

    // Final batch is also empty. Database cloner should stop and return a successful status.
    processNetworkResponse(createListCollectionsResponse(0, BSONArray(), "nextBatch"));

    ASSERT_OK(getStatus());
    ASSERT_FALSE(databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, ListCollectionsPredicate) {
    DatabaseCloner::ListCollectionsPredicateFn pred = [](const BSONObj& info) {
        return info["name"].String() != "b";
    };
    databaseCloner.reset(new DatabaseCloner(
        &getReplExecutor(),
        target,
        dbname,
        BSONObj(),
        pred,
        storageInterface.get(),
        stdx::bind(&DatabaseClonerTest::collectionWork,
                   this,
                   stdx::placeholders::_1,
                   stdx::placeholders::_2),
        stdx::bind(&DatabaseClonerTest::setStatus, this, stdx::placeholders::_1)));
    ASSERT_OK(databaseCloner->start());

    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "options"
                                                   << BSONObj()),
                                              BSON("name"
                                                   << "b"
                                                   << "options"
                                                   << BSONObj()),
                                              BSON("name"
                                                   << "c"
                                                   << "options"
                                                   << BSONObj())};
    processNetworkResponse(createListCollectionsResponse(
        0, BSON_ARRAY(sourceInfos[0] << sourceInfos[1] << sourceInfos[2])));

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(databaseCloner->isActive());

    const std::vector<BSONObj>& collectionInfos = databaseCloner->getCollectionInfos();
    ASSERT_EQUALS(2U, collectionInfos.size());
    ASSERT_EQUALS(sourceInfos[0], collectionInfos[0]);
    ASSERT_EQUALS(sourceInfos[2], collectionInfos[1]);
}

TEST_F(DatabaseClonerTest, ListCollectionsMultipleBatches) {
    ASSERT_OK(databaseCloner->start());

    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "options"
                                                   << BSONObj()),
                                              BSON("name"
                                                   << "b"
                                                   << "options"
                                                   << BSONObj())};
    processNetworkResponse(createListCollectionsResponse(1, BSON_ARRAY(sourceInfos[0])));

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(databaseCloner->isActive());

    {
        const std::vector<BSONObj>& collectionInfos = databaseCloner->getCollectionInfos();
        ASSERT_EQUALS(1U, collectionInfos.size());
        ASSERT_EQUALS(sourceInfos[0], collectionInfos[0]);
    }

    processNetworkResponse(
        createListCollectionsResponse(0, BSON_ARRAY(sourceInfos[1]), "nextBatch"));

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(databaseCloner->isActive());

    {
        const std::vector<BSONObj>& collectionInfos = databaseCloner->getCollectionInfos();
        ASSERT_EQUALS(2U, collectionInfos.size());
        ASSERT_EQUALS(sourceInfos[0], collectionInfos[0]);
        ASSERT_EQUALS(sourceInfos[1], collectionInfos[1]);
    }
}

TEST_F(DatabaseClonerTest, CollectionInfoNameFieldMissing) {
    ASSERT_OK(databaseCloner->start());
    processNetworkResponse(
        createListCollectionsResponse(0, BSON_ARRAY(BSON("options" << BSONObj()))));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, getStatus().code());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "must contain 'name' field");
    ASSERT_FALSE(databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, CollectionInfoNameNotAString) {
    ASSERT_OK(databaseCloner->start());
    processNetworkResponse(createListCollectionsResponse(
        0, BSON_ARRAY(BSON("name" << 123 << "options" << BSONObj()))));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, getStatus().code());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "'name' field must be a string");
    ASSERT_FALSE(databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, CollectionInfoNameEmpty) {
    ASSERT_OK(databaseCloner->start());
    processNetworkResponse(createListCollectionsResponse(0,
                                                         BSON_ARRAY(BSON("name"
                                                                         << ""
                                                                         << "options"
                                                                         << BSONObj()))));
    ASSERT_EQUALS(ErrorCodes::BadValue, getStatus().code());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "invalid collection namespace: db.");
    ASSERT_FALSE(databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, CollectionInfoNameDuplicate) {
    ASSERT_OK(databaseCloner->start());
    processNetworkResponse(createListCollectionsResponse(0,
                                                         BSON_ARRAY(BSON("name"
                                                                         << "a"
                                                                         << "options"
                                                                         << BSONObj())
                                                                    << BSON("name"
                                                                            << "a"
                                                                            << "options"
                                                                            << BSONObj()))));
    ASSERT_EQUALS(ErrorCodes::DuplicateKey, getStatus().code());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "duplicate collection name 'a'");
    ASSERT_FALSE(databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, CollectionInfoOptionsFieldMissing) {
    ASSERT_OK(databaseCloner->start());
    processNetworkResponse(createListCollectionsResponse(0,
                                                         BSON_ARRAY(BSON("name"
                                                                         << "a"))));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, getStatus().code());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "must contain 'options' field");
    ASSERT_FALSE(databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, CollectionInfoOptionsNotAnObject) {
    ASSERT_OK(databaseCloner->start());
    processNetworkResponse(createListCollectionsResponse(0,
                                                         BSON_ARRAY(BSON("name"
                                                                         << "a"
                                                                         << "options"
                                                                         << 123))));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, getStatus().code());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "'options' field must be an object");
    ASSERT_FALSE(databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, InvalidCollectionOptions) {
    ASSERT_OK(databaseCloner->start());

    processNetworkResponse(
        createListCollectionsResponse(0,
                                      BSON_ARRAY(BSON("name"
                                                      << "a"
                                                      << "options"
                                                      << BSON("storageEngine" << 1)))));

    ASSERT_EQUALS(ErrorCodes::BadValue, getStatus().code());
    ASSERT_FALSE(databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, ListCollectionsReturnsEmptyCollectionName) {
    databaseCloner.reset(new DatabaseCloner(
        &getReplExecutor(),
        target,
        dbname,
        BSONObj(),
        DatabaseCloner::ListCollectionsPredicateFn(),
        storageInterface.get(),
        stdx::bind(&DatabaseClonerTest::collectionWork,
                   this,
                   stdx::placeholders::_1,
                   stdx::placeholders::_2),
        stdx::bind(&DatabaseClonerTest::setStatus, this, stdx::placeholders::_1)));
    ASSERT_OK(databaseCloner->start());

    processNetworkResponse(createListCollectionsResponse(0,
                                                         BSON_ARRAY(BSON("name"
                                                                         << ""
                                                                         << "options"
                                                                         << BSONObj()))));

    ASSERT_EQUALS(ErrorCodes::BadValue, getStatus().code());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "invalid collection namespace: db.");
    ASSERT_FALSE(databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, StartFirstCollectionClonerFailed) {
    ASSERT_OK(databaseCloner->start());

    databaseCloner->setStartCollectionClonerFn(
        [](CollectionCloner& cloner) { return Status(ErrorCodes::OperationFailed, ""); });

    processNetworkResponse(createListCollectionsResponse(0,
                                                         BSON_ARRAY(BSON("name"
                                                                         << "a"
                                                                         << "options"
                                                                         << BSONObj()))));

    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus().code());
    ASSERT_FALSE(databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, StartSecondCollectionClonerFailed) {
    ASSERT_OK(databaseCloner->start());

    // Replace scheduleDbWork function so that all callbacks (including exclusive tasks)
    // will run through network interface.
    auto&& executor = getReplExecutor();
    databaseCloner->setScheduleDbWorkFn([&](const ReplicationExecutor::CallbackFn& workFn) {
        return executor.scheduleWork(workFn);
    });

    databaseCloner->setStartCollectionClonerFn([](CollectionCloner& cloner) {
        if (cloner.getSourceNamespace().coll() == "b") {
            return Status(ErrorCodes::OperationFailed, "");
        }
        return cloner.start();
    });

    processNetworkResponse(createListCollectionsResponse(0,
                                                         BSON_ARRAY(BSON("name"
                                                                         << "a"
                                                                         << "options"
                                                                         << BSONObj())
                                                                    << BSON("name"
                                                                            << "b"
                                                                            << "options"
                                                                            << BSONObj()))));

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    processNetworkResponse(createCursorResponse(0, BSONArray()));

    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus().code());
    ASSERT_FALSE(databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, FirstCollectionListIndexesFailed) {
    ASSERT_OK(databaseCloner->start());

    // Replace scheduleDbWork function so that all callbacks (including exclusive tasks)
    // will run through network interface.
    auto&& executor = getReplExecutor();
    databaseCloner->setScheduleDbWorkFn([&](const ReplicationExecutor::CallbackFn& workFn) {
        return executor.scheduleWork(workFn);
    });

    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "options"
                                                   << BSONObj()),
                                              BSON("name"
                                                   << "b"
                                                   << "options"
                                                   << BSONObj())};
    processNetworkResponse(
        createListCollectionsResponse(0, BSON_ARRAY(sourceInfos[0] << sourceInfos[1])));

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(databaseCloner->isActive());

    // Collection cloners are run serially for now.
    // This affects the order of the network responses.
    processNetworkResponse(BSON("ok" << 0 << "errmsg"
                                     << ""
                                     << "code"
                                     << ErrorCodes::NamespaceNotFound));

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    processNetworkResponse(createCursorResponse(0, BSONArray()));

    ASSERT_OK(getStatus());
    ASSERT_FALSE(databaseCloner->isActive());

    ASSERT_EQUALS(2U, collectionWorkResults.size());
    {
        auto i = collectionWorkResults.cbegin();
        ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, i->first.code());
        ASSERT_EQUALS(i->second.ns(), NamespaceString(dbname, "a").ns());
        i++;
        ASSERT_OK(i->first);
        ASSERT_EQUALS(i->second.ns(), NamespaceString(dbname, "b").ns());
    }
}

TEST_F(DatabaseClonerTest, CreateCollections) {
    ASSERT_OK(databaseCloner->start());

    // Replace scheduleDbWork function so that all callbacks (including exclusive tasks)
    // will run through network interface.
    auto&& executor = getReplExecutor();
    databaseCloner->setScheduleDbWorkFn([&](const ReplicationExecutor::CallbackFn& workFn) {
        return executor.scheduleWork(workFn);
    });

    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "options"
                                                   << BSONObj()),
                                              BSON("name"
                                                   << "b"
                                                   << "options"
                                                   << BSONObj())};
    processNetworkResponse(
        createListCollectionsResponse(0, BSON_ARRAY(sourceInfos[0] << sourceInfos[1])));

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(databaseCloner->isActive());

    // Collection cloners are run serially for now.
    // This affects the order of the network responses.
    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    processNetworkResponse(createCursorResponse(0, BSONArray()));

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    processNetworkResponse(createCursorResponse(0, BSONArray()));

    ASSERT_OK(getStatus());
    ASSERT_FALSE(databaseCloner->isActive());

    ASSERT_EQUALS(2U, collectionWorkResults.size());
    {
        auto i = collectionWorkResults.cbegin();
        ASSERT_OK(i->first);
        ASSERT_EQUALS(i->second.ns(), NamespaceString(dbname, "a").ns());
        i++;
        ASSERT_OK(i->first);
        ASSERT_EQUALS(i->second.ns(), NamespaceString(dbname, "b").ns());
    }
}

}  // namespace
