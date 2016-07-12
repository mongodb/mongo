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

#include <memory>
#include <vector>

#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/base_cloner_test_fixture.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace {

using namespace mongo;
using namespace mongo::repl;
using namespace unittest;

class MockCallbackState final : public mongo::executor::TaskExecutor::CallbackState {
public:
    MockCallbackState() = default;
    void cancel() override {}
    void waitForCompletion() override {}
    bool isCanceled() const override {
        return false;
    }
};

class CollectionClonerTest : public BaseClonerTest {
public:
    BaseCloner* getCloner() const override;

protected:
    void setUp() override;
    void tearDown() override;

    CollectionOptions options;
    std::unique_ptr<CollectionCloner> collectionCloner;
    CollectionMockStats collectionStats;  // Used by the _loader.
    CollectionBulkLoaderMock* _loader;    // Owned by CollectionCloner.
};

void CollectionClonerTest::setUp() {
    BaseClonerTest::setUp();
    options.reset();
    collectionCloner.reset(nullptr);
    collectionCloner.reset(new CollectionCloner(
        &getExecutor(),
        target,
        nss,
        options,
        stdx::bind(&CollectionClonerTest::setStatus, this, stdx::placeholders::_1),
        storageInterface.get()));
    collectionStats = CollectionMockStats();
    storageInterface->createCollectionForBulkFn =
        [this](const NamespaceString& nss,
               const CollectionOptions& options,
               const BSONObj idIndexSpec,
               const std::vector<BSONObj>& secondaryIndexSpecs) {
            (_loader = new CollectionBulkLoaderMock(&collectionStats))
                ->init(nullptr, nullptr, secondaryIndexSpecs);

            return StatusWith<std::unique_ptr<CollectionBulkLoader>>(
                std::unique_ptr<CollectionBulkLoader>(_loader));
        };
}

void CollectionClonerTest::tearDown() {
    BaseClonerTest::tearDown();
    // Executor may still invoke collection cloner's callback before shutting down.
    collectionCloner.reset(nullptr);
    options.reset();
}

BaseCloner* CollectionClonerTest::getCloner() const {
    return collectionCloner.get();
}

TEST_F(CollectionClonerTest, InvalidConstruction) {
    executor::TaskExecutor& executor = getExecutor();

    const auto& cb = [](const Status&) { FAIL("should not reach here"); };

    // Null executor -- error from Fetcher, not CollectionCloner.
    {
        StorageInterface* si = storageInterface.get();
        ASSERT_THROWS_CODE_AND_WHAT(CollectionCloner(nullptr, target, nss, options, cb, si),
                                    UserException,
                                    ErrorCodes::BadValue,
                                    "task executor cannot be null");
    }

    // Null storage interface
    ASSERT_THROWS_CODE_AND_WHAT(CollectionCloner(&executor, target, nss, options, cb, nullptr),
                                UserException,
                                ErrorCodes::BadValue,
                                "storage interface cannot be null");

    // Invalid namespace.
    {
        NamespaceString badNss("db.");
        StorageInterface* si = storageInterface.get();
        ASSERT_THROWS_CODE_AND_WHAT(CollectionCloner(&executor, target, badNss, options, cb, si),
                                    UserException,
                                    ErrorCodes::BadValue,
                                    "invalid collection namespace: db.");
    }

    // Invalid collection options - error from CollectionOptions::validate(), not CollectionCloner.
    {
        CollectionOptions invalidOptions;
        invalidOptions.storageEngine = BSON("storageEngine1"
                                            << "not a document");
        StorageInterface* si = storageInterface.get();
        ASSERT_THROWS_CODE_AND_WHAT(
            CollectionCloner(&executor, target, nss, invalidOptions, cb, si),
            UserException,
            ErrorCodes::BadValue,
            "'storageEngine.storageEngine1' has to be an embedded document.");
    }

    // Callback function cannot be null.
    {
        CollectionCloner::CallbackFn nullCb;
        StorageInterface* si = storageInterface.get();
        ASSERT_THROWS_CODE_AND_WHAT(CollectionCloner(&executor, target, nss, options, nullCb, si),
                                    UserException,
                                    ErrorCodes::BadValue,
                                    "callback function cannot be null");
    }
}

TEST_F(CollectionClonerTest, ClonerLifeCycle) {
    testLifeCycle();
}

