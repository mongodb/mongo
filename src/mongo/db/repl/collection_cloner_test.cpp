/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/repl/cloner_test_fixture.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace repl {

class MockCallbackState final : public mongo::executor::TaskExecutor::CallbackState {
public:
    MockCallbackState() = default;
    void cancel() override {}
    void waitForCompletion() override {}
    bool isCanceled() const override {
        return false;
    }
};

class CollectionClonerTest : public ClonerTestFixture {
public:
    CollectionClonerTest() {}

protected:
    void setUp() final {
        ClonerTestFixture::setUp();
        _collectionStats = std::make_shared<CollectionMockStats>();
        _standardCreateCollectionFn = [this](const NamespaceString& nss,
                                             const CollectionOptions& options,
                                             const BSONObj idIndexSpec,
                                             const std::vector<BSONObj>& nonIdIndexSpecs)
            -> StatusWith<std::unique_ptr<CollectionBulkLoaderMock>> {
            auto localLoader = std::make_unique<CollectionBulkLoaderMock>(_collectionStats);
            Status result = localLoader->init(nonIdIndexSpecs);
            if (!result.isOK())
                return result;

            _loader = localLoader.get();

            return std::move(localLoader);
        };
        _storageInterface.createCollectionForBulkFn = _standardCreateCollectionFn;

        _mockServer->assignCollectionUuid(_nss.ns(), _collUuid);
        _mockServer->setCommandReply("replSetGetRBID",
                                     BSON("ok" << 1 << "rbid" << _sharedData->getRollBackId()));
    }
    std::unique_ptr<CollectionCloner> makeCollectionCloner(
        CollectionOptions options = CollectionOptions()) {
        options.uuid = _collUuid;
        _options = options;
        return std::make_unique<CollectionCloner>(_nss,
                                                  options,
                                                  _sharedData.get(),
                                                  _source,
                                                  _mockClient.get(),
                                                  &_storageInterface,
                                                  _dbWorkThreadPool.get());
    }

    ProgressMeter& getProgressMeter(CollectionCloner* cloner) {
        return cloner->_progressMeter;
    }

    std::vector<BSONObj>& getIndexSpecs(CollectionCloner* cloner) {
        return cloner->_indexSpecs;
    }

    BSONObj& getIdIndexSpec(CollectionCloner* cloner) {
        return cloner->_idIndexSpec;
    }

    std::shared_ptr<CollectionMockStats> _collectionStats;  // Used by the _loader.
    StorageInterfaceMock::CreateCollectionForBulkFn _standardCreateCollectionFn;
    CollectionBulkLoaderMock* _loader = nullptr;  // Owned by CollectionCloner.
    CollectionOptions _options;

    NamespaceString _nss = {"testDb", "testColl"};
    UUID _collUuid = UUID::gen();
    BSONObj _idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                    << "_id_");

    std::vector<BSONObj> _secondaryIndexSpecs{BSON("v" << 1 << "key" << BSON("a" << 1) << "name"
                                                       << "a_1"),
                                              BSON("v" << 1 << "key" << BSON("b" << 1) << "name"
                                                       << "b_1")};
};

TEST_F(CollectionClonerTest, CountStage) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("count");
    _mockServer->setCommandReply("count", createCountResponse(100));
    ASSERT_OK(cloner->run());
    ASSERT_EQ(100, getProgressMeter(cloner.get()).total());
}

// On a negative count, the CollectionCloner should use a zero count.
TEST_F(CollectionClonerTest, CountStageNegativeCount) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("count");
    _mockServer->setCommandReply("count", createCountResponse(-100));
    ASSERT_OK(cloner->run());
    ASSERT_EQ(0, getProgressMeter(cloner.get()).total());
}

// On NamespaceNotFound, the CollectionCloner should exit without doing anything.
TEST_F(CollectionClonerTest, CountStageNamespaceNotFound) {
    auto cloner = makeCollectionCloner();
    _mockServer->setCommandReply("count", Status(ErrorCodes::NamespaceNotFound, "NoSuchUuid"));
    ASSERT_OK(cloner->run());
}

