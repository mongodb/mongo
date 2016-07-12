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
#include "mongo/db/repl/storage_interface.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace {

using namespace mongo;
using namespace mongo::repl;
using namespace unittest;

const std::string dbname("db");

struct CollectionCloneInfo {
    CollectionMockStats stats;
    CollectionBulkLoaderMock* loader = nullptr;
    Status status{ErrorCodes::NotYetInitialized, ""};
};

class DatabaseClonerTest : public BaseClonerTest {
public:
    void collectionWork(const Status& status, const NamespaceString& sourceNss);
    void clear() override;
    BaseCloner* getCloner() const override;

protected:
    void setUp() override;
    void tearDown() override;

    std::map<NamespaceString, CollectionCloneInfo> _collections;
    std::unique_ptr<DatabaseCloner> _databaseCloner;
};
void DatabaseClonerTest::collectionWork(const Status& status, const NamespaceString& srcNss) {
    _collections[srcNss].status = status;
}

void DatabaseClonerTest::setUp() {
    BaseClonerTest::setUp();
    _databaseCloner.reset(new DatabaseCloner(
        &getExecutor(),
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

    storageInterface->createCollectionForBulkFn =
        [this](const NamespaceString& nss,
               const CollectionOptions& options,
               const BSONObj& idIndexSpec,
               const std::vector<BSONObj>& secondaryIndexSpecs) {
            const auto collInfo = &_collections[nss];
            (collInfo->loader = new CollectionBulkLoaderMock(&collInfo->stats))
                ->init(nullptr, nullptr, secondaryIndexSpecs);

            return StatusWith<std::unique_ptr<CollectionBulkLoader>>(
                std::unique_ptr<CollectionBulkLoader>(collInfo->loader));
        };
}

void DatabaseClonerTest::tearDown() {
    BaseClonerTest::tearDown();
    _databaseCloner.reset();
    _collections.clear();
}

void DatabaseClonerTest::clear() {}

BaseCloner* DatabaseClonerTest::getCloner() const {
    return _databaseCloner.get();
}

TEST_F(DatabaseClonerTest, InvalidConstruction) {
    executor::TaskExecutor& executor = getExecutor();

    const BSONObj filter;
    DatabaseCloner::ListCollectionsPredicateFn pred;
    StorageInterface* si = storageInterface.get();
    namespace stdxph = stdx::placeholders;
    const DatabaseCloner::CollectionCallbackFn ccb =
        stdx::bind(&DatabaseClonerTest::collectionWork, this, stdxph::_1, stdxph::_2);

    const auto& cb = [](const Status&) { FAIL("should not reach here"); };

    // Null executor -- error from Fetcher, not _databaseCloner.
    ASSERT_THROWS_CODE_AND_WHAT(DatabaseCloner(nullptr, target, dbname, filter, pred, si, ccb, cb),
                                UserException,
                                ErrorCodes::BadValue,
                                "task executor cannot be null");

    // Empty database name -- error from Fetcher, not _databaseCloner.
    ASSERT_THROWS_CODE_AND_WHAT(DatabaseCloner(&executor, target, "", filter, pred, si, ccb, cb),
                                UserException,
                                ErrorCodes::BadValue,
                                "database name in remote command request cannot be empty");

    // Callback function cannot be null.
    ASSERT_THROWS_CODE_AND_WHAT(
        DatabaseCloner(&executor, target, dbname, filter, pred, si, ccb, nullptr),
        UserException,
        ErrorCodes::BadValue,
        "callback function cannot be null");

    // Storage interface cannot be null.
    ASSERT_THROWS_CODE_AND_WHAT(
        DatabaseCloner(&executor, target, dbname, filter, pred, nullptr, ccb, cb),
        UserException,
        ErrorCodes::BadValue,
        "storage interface cannot be null");

    // CollectionCallbackFn function cannot be null.
    ASSERT_THROWS_CODE_AND_WHAT(
        DatabaseCloner(&executor, target, dbname, filter, pred, si, nullptr, cb),
        UserException,
        ErrorCodes::BadValue,
        "collection callback function cannot be null");

    // Completion callback cannot be null.
    ASSERT_THROWS_CODE_AND_WHAT(
        DatabaseCloner(&executor, target, dbname, filter, pred, si, ccb, nullptr),
        UserException,
        ErrorCodes::BadValue,
        "callback function cannot be null");
}

TEST_F(DatabaseClonerTest, ClonerLifeCycle) {
    testLifeCycle();
}

TEST_F(DatabaseClonerTest, FirstRemoteCommandWithoutFilter) {
    ASSERT_OK(_databaseCloner->start());

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    ASSERT_TRUE(net->hasReadyRequests());
    NetworkOperationIterator noi = net->getNextReadyRequest();
    auto&& noiRequest = noi->getRequest();
    ASSERT_EQUALS(nss.db().toString(), noiRequest.dbname);
    ASSERT_EQUALS("listCollections", std::string(noiRequest.cmdObj.firstElementFieldName()));
    ASSERT_EQUALS(1, noiRequest.cmdObj.firstElement().numberInt());
    ASSERT_FALSE(noiRequest.cmdObj.hasField("filter"));
    ASSERT_FALSE(net->hasReadyRequests());
    ASSERT_TRUE(_databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, FirstRemoteCommandWithFilter) {
    const BSONObj listCollectionsFilter = BSON("name"
                                               << "coll");
    _databaseCloner.reset(new DatabaseCloner(
        &getExecutor(),
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
    ASSERT_OK(_databaseCloner->start());

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
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
    ASSERT_TRUE(_databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, InvalidListCollectionsFilter) {
    ASSERT_OK(_databaseCloner->start());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(BSON("ok" << 0 << "errmsg"
                                         << "unknown operator"
                                         << "code"
                                         << ErrorCodes::BadValue));
    }

    ASSERT_EQUALS(ErrorCodes::BadValue, getStatus().code());
    ASSERT_FALSE(_databaseCloner->isActive());
}

// A database may have no collections. Nothing to do for the database cloner.
TEST_F(DatabaseClonerTest, ListCollectionsReturnedNoCollections) {
    ASSERT_OK(_databaseCloner->start());

    // Keep going even if initial batch is empty.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListCollectionsResponse(1, BSONArray()));
    }

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(_databaseCloner->isActive());

    // Final batch is also empty. Database cloner should stop and return a successful status.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListCollectionsResponse(0, BSONArray(), "nextBatch"));
    }

    ASSERT_OK(getStatus());
    ASSERT_FALSE(_databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, ListCollectionsPredicate) {
    DatabaseCloner::ListCollectionsPredicateFn pred = [](const BSONObj& info) {
        return info["name"].String() != "b";
    };
    _databaseCloner.reset(new DatabaseCloner(
        &getExecutor(),
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
    ASSERT_OK(_databaseCloner->start());

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
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListCollectionsResponse(
            0, BSON_ARRAY(sourceInfos[0] << sourceInfos[1] << sourceInfos[2])));
    }

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(_databaseCloner->isActive());

    const std::vector<BSONObj>& collectionInfos = _databaseCloner->getCollectionInfos();
    ASSERT_EQUALS(2U, collectionInfos.size());
    ASSERT_EQUALS(sourceInfos[0], collectionInfos[0]);
    ASSERT_EQUALS(sourceInfos[2], collectionInfos[1]);
}

TEST_F(DatabaseClonerTest, ListCollectionsMultipleBatches) {
    ASSERT_OK(_databaseCloner->start());

    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "options"
                                                   << BSONObj()),
                                              BSON("name"
                                                   << "b"
                                                   << "options"
                                                   << BSONObj())};
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListCollectionsResponse(1, BSON_ARRAY(sourceInfos[0])));
    }

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(_databaseCloner->isActive());

    {
        const std::vector<BSONObj>& collectionInfos = _databaseCloner->getCollectionInfos();
        ASSERT_EQUALS(1U, collectionInfos.size());
        ASSERT_EQUALS(sourceInfos[0], collectionInfos[0]);
    }

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(
            createListCollectionsResponse(0, BSON_ARRAY(sourceInfos[1]), "nextBatch"));
    }

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(_databaseCloner->isActive());

    {
        const std::vector<BSONObj>& collectionInfos = _databaseCloner->getCollectionInfos();
        ASSERT_EQUALS(2U, collectionInfos.size());
        ASSERT_EQUALS(sourceInfos[0], collectionInfos[0]);
        ASSERT_EQUALS(sourceInfos[1], collectionInfos[1]);
    }
}

