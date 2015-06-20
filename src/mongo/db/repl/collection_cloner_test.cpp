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
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

class CollectionClonerTest : public BaseClonerTest {
public:
    BaseCloner* getCloner() const override;

protected:
    void setUp() override;
    void tearDown() override;

    CollectionOptions options;
    std::unique_ptr<CollectionCloner> collectionCloner;
};

void CollectionClonerTest::setUp() {
    BaseClonerTest::setUp();
    options.reset();
    options.storageEngine = BSON("storageEngine1" << BSONObj());
    collectionCloner.reset(new CollectionCloner(
        &getExecutor(),
        target,
        nss,
        options,
        stdx::bind(&CollectionClonerTest::setStatus, this, stdx::placeholders::_1),
        storageInterface.get()));
}

void CollectionClonerTest::tearDown() {
    BaseClonerTest::tearDown();
    // Executor may still invoke collection cloner's callback before shutting down.
    collectionCloner.reset();
    options.reset();
}

BaseCloner* CollectionClonerTest::getCloner() const {
    return collectionCloner.get();
}

TEST_F(CollectionClonerTest, InvalidConstruction) {
    ReplicationExecutor& executor = getExecutor();

    const auto& cb = [](const Status&) { FAIL("should not reach here"); };

    // Null executor.
    {
        CollectionCloner::StorageInterface* si = storageInterface.get();
        ASSERT_THROWS(CollectionCloner(nullptr, target, nss, options, cb, si), UserException);
    }

    // Null storage interface
    ASSERT_THROWS(CollectionCloner(&executor, target, nss, options, cb, nullptr), UserException);

    // Invalid namespace.
    {
        NamespaceString badNss("db.");
        CollectionCloner::StorageInterface* si = storageInterface.get();
        ASSERT_THROWS(CollectionCloner(&executor, target, badNss, options, cb, si), UserException);
    }

    // Invalid collection options.
    {
        CollectionOptions invalidOptions;
        invalidOptions.storageEngine = BSON("storageEngine1"
                                            << "not a document");
        CollectionCloner::StorageInterface* si = storageInterface.get();
        ASSERT_THROWS(CollectionCloner(&executor, target, nss, invalidOptions, cb, si),
                      UserException);
    }

    // Callback function cannot be null.
    {
        CollectionCloner::CallbackFn nullCb;
        CollectionCloner::StorageInterface* si = storageInterface.get();
        ASSERT_THROWS(CollectionCloner(&executor, target, nss, options, nullCb, si), UserException);
    }
}

TEST_F(CollectionClonerTest, ClonerLifeCycle) {
    testLifeCycle();
}