TEST_F(CollectionClonerTest, CollectionClonerPassesThroughNonRetriableErrorFromCountCommand) {
    auto cloner = makeCollectionCloner();
    _mockServer->setCommandReply("count", Status(ErrorCodes::OperationFailed, ""));
    ASSERT_EQUALS(ErrorCodes::OperationFailed, cloner->run());
}

TEST_F(CollectionClonerTest, CollectionClonerPassesThroughCommandStatusErrorFromCountCommand) {
    auto cloner = makeCollectionCloner();
    _mockServer->setCommandReply("count", Status(ErrorCodes::OperationFailed, ""));
    _mockServer->setCommandReply("count",
                                 BSON("ok" << 0 << "errmsg"
                                           << "TEST error"
                                           << "code" << int(ErrorCodes::OperationFailed)));
    auto status = cloner->run();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    ASSERT_STRING_CONTAINS(status.reason(), "TEST error");
}

TEST_F(CollectionClonerTest, CollectionClonerReturnsNoSuchKeyOnMissingDocumentCountFieldName) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("count");
    _mockServer->setCommandReply("count", BSON("ok" << 1));
    auto status = cloner->run();
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status);
}

TEST_F(CollectionClonerTest, ListIndexesReturnedNoIndexes) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("listIndexes");
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), BSONArray()));
    ASSERT_OK(cloner->run());
    ASSERT(getIdIndexSpec(cloner.get()).isEmpty());
    ASSERT(getIndexSpecs(cloner.get()).empty());
    ASSERT_EQ(0, cloner->getStats().indexes);
}

// NamespaceNotFound is treated the same as no index.
TEST_F(CollectionClonerTest, ListIndexesReturnedNamespaceNotFound) {
    auto cloner = makeCollectionCloner();
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes",
                                 Status(ErrorCodes::NamespaceNotFound, "No indexes here."));
    ASSERT_OK(cloner->run());
    ASSERT(!_loader);  // We expect not to have run the create collection.
    ASSERT(getIdIndexSpec(cloner.get()).isEmpty());
    ASSERT(getIndexSpecs(cloner.get()).empty());
    ASSERT_EQ(0, cloner->getStats().indexes);
}

TEST_F(CollectionClonerTest, ListIndexesHasResults) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("listIndexes");
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply(
        "listIndexes",
        createCursorResponse(
            _nss.ns(),
            BSON_ARRAY(_secondaryIndexSpecs[0] << _idIndexSpec << _secondaryIndexSpecs[1])));
    ASSERT_OK(cloner->run());
    ASSERT_BSONOBJ_EQ(_idIndexSpec, getIdIndexSpec(cloner.get()));
    ASSERT_EQ(2, getIndexSpecs(cloner.get()).size());
    ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[0], getIndexSpecs(cloner.get())[0]);
    ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[1], getIndexSpecs(cloner.get())[1]);
    ASSERT_EQ(3, cloner->getStats().indexes);
}

TEST_F(CollectionClonerTest, CollectionClonerResendsListIndexesCommandOnRetriableError) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("listIndexes");
    _mockServer->setCommandReply("count", createCountResponse(1));

    // Respond once with failure, once with success.
    _mockServer->setCommandReply(
        "listIndexes",
        {Status(ErrorCodes::HostNotFound, "HostNotFound"),
         createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec << _secondaryIndexSpecs[0]))});
    ASSERT_OK(cloner->run());
    ASSERT_BSONOBJ_EQ(_idIndexSpec, getIdIndexSpec(cloner.get()));
    ASSERT_EQ(1, getIndexSpecs(cloner.get()).size());
    ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[0], getIndexSpecs(cloner.get())[0]);
    ASSERT_EQ(2, cloner->getStats().indexes);
}