TEST_F(DatabaseClonerTest, CollectionInfoNameFieldMissing) {
    ASSERT_OK(_databaseCloner->start());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(
            createListCollectionsResponse(0, BSON_ARRAY(BSON("options" << BSONObj()))));
    }
    ASSERT_EQUALS(ErrorCodes::FailedToParse, getStatus().code());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "must contain 'name' field");
    ASSERT_FALSE(_databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, CollectionInfoNameNotAString) {
    ASSERT_OK(_databaseCloner->start());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListCollectionsResponse(
            0, BSON_ARRAY(BSON("name" << 123 << "options" << BSONObj()))));
    }
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, getStatus().code());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "'name' field must be a string");
    ASSERT_FALSE(_databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, CollectionInfoNameEmpty) {
    ASSERT_OK(_databaseCloner->start());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListCollectionsResponse(0,
                                                             BSON_ARRAY(BSON("name"
                                                                             << ""
                                                                             << "options"
                                                                             << BSONObj()))));
    }
    ASSERT_EQUALS(ErrorCodes::BadValue, getStatus().code());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "invalid collection namespace: db.");
    ASSERT_FALSE(_databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, CollectionInfoNameDuplicate) {
    ASSERT_OK(_databaseCloner->start());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListCollectionsResponse(0,
                                                             BSON_ARRAY(BSON("name"
                                                                             << "a"
                                                                             << "options"
                                                                             << BSONObj())
                                                                        << BSON("name"
                                                                                << "a"
                                                                                << "options"
                                                                                << BSONObj()))));
    }
    ASSERT_EQUALS(ErrorCodes::DuplicateKey, getStatus().code());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "duplicate collection name 'a'");
    ASSERT_FALSE(_databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, CollectionInfoOptionsFieldMissing) {
    ASSERT_OK(_databaseCloner->start());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListCollectionsResponse(0,
                                                             BSON_ARRAY(BSON("name"
                                                                             << "a"))));
    }
    ASSERT_EQUALS(ErrorCodes::FailedToParse, getStatus().code());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "must contain 'options' field");
    ASSERT_FALSE(_databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, CollectionInfoOptionsNotAnObject) {
    ASSERT_OK(_databaseCloner->start());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListCollectionsResponse(0,
                                                             BSON_ARRAY(BSON("name"
                                                                             << "a"
                                                                             << "options"
                                                                             << 123))));
    }
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, getStatus().code());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "'options' field must be an object");
    ASSERT_FALSE(_databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, InvalidCollectionOptions) {
    ASSERT_OK(_databaseCloner->start());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(
            createListCollectionsResponse(0,
                                          BSON_ARRAY(BSON("name"
                                                          << "a"
                                                          << "options"
                                                          << BSON("storageEngine" << 1)))));
    }
    ASSERT_EQUALS(ErrorCodes::BadValue, getStatus().code());
    ASSERT_FALSE(_databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, ListCollectionsReturnsEmptyCollectionName) {
    _databaseCloner.reset(new DatabaseCloner(
        &getExecutor(),
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
    ASSERT_OK(_databaseCloner->start());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListCollectionsResponse(0,
                                                             BSON_ARRAY(BSON("name"
                                                                             << ""
                                                                             << "options"
                                                                             << BSONObj()))));
    }
    ASSERT_EQUALS(ErrorCodes::BadValue, getStatus().code());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "invalid collection namespace: db.");
    ASSERT_FALSE(_databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, StartFirstCollectionClonerFailed) {
    ASSERT_OK(_databaseCloner->start());

    _databaseCloner->setStartCollectionClonerFn([](CollectionCloner& cloner) {
        return Status(ErrorCodes::OperationFailed,
                      "StartFirstCollectionClonerFailed injected failure.");
    });

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListCollectionsResponse(0,
                                                             BSON_ARRAY(BSON("name"
                                                                             << "a"
                                                                             << "options"
                                                                             << BSONObj()))));
    }
    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus().code());
    ASSERT_FALSE(_databaseCloner->isActive());
}