TEST_F(CollectionClonerTest, FirstRemoteCommand) {
    ASSERT_OK(collectionCloner->start());

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
    ASSERT_TRUE(net->hasReadyRequests());
    NetworkOperationIterator noi = net->getNextReadyRequest();
    auto&& noiRequest = noi->getRequest();
    ASSERT_EQUALS(nss.db().toString(), noiRequest.dbname);
    ASSERT_EQUALS("listIndexes", std::string(noiRequest.cmdObj.firstElementFieldName()));
    ASSERT_EQUALS(nss.coll().toString(), noiRequest.cmdObj.firstElement().valuestrsafe());
    ASSERT_FALSE(net->hasReadyRequests());
    ASSERT_TRUE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, DoNotCreateIDIndexIfAutoIndexIdUsed) {
    options.reset();
    options.autoIndexId = CollectionOptions::NO;
    collectionCloner.reset(new CollectionCloner(
        &getExecutor(),
        target,
        nss,
        options,
        stdx::bind(&CollectionClonerTest::setStatus, this, stdx::placeholders::_1),
        storageInterface.get()));

    NamespaceString collNss;
    CollectionOptions collOptions;
    std::vector<BSONObj> collIndexSpecs{BSON("fakeindexkeys" << 1)};  // init with one doc.
    storageInterface->createCollectionForBulkFn = [&,
                                                   this](const NamespaceString& theNss,
                                                         const CollectionOptions& theOptions,
                                                         const BSONObj idIndexSpec,
                                                         const std::vector<BSONObj>& theIndexSpecs)
        -> StatusWith<std::unique_ptr<CollectionBulkLoader>> {
            CollectionBulkLoaderMock* loader = new CollectionBulkLoaderMock(&collectionStats);
            collNss = theNss;
            collOptions = theOptions;
            collIndexSpecs = theIndexSpecs;
            loader->init(nullptr, nullptr, theIndexSpecs);
            return std::unique_ptr<CollectionBulkLoader>(loader);
        };

    ASSERT_OK(collectionCloner->start());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSONArray()));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(0, BSON_ARRAY(doc)));
    }
    collectionCloner->wait();
    ASSERT_EQUALS(1, collectionStats.insertCount);
    ASSERT_TRUE(collectionStats.commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
    ASSERT_EQ(collOptions.autoIndexId, CollectionOptions::NO);
    ASSERT_EQ(0UL, collIndexSpecs.size());
    ASSERT_EQ(collNss, nss);
}

// A collection may have no indexes. The cloner will produce a warning but
// will still proceed with cloning.
TEST_F(CollectionClonerTest, ListIndexesReturnedNoIndexes) {
    ASSERT_OK(collectionCloner->start());

    // Using a non-zero cursor to ensure that
    // the cloner stops the fetcher from retrieving more results.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(1, BSONArray()));
    }

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        ASSERT_TRUE(getNet()->hasReadyRequests());
    }
}

TEST_F(CollectionClonerTest, ListIndexesReturnedNamespaceNotFound) {
    ASSERT_OK(collectionCloner->start());

    bool collectionCreated = false;
    NamespaceString collNss;
    storageInterface->createCollFn = [&collNss, &collectionCreated](
        OperationContext*, const NamespaceString& nss, const CollectionOptions& options) {
        collectionCreated = true;
        collNss = nss;
        return Status::OK();
    };
    // Using a non-zero cursor to ensure that
    // the cloner stops the fetcher from retrieving more results.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(ErrorCodes::NamespaceNotFound, "The collection doesn't exist.");
    }

    collectionCloner->wait();
    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
    ASSERT_TRUE(collectionCreated);
    ASSERT_EQ(collNss, nss);
}