TEST_F(CollectionClonerTest, BeginCollection) {
    NamespaceString collNss;
    CollectionOptions collOptions;
    BSONObj collIdIndexSpec;
    std::vector<BSONObj> collSecondaryIndexSpecs;

    _storageInterface.createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                      const CollectionOptions& theOptions,
                                                      const BSONObj idIndexSpec,
                                                      const std::vector<BSONObj>& nonIdIndexSpecs) {
        collNss = theNss;
        collOptions = theOptions;
        collIdIndexSpec = idIndexSpec;
        collSecondaryIndexSpecs = nonIdIndexSpecs;
        return _standardCreateCollectionFn(theNss, theOptions, idIndexSpec, nonIdIndexSpecs);
    };

    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("createCollection");
    _mockServer->setCommandReply("count", createCountResponse(1));
    BSONArrayBuilder indexSpecs;
    indexSpecs.append(_idIndexSpec);
    for (const auto& secondaryIndexSpec : _secondaryIndexSpecs) {
        indexSpecs.append(secondaryIndexSpec);
    }
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), indexSpecs.arr()));

    ASSERT_EQUALS(Status::OK(), cloner->run());

    ASSERT_EQUALS(_nss.ns(), collNss.ns());
    ASSERT_BSONOBJ_EQ(_options.toBSON(), collOptions.toBSON());
    ASSERT_EQUALS(_secondaryIndexSpecs.size(), collSecondaryIndexSpecs.size());
    for (std::vector<BSONObj>::size_type i = 0; i < _secondaryIndexSpecs.size(); ++i) {
        ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[i], collSecondaryIndexSpecs[i]);
    }
}

TEST_F(CollectionClonerTest, BeginCollectionFailed) {
    _storageInterface.createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                      const CollectionOptions& theOptions,
                                                      const BSONObj idIndexSpec,
                                                      const std::vector<BSONObj>& theIndexSpecs) {
        return Status(ErrorCodes::OperationFailed, "");
    };

    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("createCollection");
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), BSONArray()));
    ASSERT_EQUALS(ErrorCodes::OperationFailed, cloner->run());
}

TEST_F(CollectionClonerTest, InsertDocumentsSingleBatch) {
    // Set up data for preliminary stages
    _mockServer->setCommandReply("count", createCountResponse(2));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));

    auto cloner = makeCollectionCloner();
    ASSERT_OK(cloner->run());

    ASSERT_EQUALS(2, _collectionStats->insertCount);
    ASSERT_TRUE(_collectionStats->commitCalled);

    auto stats = cloner->getStats();
    ASSERT_EQUALS(1u, stats.receivedBatches);
}

TEST_F(CollectionClonerTest, InsertDocumentsMultipleBatches) {
    // Set up data for preliminary stages
    _mockServer->setCommandReply("count", createCountResponse(2));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));
    _mockServer->insert(_nss.ns(), BSON("_id" << 3));

    auto cloner = makeCollectionCloner();
    cloner->setBatchSize_forTest(2);

    ASSERT_OK(cloner->run());

    ASSERT_EQUALS(3, _collectionStats->insertCount);
    ASSERT_TRUE(_collectionStats->commitCalled);
    auto stats = cloner->getStats();
    ASSERT_EQUALS(2u, stats.receivedBatches);
}

TEST_F(CollectionClonerTest, InsertDocumentsScheduleDBWorkFailed) {
    // Set up data for preliminary stages
    _mockServer->setCommandReply("count", createCountResponse(2));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));
    _mockServer->insert(_nss.ns(), BSON("_id" << 3));

    auto cloner = makeCollectionCloner();
    // Stop before running the query to set up the failure.
    auto collClonerBeforeFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto timesEntered = collClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'CollectionCloner', stage: 'query', nss: '" + _nss.ns() + "'}"));

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_EQUALS(ErrorCodes::UnknownError, cloner->run());
    });
    // Wait for the failpoint to be reached
    collClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);
    // Replace scheduleDbWork function so that cloner will fail to schedule DB work after
    // getting documents.
    cloner->setScheduleDbWorkFn_forTest([](const executor::TaskExecutor::CallbackFn& workFn) {
        return StatusWith<executor::TaskExecutor::CallbackHandle>(ErrorCodes::UnknownError, "");
    });

    // Continue and finish. Final status is checked in the thread.
    collClonerBeforeFailPoint->setMode(FailPoint::off, 0);
    clonerThread.join();
}