TEST_F(DatabaseClonerTest, StartSecondCollectionClonerFailed) {
    ASSERT_OK(_databaseCloner->start());
    const Status errStatus{ErrorCodes::OperationFailed,
                           "StartSecondCollectionClonerFailed injected failure."};

    _databaseCloner->setStartCollectionClonerFn([errStatus](CollectionCloner& cloner) -> Status {
        if (cloner.getSourceNamespace().coll() == "b") {
            return errStatus;
        }
        return cloner.start();
    });

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
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
    }
    _databaseCloner->wait();
    ASSERT_FALSE(_databaseCloner->isActive());
    ASSERT_EQUALS(errStatus, getStatus());
}

TEST_F(DatabaseClonerTest, FirstCollectionListIndexesFailed) {
    ASSERT_OK(_databaseCloner->start());

    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "options"
                                                   << BSONObj()),
                                              BSON("name"
                                                   << "b"
                                                   << "options"
                                                   << BSONObj())};
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(
            createListCollectionsResponse(0, BSON_ARRAY(sourceInfos[0] << sourceInfos[1])));
    }
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(_databaseCloner->isActive());

    // Collection cloners are run serially for now.
    // This affects the order of the network responses.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(BSON("ok" << 0 << "errmsg"
                                         << "fake message"
                                         << "code"
                                         << ErrorCodes::CursorNotFound));

        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
        processNetworkResponse(createCursorResponse(0, BSONArray()));
    }
    _databaseCloner->wait();
    ASSERT_EQ(getStatus().code(), ErrorCodes::InitialSyncFailure);
    ASSERT_FALSE(_databaseCloner->isActive());

    ASSERT_EQUALS(2U, _collections.size());

    auto collInfo = _collections[NamespaceString{"db.a"}];
    ASSERT_EQUALS(ErrorCodes::CursorNotFound, collInfo.status.code());
    auto stats = collInfo.stats;
    stats.insertCount = 0;
    stats.commitCalled = false;

    collInfo = _collections[NamespaceString{"db.b"}];
    ASSERT_OK(collInfo.status);
    stats = collInfo.stats;
    stats.insertCount = 0;
    stats.commitCalled = true;
}