TEST_F(CollectionClonerTest, BeginCollectionScheduleDbWorkFailed) {
    ASSERT_OK(collectionCloner->start());

    // Replace scheduleDbWork function so that cloner will fail to schedule DB work after
    // getting index specs.
    collectionCloner->setScheduleDbWorkFn([](const executor::TaskExecutor::CallbackFn& workFn) {
        return StatusWith<executor::TaskExecutor::CallbackHandle>(ErrorCodes::UnknownError, "");
    });

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    ASSERT_EQUALS(ErrorCodes::UnknownError, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, BeginCollectionCallbackCanceled) {
    ASSERT_OK(collectionCloner->start());

    // Replace scheduleDbWork function so that the callback runs with a cancelled status.
    auto&& executor = getExecutor();
    collectionCloner->setScheduleDbWorkFn([&](const executor::TaskExecutor::CallbackFn& workFn) {
        executor::TaskExecutor::CallbackHandle handle(std::make_shared<MockCallbackState>());
        mongo::executor::TaskExecutor::CallbackArgs args{
            &executor,
            handle,
            {ErrorCodes::CallbackCanceled, "Never run, but treat like cancelled."}};
        workFn(args);
        return StatusWith<executor::TaskExecutor::CallbackHandle>(handle);
    });

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, BeginCollectionFailed) {
    ASSERT_OK(collectionCloner->start());

    storageInterface->createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                      const CollectionOptions& theOptions,
                                                      const BSONObj idIndexSpec,
                                                      const std::vector<BSONObj>& theIndexSpecs) {
        return Status(ErrorCodes::OperationFailed, "");
    };

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();

    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, BeginCollection) {
    ASSERT_OK(collectionCloner->start());

    CollectionMockStats stats;
    CollectionBulkLoaderMock* loader = new CollectionBulkLoaderMock(&stats);
    NamespaceString collNss;
    CollectionOptions collOptions;
    std::vector<BSONObj> collIndexSpecs;
    storageInterface->createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                      const CollectionOptions& theOptions,
                                                      const BSONObj idIndexSpec,
                                                      const std::vector<BSONObj>& theIndexSpecs)
        -> StatusWith<std::unique_ptr<CollectionBulkLoader>> {
            collNss = theNss;
            collOptions = theOptions;
            collIndexSpecs = theIndexSpecs;
            return std::unique_ptr<CollectionBulkLoader>(loader);
        };

    // Split listIndexes response into 2 batches: first batch contains idIndexSpec and
    // second batch contains specs
    std::vector<BSONObj> specs{BSON("v" << 1 << "key" << BSON("a" << 1) << "name"
                                        << "a_1"
                                        << "ns"
                                        << nss.ns()),
                               BSON("v" << 1 << "key" << BSON("b" << 1) << "name"
                                        << "b_1"
                                        << "ns"
                                        << nss.ns())};

    // First batch contains the _id_ index spec.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(1, BSON_ARRAY(idIndexSpec)));
    }

    // 'status' should not be modified because cloning is not finished.
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    // Second batch contains the other index specs.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(
            createListIndexesResponse(0, BSON_ARRAY(specs[0] << specs[1]), "nextBatch"));
    }

    collectionCloner->waitForDbWorker();

    // 'status' will be set if listIndexes fails.
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());

    ASSERT_EQUALS(nss.ns(), collNss.ns());
    ASSERT_EQUALS(options.toBSON(), collOptions.toBSON());
    ASSERT_EQUALS(specs.size(), collIndexSpecs.size());
    for (std::vector<BSONObj>::size_type i = 0; i < specs.size(); ++i) {
        ASSERT_EQUALS(specs[i], collIndexSpecs[i]);
    }

    // Cloner is still active because it has to read the documents from the source collection.
    ASSERT_TRUE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, FindFetcherScheduleFailed) {
    ASSERT_OK(collectionCloner->start());

    // Shut down executor while in beginCollection callback.
    // This will cause the fetcher to fail to schedule the find command.
    CollectionMockStats stats;
    CollectionBulkLoaderMock* loader = new CollectionBulkLoaderMock(&stats);
    bool collectionCreated = false;
    storageInterface->createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                      const CollectionOptions& theOptions,
                                                      const BSONObj idIndexSpec,
                                                      const std::vector<BSONObj>& theIndexSpecs)
        -> StatusWith<std::unique_ptr<CollectionBulkLoader>> {
            collectionCreated = true;
            getExecutor().shutdown();
            return std::unique_ptr<CollectionBulkLoader>(loader);
        };

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCreated);

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, FindCommandAfterBeginCollection) {
    ASSERT_OK(collectionCloner->start());

    CollectionMockStats stats;
    CollectionBulkLoaderMock* loader = new CollectionBulkLoaderMock(&stats);
    bool collectionCreated = false;
    storageInterface->createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                      const CollectionOptions& theOptions,
                                                      const BSONObj idIndexSpec,
                                                      const std::vector<BSONObj>& theIndexSpecs)
        -> StatusWith<std::unique_ptr<CollectionBulkLoader>> {
            collectionCreated = true;
            return std::unique_ptr<CollectionBulkLoader>(loader);
        };

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCreated);

    // Fetcher should be scheduled after cloner creates collection.
    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    ASSERT_TRUE(net->hasReadyRequests());
    NetworkOperationIterator noi = net->getNextReadyRequest();
    auto&& noiRequest = noi->getRequest();
    ASSERT_EQUALS(nss.db().toString(), noiRequest.dbname);
    ASSERT_EQUALS("find", std::string(noiRequest.cmdObj.firstElementFieldName()));
    ASSERT_EQUALS(nss.coll().toString(), noiRequest.cmdObj.firstElement().valuestrsafe());
    ASSERT_TRUE(noiRequest.cmdObj.getField("noCursorTimeout").trueValue());
    ASSERT_FALSE(net->hasReadyRequests());
}