TEST_F(CollectionClonerTest, InsertDocumentsCallbackCanceled) {
    // Set up data for preliminary stages
    _mockServer->setCommandReply("count", createCountResponse(2));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));
    _mockServer->insert(_nss.ns(), BSON("_id" << 3));

    auto cloner = makeCollectionCloner();
    // Stop before running the query to set up the failure.
    auto collClonerBeforeFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto timesEntered = collClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'CollectionCloner', stage: 'query', nss: '" + _nss.ns() + "'}"));

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_EQUALS(ErrorCodes::CallbackCanceled, cloner->run());
    });
    // Wait for the failpoint to be reached
    collClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);
    // Replace scheduleDbWork function so that cloner will fail to schedule DB work after
    // getting documents.
    cloner->setScheduleDbWorkFn_forTest([&](const executor::TaskExecutor::CallbackFn& workFn) {
        executor::TaskExecutor::CallbackHandle handle(std::make_shared<MockCallbackState>());
        mongo::executor::TaskExecutor::CallbackArgs args{
            nullptr,
            handle,
            {ErrorCodes::CallbackCanceled, "Never run, but treat like cancelled."}};
        workFn(args);
        return StatusWith<executor::TaskExecutor::CallbackHandle>(handle);
    });

    // Continue and finish. Final status is checked in the thread.
    collClonerBeforeFailPoint->setMode(FailPoint::off, 0);
    clonerThread.join();
}

TEST_F(CollectionClonerTest, InsertDocumentsFailed) {
    // Set up data for preliminary stages
    _mockServer->setCommandReply("count", createCountResponse(2));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));
    _mockServer->insert(_nss.ns(), BSON("_id" << 3));

    auto cloner = makeCollectionCloner();
    // Stop before running the query to set up the failure.
    auto collClonerBeforeFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto timesEntered = collClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'CollectionCloner', stage: 'query', nss: '" + _nss.ns() + "'}"));

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_EQUALS(ErrorCodes::OperationFailed, cloner->run());
    });

    // Wait for the failpoint to be reached
    collClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);

    // Modify the loader so insert documents fails.
    ASSERT(_loader != nullptr);
    _loader->insertDocsFn = [](const std::vector<BSONObj>::const_iterator begin,
                               const std::vector<BSONObj>::const_iterator end) {
        return Status(ErrorCodes::OperationFailed, "");
    };

    // Continue and finish. Final status is checked in the thread.
    collClonerBeforeFailPoint->setMode(FailPoint::off, 0);
    clonerThread.join();
}

TEST_F(CollectionClonerTest, DoNotCreateIDIndexIfAutoIndexIdUsed) {
    NamespaceString collNss;
    CollectionOptions collOptions;
    // We initialize collIndexSpecs with fake information to ensure it is overwritten by an empty
    // vector.
    std::vector<BSONObj> collIndexSpecs{BSON("fakeindexkeys" << 1)};
    _storageInterface.createCollectionForBulkFn = [&,
                                                   this](const NamespaceString& theNss,
                                                         const CollectionOptions& theOptions,
                                                         const BSONObj idIndexSpec,
                                                         const std::vector<BSONObj>& theIndexSpecs)
        -> StatusWith<std::unique_ptr<CollectionBulkLoader>> {
        collNss = theNss;
        collOptions = theOptions;
        collIndexSpecs = theIndexSpecs;
        return _standardCreateCollectionFn(theNss, theOptions, idIndexSpec, theIndexSpecs);
    };

    const BSONObj doc = BSON("_id" << 1);
    _mockServer->insert(_nss.ns(), doc);

    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), BSONArray()));

    CollectionOptions options;
    options.autoIndexId = CollectionOptions::NO;
    auto cloner = makeCollectionCloner(options);
    ASSERT_OK(cloner->run());
    ASSERT_EQUALS(1, _collectionStats->insertCount);
    ASSERT_TRUE(_collectionStats->commitCalled);
    ASSERT_EQ(collOptions.autoIndexId, CollectionOptions::NO);
    ASSERT_EQ(0UL, collIndexSpecs.size());
    ASSERT_EQ(collNss, _nss);
}

}  // namespace repl
}  // namespace mongo