TEST_F(DatabaseClonerTest, CreateCollections) {
    ASSERT_OK(_databaseCloner->start());

    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "options"
                                                   << BSONObj()),
                                              BSON("name"
                                                   << "b"
                                                   << "options"
                                                   << BSONObj())};
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(
            createListCollectionsResponse(0, BSON_ARRAY(sourceInfos[0] << sourceInfos[1])));
    }
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(_databaseCloner->isActive());

    // Collection cloners are run serially for now.
    // This affects the order of the network responses.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(_databaseCloner->isActive());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(0, BSONArray()));
    }
    ASSERT_TRUE(_databaseCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(_databaseCloner->isActive());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(0, BSONArray()));
    }

    _databaseCloner->wait();
    ASSERT_OK(getStatus());
    ASSERT_FALSE(_databaseCloner->isActive());

    ASSERT_EQUALS(2U, _collections.size());

    auto collInfo = _collections[NamespaceString{"db.a"}];
    ASSERT_OK(collInfo.status);
    auto stats = collInfo.stats;
    stats.insertCount = 0;
    stats.commitCalled = true;

    collInfo = _collections[NamespaceString{"db.b"}];
    ASSERT_OK(collInfo.status);
    stats = collInfo.stats;
    stats.insertCount = 0;
    stats.commitCalled = true;
}

}  // namespace