TEST_F(CollectionClonerTest, FindCommandFailed) {
    ASSERT_OK(collectionCloner->start());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());
    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(BSON("ok" << 0 << "errmsg"
                                         << ""
                                         << "code"
                                         << ErrorCodes::CursorNotFound));
    }

    ASSERT_EQUALS(ErrorCodes::CursorNotFound, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, FindCommandCanceled) {
    ASSERT_OK(collectionCloner->start());

    ASSERT_TRUE(collectionCloner->isActive());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        scheduleNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());

        net->runReadyNetworkOperations();
    }

    collectionCloner->waitForDbWorker();

    ASSERT_TRUE(collectionCloner->isActive());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        scheduleNetworkResponse(BSON("ok" << 1));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->cancel();

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        getNet()->logQueues();
        net->runReadyNetworkOperations();
    }

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsScheduleDbWorkFailed) {
    ASSERT_OK(collectionCloner->start());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();

    // Replace scheduleDbWork function so that cloner will fail to schedule DB work after
    // getting documents.
    collectionCloner->setScheduleDbWorkFn([](const executor::TaskExecutor::CallbackFn& workFn) {
        return StatusWith<executor::TaskExecutor::CallbackHandle>(ErrorCodes::UnknownError, "");
    });

    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(0, BSON_ARRAY(doc)));
    }

    ASSERT_EQUALS(ErrorCodes::UnknownError, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsCallbackCanceled) {
    ASSERT_OK(collectionCloner->start());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();

    // Replace scheduleDbWork function so that the callback runs with a cancelled status.
    auto&& executor = getExecutor();
    collectionCloner->setScheduleDbWorkFn([&](const executor::TaskExecutor::CallbackFn& workFn) {
        executor::TaskExecutor::CallbackHandle handle(std::make_shared<MockCallbackState>());
        mongo::executor::TaskExecutor::CallbackArgs args{
            &executor,
            handle,
            {ErrorCodes::CallbackCanceled, "Never run, but treat like cancelled."}};
        workFn(args);
        return StatusWith<executor::TaskExecutor::CallbackHandle>(handle);
    });

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(0, BSON_ARRAY(BSON("_id" << 1))));
    }
    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsFailed) {
    ASSERT_OK(collectionCloner->start());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());
    getNet()->logQueues();

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    ASSERT(_loader != nullptr);
    _loader->insertDocsFn = [](const std::vector<BSONObj>::const_iterator begin,
                               const std::vector<BSONObj>::const_iterator end) {
        return Status(ErrorCodes::OperationFailed, "");
    };

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(0, BSON_ARRAY(BSON("_id" << 1))));
    }

    collectionCloner->wait();
    ASSERT_FALSE(collectionCloner->isActive());
    ASSERT_EQUALS(0, collectionStats.insertCount);

    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus().code());
}

TEST_F(CollectionClonerTest, InsertDocumentsSingleBatch) {
    ASSERT_OK(collectionCloner->start());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(0, BSON_ARRAY(doc)));
    }

    collectionCloner->waitForDbWorker();
    // TODO: record the documents during insert and compare them
    //       -- maybe better done using a real storage engine, like ephemeral for test.
    ASSERT_EQUALS(1, collectionStats.insertCount);
    ASSERT_TRUE(collectionStats.commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsMultipleBatches) {
    ASSERT_OK(collectionCloner->start());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, BSON_ARRAY(doc)));
    }

    collectionCloner->waitForDbWorker();
    // TODO: record the documents during insert and compare them
    //       -- maybe better done using a real storage engine, like ephemeral for test.
    ASSERT_EQUALS(1, collectionStats.insertCount);

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    const BSONObj doc2 = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(0, BSON_ARRAY(doc2), "nextBatch"));
    }

    collectionCloner->waitForDbWorker();
    // TODO: record the documents during insert and compare them
    //       -- maybe better done using a real storage engine, like ephemeral for test.
    ASSERT_EQUALS(2, collectionStats.insertCount);
    ASSERT_TRUE(collectionStats.commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, LastBatchContainsNoDocuments) {
    ASSERT_OK(collectionCloner->start());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, BSON_ARRAY(doc)));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(1, collectionStats.insertCount);

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    const BSONObj doc2 = BSON("_id" << 2);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, BSON_ARRAY(doc2), "nextBatch"));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(2, collectionStats.insertCount);

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(0, emptyArray, "nextBatch"));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(2, collectionStats.insertCount);
    ASSERT_TRUE(collectionStats.commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, MiddleBatchContainsNoDocuments) {
    ASSERT_OK(collectionCloner->start());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats.initCalled);

    const BSONObj doc = BSON("_id" << 1);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, BSON_ARRAY(doc)));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(1, collectionStats.insertCount);

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    BSONArray emptyArray;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(1, emptyArray, "nextBatch"));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(1, collectionStats.insertCount);

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    const BSONObj doc2 = BSON("_id" << 2);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCursorResponse(0, BSON_ARRAY(doc2), "nextBatch"));
    }

    collectionCloner->waitForDbWorker();

    ASSERT_EQUALS(2, collectionStats.insertCount);
    ASSERT_TRUE(collectionStats.commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

}  // namespace