TEST_F(CollectionClonerTest, FirstRemoteCommand) {
    ASSERT_OK(collectionCloner->start());

    auto net = getNet();
    ASSERT_TRUE(net->hasReadyRequests());
    NetworkOperationIterator noi = net->getNextReadyRequest();
    auto&& noiRequest = noi->getRequest();
    ASSERT_EQUALS(nss.db().toString(), noiRequest.dbname);
    ASSERT_EQUALS("listIndexes", std::string(noiRequest.cmdObj.firstElementFieldName()));
    ASSERT_EQUALS(nss.coll().toString(), noiRequest.cmdObj.firstElement().valuestrsafe());
    ASSERT_FALSE(net->hasReadyRequests());
    ASSERT_TRUE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, RemoteCollectionMissing) {
    ASSERT_OK(collectionCloner->start());

    processNetworkResponse(BSON("ok" << 0 << "errmsg"
                                     << ""
                                     << "code" << ErrorCodes::NamespaceNotFound));

    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

// A collection may have no indexes. The cloner will produce a warning but
// will still proceed with cloning.
TEST_F(CollectionClonerTest, ListIndexesReturnedNoIndexes) {
    ASSERT_OK(collectionCloner->start());

    // Using a non-zero cursor to ensure that
    // the cloner stops the fetcher from retrieving more results.
    processNetworkResponse(createListIndexesResponse(1, BSONArray()));

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    ASSERT_TRUE(getNet()->hasReadyRequests());
}

TEST_F(CollectionClonerTest, BeginCollectionScheduleDbWorkFailed) {
    ASSERT_OK(collectionCloner->start());

    // Replace scheduleDbWork function so that cloner will fail to schedule DB work after
    // getting index specs.
    collectionCloner->setScheduleDbWorkFn([](const ReplicationExecutor::CallbackFn& workFn) {
        return StatusWith<ReplicationExecutor::CallbackHandle>(ErrorCodes::UnknownError, "");
    });

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));

    ASSERT_EQUALS(ErrorCodes::UnknownError, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, BeginCollectionCallbackCanceled) {
    ASSERT_OK(collectionCloner->start());

    // Replace scheduleDbWork function so that the callback for beginCollection is canceled
    // immediately after scheduling.
    auto&& executor = getExecutor();
    collectionCloner->setScheduleDbWorkFn([&](const ReplicationExecutor::CallbackFn& workFn) {
        // Schedule as non-exclusive task to allow us to cancel it before the executor is able
        // to invoke the callback.
        auto scheduleResult = executor.scheduleWork(workFn);
        ASSERT_OK(scheduleResult.getStatus());
        executor.cancel(scheduleResult.getValue());
        return scheduleResult;
    });

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, BeginCollectionFailed) {
    ASSERT_OK(collectionCloner->start());

    storageInterface->beginCollectionFn = [&](OperationContext* txn,
                                              const NamespaceString& theNss,
                                              const CollectionOptions& theOptions,
                                              const std::vector<BSONObj>& theIndexSpecs) {
        return Status(ErrorCodes::OperationFailed, "");
    };

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));

    collectionCloner->waitForDbWorker();

    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, BeginCollection) {
    ASSERT_OK(collectionCloner->start());

    NamespaceString collNss;
    CollectionOptions collOptions;
    std::vector<BSONObj> collIndexSpecs;
    storageInterface->beginCollectionFn = [&](OperationContext* txn,
                                              const NamespaceString& theNss,
                                              const CollectionOptions& theOptions,
                                              const std::vector<BSONObj>& theIndexSpecs) {
        ASSERT(txn);
        collNss = theNss;
        collOptions = theOptions;
        collIndexSpecs = theIndexSpecs;
        return Status::OK();
    };

    // Split listIndexes response into 2 batches: first batch contains specs[0] and specs[1];
    // second batch contains specs[2]
    const std::vector<BSONObj> specs = {idIndexSpec,
                                        BSON("v" << 1 << "key" << BSON("a" << 1) << "name"
                                                 << "a_1"
                                                 << "ns" << nss.ns()),
                                        BSON("v" << 1 << "key" << BSON("b" << 1) << "name"
                                                 << "b_1"
                                                 << "ns" << nss.ns())};

    processNetworkResponse(createListIndexesResponse(1, BSON_ARRAY(specs[0] << specs[1])));

    // 'status' should not be modified because cloning is not finished.
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(specs[2]), "nextBatch"));

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
    bool collectionCreated = false;
    storageInterface->beginCollectionFn = [&](OperationContext* txn,
                                              const NamespaceString& theNss,
                                              const CollectionOptions& theOptions,
                                              const std::vector<BSONObj>& theIndexSpecs) {
        collectionCreated = true;
        getExecutor().shutdown();
        return Status::OK();
    };

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCreated);

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, FindCommandAfterBeginCollection) {
    ASSERT_OK(collectionCloner->start());

    bool collectionCreated = false;
    storageInterface->beginCollectionFn = [&](OperationContext* txn,
                                              const NamespaceString& theNss,
                                              const CollectionOptions& theOptions,
                                              const std::vector<BSONObj>& theIndexSpecs) {
        collectionCreated = true;
        return Status::OK();
    };

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCreated);

    // Fetcher should be scheduled after cloner creates collection.
    auto net = getNet();
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

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));

    collectionCloner->waitForDbWorker();

    processNetworkResponse(BSON("ok" << 0 << "errmsg"
                                     << ""
                                     << "code" << ErrorCodes::CursorNotFound));

    ASSERT_EQUALS(ErrorCodes::CursorNotFound, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, FindCommandCanceled) {
    ASSERT_OK(collectionCloner->start());

    scheduleNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));

    auto net = getNet();
    net->runReadyNetworkOperations();

    collectionCloner->waitForDbWorker();

    scheduleNetworkResponse(BSON("ok" << 1));

    collectionCloner->cancel();

    net->runReadyNetworkOperations();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsScheduleDbWorkFailed) {
    ASSERT_OK(collectionCloner->start());

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));

    collectionCloner->waitForDbWorker();

    // Replace scheduleDbWork function so that cloner will fail to schedule DB work after
    // getting documents.
    collectionCloner->setScheduleDbWorkFn([](const ReplicationExecutor::CallbackFn& workFn) {
        return StatusWith<ReplicationExecutor::CallbackHandle>(ErrorCodes::UnknownError, "");
    });

    const BSONObj doc = BSON("_id" << 1);
    processNetworkResponse(createCursorResponse(0, BSON_ARRAY(doc)));

    ASSERT_EQUALS(ErrorCodes::UnknownError, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsCallbackCanceled) {
    ASSERT_OK(collectionCloner->start());

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));

    collectionCloner->waitForDbWorker();

    // Replace scheduleDbWork function so that the callback for insertDocuments is canceled
    // immediately after scheduling.
    auto&& executor = getExecutor();
    collectionCloner->setScheduleDbWorkFn([&](const ReplicationExecutor::CallbackFn& workFn) {
        // Schedule as non-exclusive task to allow us to cancel it before the executor is able
        // to invoke the callback.
        auto scheduleResult = executor.scheduleWork(workFn);
        ASSERT_OK(scheduleResult.getStatus());
        executor.cancel(scheduleResult.getValue());
        return scheduleResult;
    });

    const BSONObj doc = BSON("_id" << 1);
    processNetworkResponse(createCursorResponse(0, BSON_ARRAY(doc)));

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsFailed) {
    ASSERT_OK(collectionCloner->start());

    bool insertDocumentsCalled = false;
    storageInterface->insertDocumentsFn = [&](OperationContext* txn,
                                              const NamespaceString& theNss,
                                              const std::vector<BSONObj>& theDocuments) {
        insertDocumentsCalled = true;
        return Status(ErrorCodes::OperationFailed, "");
    };

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));

    collectionCloner->waitForDbWorker();

    processNetworkResponse(createCursorResponse(0, BSONArray()));

    collectionCloner->wait();

    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsSingleBatch) {
    ASSERT_OK(collectionCloner->start());

    std::vector<BSONObj> collDocuments;
    storageInterface->insertDocumentsFn = [&](OperationContext* txn,
                                              const NamespaceString& theNss,
                                              const std::vector<BSONObj>& theDocuments) {
        ASSERT(txn);
        collDocuments = theDocuments;
        return Status::OK();
    };

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));

    collectionCloner->waitForDbWorker();

    const BSONObj doc = BSON("_id" << 1);
    processNetworkResponse(createCursorResponse(0, BSON_ARRAY(doc)));

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(1U, collDocuments.size());
    ASSERT_EQUALS(doc, collDocuments[0]);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsMultipleBatches) {
    ASSERT_OK(collectionCloner->start());

    std::vector<BSONObj> collDocuments;
    storageInterface->insertDocumentsFn = [&](OperationContext* txn,
                                              const NamespaceString& theNss,
                                              const std::vector<BSONObj>& theDocuments) {
        ASSERT(txn);
        collDocuments = theDocuments;
        return Status::OK();
    };

    processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));

    collectionCloner->waitForDbWorker();

    const BSONObj doc = BSON("_id" << 1);
    processNetworkResponse(createCursorResponse(1, BSON_ARRAY(doc)));

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(1U, collDocuments.size());
    ASSERT_EQUALS(doc, collDocuments[0]);

    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    const BSONObj doc2 = BSON("_id" << 1);
    processNetworkResponse(createCursorResponse(0, BSON_ARRAY(doc2), "nextBatch"));

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(1U, collDocuments.size());
    ASSERT_EQUALS(doc2, collDocuments[0]);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

}  // namespace
