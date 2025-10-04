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

#include "mongo/base/status.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/repl/initial_sync/collection_cloner.h"
#include "mongo/db/repl/initial_sync/initial_sync_cloner_test_fixture.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/tenant_id.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#include <functional>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

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

const std::string kTestNs = "testDb.testColl";

class CollectionClonerTest : public InitialSyncClonerTestFixture {
public:
    CollectionClonerTest()
        : _nss(NamespaceString::createNamespaceString_forTest(boost::none, kTestNs)) {}

protected:
    void setUp() override {
        InitialSyncClonerTestFixture::setUp();
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

        _mockServer->assignCollectionUuid(_nss.ns_forTest(), _collUuid);
        _mockServer->setCommandReply("replSetGetRBID",
                                     BSON("ok" << 1 << "rbid" << getSharedData()->getRollBackId()));
    }
    std::unique_ptr<CollectionCloner> makeCollectionCloner(
        CollectionOptions options = CollectionOptions()) {
        options.uuid = _collUuid;
        _options = options;
        return std::make_unique<CollectionCloner>(_nss,
                                                  options,
                                                  getSharedData(),
                                                  _source,
                                                  _mockClient.get(),
                                                  &_storageInterface,
                                                  _dbWorkThreadPool.get());
    }

    ProgressMeter& getProgressMeter(CollectionCloner* cloner) {
        return cloner->_progressMeter;
    }

    std::vector<BSONObj> getIndexSpecs(CollectionCloner* cloner) {
        std::vector<BSONObj> indexSpecs = cloner->_readyIndexSpecs;
        for (const auto& unfinishedSpec : cloner->_unfinishedIndexSpecs) {
            indexSpecs.push_back(unfinishedSpec["spec"].Obj());
        }
        return indexSpecs;
    }

    void setMockServerReplies(const StatusWith<mongo::BSONObj>& collStatsSwBson,
                              const StatusWith<mongo::BSONObj>& countSwBson,
                              const StatusWith<mongo::BSONObj>& listIndexesSwBson) {
        _mockServer->setCommandReply("collStats", collStatsSwBson);
        _mockServer->setCommandReply("count", countSwBson);
        _mockServer->setCommandReply("listIndexes", listIndexesSwBson);
    }

    BSONObj& getIdIndexSpec(CollectionCloner* cloner) {
        return cloner->_idIndexSpec;
    }

    std::shared_ptr<CollectionMockStats> _collectionStats;  // Used by the _loader.
    StorageInterfaceMock::CreateCollectionForBulkFn _standardCreateCollectionFn;
    CollectionBulkLoaderMock* _loader = nullptr;  // Owned by CollectionCloner.
    CollectionOptions _options;

    NamespaceString _nss;
    UUID _collUuid = UUID::gen();
    BSONObj _idIndexSpec =
        BSON("v" << 1 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName);

    std::vector<BSONObj> _secondaryIndexSpecs{BSON("v" << 1 << "key" << BSON("a" << 1) << "name"
                                                       << "a_1"),
                                              BSON("v" << 1 << "key" << BSON("b" << 1) << "name"
                                                       << "b_1")};
};

class CollectionClonerTestResumable : public CollectionClonerTest {
protected:
    void setUp() override {
        CollectionClonerTest::setUp();
        setInitialSyncId();
    }
};

TEST_F(CollectionClonerTestResumable, CollectionClonerPassesThroughErrorFromCollStatsCommand) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("count");
    // The collection cloner pre-stage makes a remote call to collStats to store in-progress
    // metrics.
    _mockServer->setCommandReply("collStats", Status(ErrorCodes::OperationFailed, ""));
    _mockServer->setCommandReply("count", createCountResponse(100));
    ASSERT_OK(cloner->run());
    ASSERT_EQ(100, getProgressMeter(cloner.get()).total());
}

TEST_F(CollectionClonerTestResumable, CountStage) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("count");
    // The collection cloner pre-stage makes a remote call to collStats to store in-progress
    // metrics.
    _mockServer->setCommandReply("collStats", BSON("size" << 10000));
    _mockServer->setCommandReply("count", createCountResponse(100));
    ASSERT_OK(cloner->run());
    ASSERT_EQ(100, getProgressMeter(cloner.get()).total());
}

// On a negative count, the CollectionCloner should use a zero count.
TEST_F(CollectionClonerTestResumable, CountStageNegativeCount) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("count");
    // The collection cloner pre-stage makes a remote call to collStats to store in-progress
    // metrics.
    _mockServer->setCommandReply("collStats", BSON("size" << 10000));
    _mockServer->setCommandReply("count", createCountResponse(-100));
    ASSERT_OK(cloner->run());
    ASSERT_EQ(0, getProgressMeter(cloner.get()).total());
}

// On NamespaceNotFound, the CollectionCloner should exit without doing anything.
TEST_F(CollectionClonerTestResumable, CountStageNamespaceNotFound) {
    auto cloner = makeCollectionCloner();
    // The collection cloner pre-stage makes a remote call to collStats to store in-progress
    // metrics.
    _mockServer->setCommandReply("collStats", BSON("size" << 10000));
    _mockServer->setCommandReply("count", Status(ErrorCodes::NamespaceNotFound, "NoSuchUuid"));
    ASSERT_OK(cloner->run());
}

TEST_F(CollectionClonerTestResumable,
       CollectionClonerPassesThroughNonRetriableErrorFromCountCommand) {
    auto cloner = makeCollectionCloner();
    // The collection cloner pre-stage makes a remote call to collStats to store in-progress
    // metrics.
    _mockServer->setCommandReply("collStats", BSON("size" << 10000));
    _mockServer->setCommandReply("count", Status(ErrorCodes::OperationFailed, ""));
    ASSERT_EQUALS(ErrorCodes::OperationFailed, cloner->run());
}

TEST_F(CollectionClonerTestResumable,
       CollectionClonerPassesThroughCommandStatusErrorFromCountCommand) {
    auto cloner = makeCollectionCloner();
    // The collection cloner pre-stage makes a remote call to collStats to store in-progress
    // metrics.
    _mockServer->setCommandReply("collStats", BSON("size" << 10000));
    _mockServer->setCommandReply("count", Status(ErrorCodes::OperationFailed, ""));
    _mockServer->setCommandReply("count",
                                 BSON("ok" << 0 << "errmsg"
                                           << "TEST error"
                                           << "code" << int(ErrorCodes::OperationFailed)));
    auto status = cloner->run();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    ASSERT_STRING_CONTAINS(status.reason(), "TEST error");
}

TEST_F(CollectionClonerTestResumable,
       CollectionClonerReturnsNoSuchKeyOnMissingDocumentCountFieldName) {
    auto cloner = makeCollectionCloner();
    // The collection cloner pre-stage makes a remote call to collStats to store in-progress
    // metrics.
    _mockServer->setCommandReply("collStats", BSON("size" << 10000));
    cloner->setStopAfterStage_forTest("count");
    _mockServer->setCommandReply("count", BSON("ok" << 1));
    auto status = cloner->run();
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status);
}

TEST_F(CollectionClonerTestResumable, ListIndexesReturnedNoIndexes) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("listIndexes");
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(1),
                         createCursorResponse(_nss.ns_forTest(), BSONArray()));
    ASSERT_OK(cloner->run());
    ASSERT(getIdIndexSpec(cloner.get()).isEmpty());
    ASSERT(getIndexSpecs(cloner.get()).empty());
    ASSERT_EQ(0, cloner->getStats().indexes);
}

// NamespaceNotFound is treated the same as no index.
TEST_F(CollectionClonerTestResumable, ListIndexesReturnedNamespaceNotFound) {
    auto cloner = makeCollectionCloner();
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(1),
                         Status(ErrorCodes::NamespaceNotFound, "No indexes here."));
    ASSERT_OK(cloner->run());
    ASSERT(!_loader);  // We expect not to have run the create collection.
    ASSERT(getIdIndexSpec(cloner.get()).isEmpty());
    ASSERT(getIndexSpecs(cloner.get()).empty());
    ASSERT_EQ(0, cloner->getStats().indexes);
}

TEST_F(CollectionClonerTestResumable, ListIndexesHasResults) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("listIndexes");
    setMockServerReplies(
        BSON("size" << 10),
        createCountResponse(1),
        createCursorResponse(
            _nss.ns_forTest(),
            BSON_ARRAY(_secondaryIndexSpecs[0] << _idIndexSpec << _secondaryIndexSpecs[1])));
    ASSERT_OK(cloner->run());
    ASSERT_BSONOBJ_EQ(_idIndexSpec, getIdIndexSpec(cloner.get()));
    ASSERT_EQ(2, getIndexSpecs(cloner.get()).size());
    ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[0], getIndexSpecs(cloner.get())[0]);
    ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[1], getIndexSpecs(cloner.get())[1]);
    ASSERT_EQ(3, cloner->getStats().indexes);
}

TEST_F(CollectionClonerTestResumable, CollectionClonerResendsListIndexesCommandOnRetriableError) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("listIndexes");

    // Respond to listIndexes once with failure, once with success.
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(1),
                         Status(ErrorCodes::HostNotFound, "HostNotFound"));
    _mockServer->setCommandReply(
        "listIndexes",
        createCursorResponse(_nss.ns_forTest(),
                             BSON_ARRAY(_idIndexSpec << _secondaryIndexSpecs[0])));

    ASSERT_OK(cloner->run());
    ASSERT_BSONOBJ_EQ(_idIndexSpec, getIdIndexSpec(cloner.get()));
    ASSERT_EQ(1, getIndexSpecs(cloner.get()).size());
    ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[0], getIndexSpecs(cloner.get())[0]);
    ASSERT_EQ(2, cloner->getStats().indexes);
}

TEST_F(CollectionClonerTestResumable, BeginCollection) {
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

    BSONArrayBuilder indexSpecs;
    indexSpecs.append(_idIndexSpec);
    for (const auto& secondaryIndexSpec : _secondaryIndexSpecs) {
        indexSpecs.append(secondaryIndexSpec);
    }
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(1),
                         createCursorResponse(_nss.ns_forTest(), indexSpecs.arr()));

    ASSERT_EQUALS(Status::OK(), cloner->run());

    ASSERT_EQUALS(_nss.ns_forTest(), collNss.ns_forTest());
    ASSERT_BSONOBJ_EQ(_options.toBSON(), collOptions.toBSON());
    ASSERT_EQUALS(_secondaryIndexSpecs.size(), collSecondaryIndexSpecs.size());
    for (std::vector<BSONObj>::size_type i = 0; i < _secondaryIndexSpecs.size(); ++i) {
        ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[i], collSecondaryIndexSpecs[i]);
    }
}

TEST_F(CollectionClonerTestResumable, BeginCollectionFailed) {
    _storageInterface.createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                      const CollectionOptions& theOptions,
                                                      const BSONObj idIndexSpec,
                                                      const std::vector<BSONObj>& theIndexSpecs) {
        return Status(ErrorCodes::OperationFailed, "");
    };

    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("createCollection");
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(1),
                         createCursorResponse(_nss.ns_forTest(), BSONArray()));
    ASSERT_EQUALS(ErrorCodes::OperationFailed, cloner->run());
}

TEST_F(CollectionClonerTestResumable, InsertDocumentsSingleBatch) {
    // Set up data for preliminary stages
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(2),
                         createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(_idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss, BSON("_id" << 1));
    _mockServer->insert(_nss, BSON("_id" << 2));

    auto cloner = makeCollectionCloner();
    ASSERT_OK(cloner->run());

    ASSERT_EQUALS(2, _collectionStats->insertCount);
    ASSERT_TRUE(_collectionStats->commitCalled);

    auto stats = cloner->getStats();
    ASSERT_EQUALS(1u, stats.receivedBatches);
}

TEST_F(CollectionClonerTestResumable, BatchSizeStoredInConstructor) {
    auto batchSizeDefault = collectionClonerBatchSize;
    collectionClonerBatchSize = 3;
    ON_BLOCK_EXIT([&]() { collectionClonerBatchSize = batchSizeDefault; });

    // Set up data for preliminary stages.
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(2),
                         createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(_idIndexSpec)));

    // Set up documents to be returned from upstream node. It should take 3 batches to clone the
    // documents.
    _mockServer->insert(_nss, BSON("_id" << 1));
    _mockServer->insert(_nss, BSON("_id" << 2));
    _mockServer->insert(_nss, BSON("_id" << 3));
    _mockServer->insert(_nss, BSON("_id" << 4));
    _mockServer->insert(_nss, BSON("_id" << 5));
    _mockServer->insert(_nss, BSON("_id" << 6));
    _mockServer->insert(_nss, BSON("_id" << 7));

    auto cloner = makeCollectionCloner();
    ASSERT_OK(cloner->run());

    ASSERT_EQUALS(7, _collectionStats->insertCount);
    ASSERT_TRUE(_collectionStats->commitCalled);

    auto stats = cloner->getStats();
    ASSERT_EQUALS(3u, stats.receivedBatches);
}

TEST_F(CollectionClonerTestResumable, InsertDocumentsMultipleBatches) {
    // Set up data for preliminary stages
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(2),
                         createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(_idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss, BSON("_id" << 1));
    _mockServer->insert(_nss, BSON("_id" << 2));
    _mockServer->insert(_nss, BSON("_id" << 3));

    auto cloner = makeCollectionCloner();
    cloner->setBatchSize_forTest(2);

    ASSERT_OK(cloner->run());

    ASSERT_EQUALS(3, _collectionStats->insertCount);
    ASSERT_TRUE(_collectionStats->commitCalled);
    auto stats = cloner->getStats();
    ASSERT_EQUALS(2u, stats.receivedBatches);
}

TEST_F(CollectionClonerTestResumable, InsertDocumentsScheduleDBWorkFailed) {
    // Set up data for preliminary stages
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(2),
                         createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(_idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss, BSON("_id" << 1));
    _mockServer->insert(_nss, BSON("_id" << 2));
    _mockServer->insert(_nss, BSON("_id" << 3));

    auto cloner = makeCollectionCloner();
    // Stop before running the query to set up the failure.
    auto collClonerBeforeFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto timesEntered = collClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'CollectionCloner', stage: 'query', nss: '" + _nss.ns_forTest() + "'}"));

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonnerRunner", getGlobalServiceContext()->getService());
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

TEST_F(CollectionClonerTestResumable, InsertDocumentsCallbackCanceled) {
    // Set up data for preliminary stages
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(2),
                         createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(_idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss, BSON("_id" << 1));
    _mockServer->insert(_nss, BSON("_id" << 2));
    _mockServer->insert(_nss, BSON("_id" << 3));

    auto cloner = makeCollectionCloner();
    // Stop before running the query to set up the failure.
    auto collClonerBeforeFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto timesEntered = collClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'CollectionCloner', stage: 'query', nss: '" + _nss.ns_forTest() + "'}"));

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonnerRunner", getGlobalServiceContext()->getService());
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

TEST_F(CollectionClonerTestResumable, InsertDocumentsFailed) {
    // Set up data for preliminary stages
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(2),
                         createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(_idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss, BSON("_id" << 1));
    _mockServer->insert(_nss, BSON("_id" << 2));
    _mockServer->insert(_nss, BSON("_id" << 3));

    auto cloner = makeCollectionCloner();
    // Stop before running the query to set up the failure.
    auto collClonerBeforeFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto timesEntered = collClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'CollectionCloner', stage: 'query', nss: '" + _nss.ns_forTest() + "'}"));

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonnerRunner", getGlobalServiceContext()->getService());
        ASSERT_EQUALS(ErrorCodes::OperationFailed, cloner->run());
    });

    // Wait for the failpoint to be reached
    collClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);

    // Modify the loader so insert documents fails.
    ASSERT(_loader != nullptr);
    _loader->insertDocsFn = [](std::span<BSONObj> docs,
                               CollectionBulkLoader::ParseRecordIdAndDocFunc fn) {
        return Status(ErrorCodes::OperationFailed, "");
    };

    // Continue and finish. Final status is checked in the thread.
    collClonerBeforeFailPoint->setMode(FailPoint::off, 0);
    clonerThread.join();
}

TEST_F(CollectionClonerTestResumable, DoNotCreateIDIndexIfAutoIndexIdUsed) {
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
    _mockServer->insert(_nss, doc);

    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(1),
                         createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(_idIndexSpec)));

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

// We will retry our query without having yet obtained a resume token.
TEST_F(CollectionClonerTestResumable, ResumableQueryFailTransientlyBeforeFirstBatchRetrySuccess) {
    _mockServer->setCommandReply("replSetGetRBID", fromjson("{ok:1, rbid:1}"));

    // Set up data for preliminary stages
    auto idIndexSpec =
        BSON("v" << 1 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName);
    // The collection cloner pre-stage makes a remote call to collStats to store in-progress
    // metrics.
    _mockServer->setCommandReply("collStats", BSON("size" << 10));
    _mockServer->setCommandReply("count", createCountResponse(3));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss, BSON("_id" << 1));
    _mockServer->insert(_nss, BSON("_id" << 2));
    _mockServer->insert(_nss, BSON("_id" << 3));

    // Preliminary setup for failpoints.
    auto beforeStageFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto timesEnteredBeforeStage = beforeStageFailPoint->setMode(
        FailPoint::alwaysOn, 0, fromjson("{cloner: 'CollectionCloner', stage: 'query'}"));

    auto beforeRetryFailPoint = globalFailPointRegistry().find("hangBeforeRetryingClonerStage");
    auto timesEnteredBeforeRetry = beforeRetryFailPoint->setMode(
        FailPoint::alwaysOn, 0, fromjson("{cloner: 'CollectionCloner', stage: 'query'}"));

    auto cloner = makeCollectionCloner();

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonnerRunner", getGlobalServiceContext()->getService());
        ASSERT_OK(cloner->run());
    });

    // Wait until we get to the query stage.
    beforeStageFailPoint->waitForTimesEntered(timesEnteredBeforeStage + 1);

    // Bring the server down. We will fail right before our first batch.
    _mockServer->shutdown();

    // Let the cloner retry and wait until just before it.
    beforeStageFailPoint->setMode(FailPoint::off, 0);
    beforeRetryFailPoint->waitForTimesEntered(timesEnteredBeforeRetry);

    // Verify we haven't been able to receive anything yet.
    auto stats = cloner->getStats();
    ASSERT_EQUALS(0, stats.receivedBatches);

    // Bring the server back up.
    _mockServer->reboot();

    // Let the retry commence.
    beforeRetryFailPoint->setMode(FailPoint::off, 0);

    clonerThread.join();

    // Check that we've received all the data.
    ASSERT_EQUALS(3, _collectionStats->insertCount);
    ASSERT_TRUE(_collectionStats->commitCalled);
    stats = cloner->getStats();
    ASSERT_EQUALS(3u, stats.documentsCopied);
}

// We will resume our query using the resume token we stored after receiving the first batch.
TEST_F(CollectionClonerTestResumable, ResumableQueryFailTransientlyAfterFirstBatchRetrySuccess) {
    _mockServer->setCommandReply("replSetGetRBID", fromjson("{ok:1, rbid:1}"));

    // Set up data for preliminary stages
    auto idIndexSpec =
        BSON("v" << 1 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName);
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(5),
                         createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss, BSON("_id" << 1));
    _mockServer->insert(_nss, BSON("_id" << 2));
    _mockServer->insert(_nss, BSON("_id" << 3));
    _mockServer->insert(_nss, BSON("_id" << 4));
    _mockServer->insert(_nss, BSON("_id" << 5));

    // Preliminary setup for hanging failpoint.
    auto afterBatchFailpoint =
        globalFailPointRegistry().find("initialSyncHangCollectionClonerAfterHandlingBatchResponse");
    auto timesEnteredAfterBatch = afterBatchFailpoint->setMode(FailPoint::alwaysOn, 0);

    auto cloner = makeCollectionCloner();
    cloner->setBatchSize_forTest(2);

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonnerRunner", getGlobalServiceContext()->getService());
        ASSERT_OK(cloner->run());
    });

    // Wait for us to process the first batch.
    afterBatchFailpoint->waitForTimesEntered(timesEnteredAfterBatch + 1);

    // Verify we've only managed to store one batch.
    auto stats = cloner->getStats();
    ASSERT_EQUALS(1, stats.receivedBatches);

    // This will cause the next batch to fail once (transiently).
    auto failNextBatch = globalFailPointRegistry().find("mockCursorThrowErrorOnGetMore");
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'HostUnreachable'}"));

    // Let the query stage finish.
    afterBatchFailpoint->setMode(FailPoint::off, 0);
    clonerThread.join();

    // Since the CollectionMockStats class does not de-duplicate inserts, it is possible to insert
    // the same document more than once, thereby also increasing the insertCount more than once.
    // This means that here insertCount=5 is evidence that we correctly resumed our query where we
    // left off (2 inserts in) instead of retrying the whole query (that leads to insertCount=7).
    ASSERT_EQUALS(5, _collectionStats->insertCount);
    ASSERT_TRUE(_collectionStats->commitCalled);
    stats = cloner->getStats();
    ASSERT_EQUALS(5u, stats.documentsCopied);
}

TEST_F(CollectionClonerTestResumable, ResumableQueryNonRetriableError) {
    _mockServer->setCommandReply("replSetGetRBID", fromjson("{ok:1, rbid:1}"));

    // Set up data for preliminary stages
    auto idIndexSpec =
        BSON("v" << 1 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName);
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(3),
                         createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss, BSON("_id" << 1));
    _mockServer->insert(_nss, BSON("_id" << 2));
    _mockServer->insert(_nss, BSON("_id" << 3));

    auto cloner = makeCollectionCloner();
    auto beforeStageFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto timesEnteredBeforeStage = beforeStageFailPoint->setMode(
        FailPoint::alwaysOn, 0, fromjson("{cloner: 'CollectionCloner', stage: 'query'}"));

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonnerRunner", getGlobalServiceContext()->getService());
        auto status = cloner->run();
        ASSERT_EQUALS(ErrorCodes::UnknownError, status);
    });

    // Wait until we get to the query stage.
    beforeStageFailPoint->waitForTimesEntered(timesEnteredBeforeStage + 1);

    // Verify we've made no progress yet.
    auto stats = cloner->getStats();
    ASSERT_EQUALS(0, stats.receivedBatches);

    // This will cause the next batch to fail once, non-transiently.
    auto failNextBatch = globalFailPointRegistry().find("mockCursorThrowErrorOnGetMore");
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'UnknownError'}"));

    // Let us begin with the query stage.
    beforeStageFailPoint->setMode(FailPoint::off, 0);

    clonerThread.join();
}

TEST_F(CollectionClonerTestResumable,
       ResumableQueryFailNonTransientlyAfterProgressMadeCannotRetry) {
    _mockServer->setCommandReply("replSetGetRBID", fromjson("{ok:1, rbid:1}"));

    // Set up data for preliminary stages
    auto idIndexSpec =
        BSON("v" << 1 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName);
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(3),
                         createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss, BSON("_id" << 1));
    _mockServer->insert(_nss, BSON("_id" << 2));
    _mockServer->insert(_nss, BSON("_id" << 3));

    auto cloner = makeCollectionCloner();
    cloner->setBatchSize_forTest(2);
    auto afterBatchFailpoint =
        globalFailPointRegistry().find("initialSyncHangCollectionClonerAfterHandlingBatchResponse");
    auto timesEnteredAfterBatch = afterBatchFailpoint->setMode(FailPoint::alwaysOn, 0);

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonnerRunner", getGlobalServiceContext()->getService());
        auto status = cloner->run();
        ASSERT_EQUALS(ErrorCodes::UnknownError, status);
    });

    afterBatchFailpoint->waitForTimesEntered(timesEnteredAfterBatch + 1);

    // Verify we've only managed to store one batch.
    auto stats = cloner->getStats();
    ASSERT_EQUALS(1, stats.receivedBatches);

    // This will cause the next batch to fail once, non-transiently.
    auto failNextBatch = globalFailPointRegistry().find("mockCursorThrowErrorOnGetMore");
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'UnknownError'}"));

    // Allow the cloner to attempt (and fail) the next batch.
    afterBatchFailpoint->setMode(FailPoint::off, 0);

    clonerThread.join();
}

// We retry the query after a transient error and we immediately encounter a non-retriable one.
TEST_F(CollectionClonerTestResumable, ResumableQueryNonTransientErrorAtRetry) {
    _mockServer->setCommandReply("replSetGetRBID", fromjson("{ok:1, rbid:1}"));

    // Set up data for preliminary stages
    auto idIndexSpec =
        BSON("v" << 1 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName);
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(5),
                         createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss, BSON("_id" << 1));
    _mockServer->insert(_nss, BSON("_id" << 2));
    _mockServer->insert(_nss, BSON("_id" << 3));

    // Preliminary setup for hanging failpoints.
    auto afterBatchFailpoint =
        globalFailPointRegistry().find("initialSyncHangCollectionClonerAfterHandlingBatchResponse");
    auto timesEnteredAfterBatch = afterBatchFailpoint->setMode(FailPoint::alwaysOn, 0);

    auto beforeRetryFailPoint = globalFailPointRegistry().find("hangBeforeRetryingClonerStage");
    auto timesEnteredBeforeRetry = beforeRetryFailPoint->setMode(
        FailPoint::alwaysOn, 0, fromjson("{cloner: 'CollectionCloner', stage: 'query'}"));

    auto cloner = makeCollectionCloner();
    cloner->setBatchSize_forTest(2);

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonnerRunner", getGlobalServiceContext()->getService());
        auto status = cloner->run();
        ASSERT_EQUALS(ErrorCodes::UnknownError, status);
    });

    // Wait for us to process the first batch.
    afterBatchFailpoint->waitForTimesEntered(timesEnteredAfterBatch + 1);

    // Verify we've only managed to store one batch.
    auto stats = cloner->getStats();
    ASSERT_EQUALS(1, stats.receivedBatches);

    // This will cause the next batch to fail once (transiently).
    auto failNextBatch = globalFailPointRegistry().find("mockCursorThrowErrorOnGetMore");
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'HostUnreachable'}"));

    afterBatchFailpoint->setMode(FailPoint::off, 0);
    beforeRetryFailPoint->waitForTimesEntered(timesEnteredBeforeRetry + 1);

    // Follow-up the transient error with a non-retriable one.
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'UnknownError'}"));

    beforeRetryFailPoint->setMode(FailPoint::off, 0);
    clonerThread.join();

    // We only made it one batch in before failing.
    stats = cloner->getStats();
    ASSERT_EQUALS(1u, stats.receivedBatches);
}

// We retry the query after a transient error, we make a bit more progress and then we encounter
// a non-retriable one.
TEST_F(CollectionClonerTestResumable, ResumableQueryNonTransientErrorAfterPastRetry) {
    _mockServer->setCommandReply("replSetGetRBID", fromjson("{ok:1, rbid:1}"));

    // Set up data for preliminary stages
    auto idIndexSpec =
        BSON("v" << 1 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName);
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(5),
                         createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss, BSON("_id" << 1));
    _mockServer->insert(_nss, BSON("_id" << 2));
    _mockServer->insert(_nss, BSON("_id" << 3));
    _mockServer->insert(_nss, BSON("_id" << 4));
    _mockServer->insert(_nss, BSON("_id" << 5));

    // Preliminary setup for hanging failpoints.
    auto afterBatchFailpoint =
        globalFailPointRegistry().find("initialSyncHangCollectionClonerAfterHandlingBatchResponse");
    auto timesEnteredAfterBatch = afterBatchFailpoint->setMode(FailPoint::alwaysOn, 0);

    auto beforeRetryFailPoint = globalFailPointRegistry().find("hangBeforeRetryingClonerStage");
    auto timesEnteredBeforeRetry = beforeRetryFailPoint->setMode(
        FailPoint::alwaysOn, 0, fromjson("{cloner: 'CollectionCloner', stage: 'query'}"));

    auto cloner = makeCollectionCloner();
    cloner->setBatchSize_forTest(2);

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner", getGlobalServiceContext()->getService());
        auto status = cloner->run();
        ASSERT_EQUALS(ErrorCodes::UnknownError, status);
    });

    // Wait for us to process the first batch.
    afterBatchFailpoint->waitForTimesEntered(timesEnteredAfterBatch + 1);

    // Verify we've only managed to store one batch.
    auto stats = cloner->getStats();
    ASSERT_EQUALS(1, stats.receivedBatches);

    // This will cause the next batch to fail once (transiently).
    auto failNextBatch = globalFailPointRegistry().find("mockCursorThrowErrorOnGetMore");
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'HostUnreachable'}"));

    afterBatchFailpoint->setMode(FailPoint::off, 0);
    beforeRetryFailPoint->waitForTimesEntered(timesEnteredBeforeRetry + 1);

    // Do a quick failpoint dance so we clone one more batch before failing.
    timesEnteredAfterBatch = afterBatchFailpoint->setMode(FailPoint::alwaysOn, 0);
    beforeRetryFailPoint->setMode(FailPoint::off, 0);
    afterBatchFailpoint->waitForTimesEntered(timesEnteredAfterBatch + 1);

    // Follow-up the transient error with a non-retriable one.
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'UnknownError'}"));

    afterBatchFailpoint->setMode(FailPoint::off, 0);
    clonerThread.join();

    // We only made it one batch in before failing.
    stats = cloner->getStats();
    ASSERT_EQUALS(2u, stats.receivedBatches);
}

// We resume a query, receive some more data, then get a transient error again. The goal of this
// test is to make sure we don't forget to request the _next_ resume token when resuming a query.
TEST_F(CollectionClonerTestResumable, ResumableQueryTwoResumes) {

    /**
     * Test runs like so:
     *
     *      |___batch___| . |___batch___| |___batch___| . |batch|
     *                    |                             |
     *                 resume 1                      resume 2
     */

    _mockServer->setCommandReply("replSetGetRBID", fromjson("{ok:1, rbid:1}"));

    // Set up data for preliminary stages
    auto idIndexSpec =
        BSON("v" << 1 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName);
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(5),
                         createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(idIndexSpec)));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss, BSON("_id" << 1));
    _mockServer->insert(_nss, BSON("_id" << 2));
    _mockServer->insert(_nss, BSON("_id" << 3));
    _mockServer->insert(_nss, BSON("_id" << 4));
    _mockServer->insert(_nss, BSON("_id" << 5));
    _mockServer->insert(_nss, BSON("_id" << 6));
    _mockServer->insert(_nss, BSON("_id" << 7));

    // Preliminary setup for hanging failpoints.
    auto beforeBatchFailpoint = globalFailPointRegistry().find(
        "initialSyncHangCollectionClonerBeforeHandlingBatchResponse");
    auto afterBatchFailpoint =
        globalFailPointRegistry().find("initialSyncHangCollectionClonerAfterHandlingBatchResponse");
    auto timesEnteredAfterBatch = afterBatchFailpoint->setMode(FailPoint::alwaysOn, 0);

    auto beforeRetryFailPoint = globalFailPointRegistry().find("hangBeforeRetryingClonerStage");
    auto timesEnteredBeforeRetry = beforeRetryFailPoint->setMode(
        FailPoint::alwaysOn, 0, fromjson("{cloner: 'CollectionCloner', stage: 'query'}"));

    auto cloner = makeCollectionCloner();
    cloner->setBatchSize_forTest(2);

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonnerRunner", getGlobalServiceContext()->getService());
        ASSERT_OK(cloner->run());
    });

    // Wait for us to process the first batch.
    afterBatchFailpoint->waitForTimesEntered(timesEnteredAfterBatch + 1);

    // Verify we've only managed to store one batch.
    auto stats = cloner->getStats();
    ASSERT_EQUALS(1, stats.receivedBatches);

    // This will cause the next batch to fail once (transiently).
    auto failNextBatch = globalFailPointRegistry().find("mockCursorThrowErrorOnGetMore");
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'HostUnreachable'}"));

    afterBatchFailpoint->setMode(FailPoint::off, 0);
    // Ensure that the retry state is initially cleared.
    ASSERT_EQUALS(0, cloner->getRetryableOperationCount_forTest());
    beforeRetryFailPoint->waitForTimesEntered(timesEnteredBeforeRetry + 1);

    // Allow copying two more batches before the next error.
    // It is important that the resumes come after differing amounts of progress, so that we can
    // more easily distinguish error scenarios based on document count. (see end of test)
    failNextBatch->setMode(FailPoint::skip, 2, fromjson("{errorType: 'HostUnreachable'}"));

    // Do a failpoint dance so we can get to the next retry.
    auto timesEnteredBeforeBatch = beforeBatchFailpoint->setMode(FailPoint::alwaysOn, 0);
    timesEnteredAfterBatch = afterBatchFailpoint->setMode(FailPoint::alwaysOn, 0);
    beforeRetryFailPoint->setMode(FailPoint::off, 0);
    beforeBatchFailpoint->waitForTimesEntered(timesEnteredBeforeBatch + 1);
    // Ensure that the retry state records the last transient error retrial.
    ASSERT_EQUALS(1, cloner->getRetryableOperationCount_forTest());
    beforeBatchFailpoint->setMode(FailPoint::off, 0);

    afterBatchFailpoint->waitForTimesEntered(timesEnteredAfterBatch + 1);
    // Ensure that the retry state is cleared after a successful batch.
    ASSERT_EQUALS(0, cloner->getRetryableOperationCount_forTest());

    timesEnteredBeforeRetry = beforeRetryFailPoint->setMode(
        FailPoint::alwaysOn, 0, fromjson("{cloner: 'CollectionCloner', stage: 'query'}"));
    afterBatchFailpoint->setMode(FailPoint::off, 0);
    beforeRetryFailPoint->waitForTimesEntered(timesEnteredBeforeRetry + 1);
    timesEnteredBeforeBatch = beforeBatchFailpoint->setMode(FailPoint::alwaysOn, 0);

    // Allow the clone to finish.
    failNextBatch->setMode(FailPoint::off, 0);
    beforeRetryFailPoint->setMode(FailPoint::off, 0);

    beforeBatchFailpoint->waitForTimesEntered(timesEnteredBeforeBatch + 1);
    // Ensure that the retry state records the last transient error retrial.
    ASSERT_EQUALS(1, cloner->getRetryableOperationCount_forTest());
    beforeBatchFailpoint->setMode(FailPoint::off, 0);


    clonerThread.join();

    // Ensure that the retry state is cleared after a successful batch.
    ASSERT_EQUALS(0, cloner->getRetryableOperationCount_forTest());
    /**
     * Since the CollectionMockStats class does not de-duplicate inserts, it is possible to insert
     * the same document more than once, thereby also increasing the insertCount more than once.
     * We can therefore infer the resume history from the insertCount. In this test:
     * - insertCount = 7: all the resumes were correct and we got every doc exactly once
     *      - this is the correct result
     * - insertCount = 9: the first resume retried instead of resuming (second resume was correct)
     * - insertCount = 11: the second resume used the first resume token instead of the second one
     *      - we test that we avoid this result
     * - insertCount = 13: the second resume retried instead of resuming (first one was correct)
     */

    ASSERT_EQUALS(7, _collectionStats->insertCount);
    ASSERT_TRUE(_collectionStats->commitCalled);
    stats = cloner->getStats();
    ASSERT_EQUALS(7u, stats.documentsCopied);
}

// Test that the collection cloner uses a project to fetch documents from the upstream
// node.
TEST_F(CollectionClonerTestResumable, RecordIdsReplicatedFindProjects) {
    // Set up data for preliminary stages
    setMockServerReplies(BSON("size" << 10),
                         createCountResponse(2),
                         createCursorResponse(_nss.ns_forTest(), BSON_ARRAY(_idIndexSpec)));

    // Set up documents to be returned from upstream node. Unfortunately, because the
    // documents are not really stored in a storage engine on the mock server, they don't
    // have recordIds associated with them and therefore the CollectionCloner's projection to
    // return recordIds doesn't work.
    // However, the document projection still works. Therefore the returned documents
    // are of the form {d: <original document>}.
    _mockServer->insert(_nss, BSON("_id" << 1));
    _mockServer->insert(_nss, BSON("_id" << 2));
    _mockServer->insert(_nss, BSON("_id" << 3));

    auto collClonerBeforeFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto timesEntered = collClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'CollectionCloner', stage: 'query', nss: '" + _nss.ns_forTest() + "'}"));

    // Create a cloner that tries to replicate recordIds.
    CollectionOptions options;
    options.recordIdsReplicated = true;
    auto cloner = makeCollectionCloner(options);
    // Get multiple batches.
    cloner->setBatchSize_forTest(1);

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonnerRunner", getGlobalServiceContext()->getService());
        ASSERT_OK(cloner->run());
    });

    // Wait for the failpoint to be reached
    collClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);

    // Intercept the loader's attempt to insert documents.
    ASSERT(_loader != nullptr);
    _loader->insertDocsFn = [](std::span<BSONObj> docs,
                               CollectionBulkLoader::ParseRecordIdAndDocFunc fn) {
        for (auto&& doc : docs) {
            LOGV2(8613800, "Processing projected document", "doc"_attr = doc);
            ASSERT(doc.nFields() == 1);
            ASSERT(doc.hasField("d"));
        }

        // Assert that the correct parsing function was passed in, i.e. a function
        // that can parse documents of the form {r: <long recordId>, d: <original document>}.
        auto testDoc = BSON("r" << 10LL << "d" << BSON("_id" << 42));
        const auto& [rid, doc] = fn(testDoc);
        ASSERT_EQUALS(rid, RecordId(10));
        ASSERT_EQUALS(doc.woCompare(BSON("_id" << 42)), 0);

        return Status::OK();
    };

    collClonerBeforeFailPoint->setMode(FailPoint::off, 0);
    clonerThread.join();

    auto stats = cloner->getStats();
    ASSERT_EQUALS(3u, stats.receivedBatches);
}

class CollectionClonerMultitenancyTest : public CollectionClonerTestResumable {
public:
    CollectionClonerMultitenancyTest()
        : _nss(NamespaceString::createNamespaceString_forTest(TenantId(OID::gen()), kTestNs)) {}

protected:
    void setUp() final {
        RAIIServerParameterControllerForTest multitenancySupportController("multitenancySupport",
                                                                           true);
        CollectionClonerTestResumable::setUp();
    }

    std::unique_ptr<CollectionCloner> makeCollectionCloner(
        CollectionOptions options = CollectionOptions()) {
        options.uuid = _collUuid;
        _options = options;
        return std::make_unique<CollectionCloner>(_nss,
                                                  options,
                                                  getSharedData(),
                                                  _source,
                                                  _mockClient.get(),
                                                  &_storageInterface,
                                                  _dbWorkThreadPool.get());
    }

    NamespaceString _nss;
};

TEST_F(CollectionClonerMultitenancyTest, CollectionClonerMultitenancy) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    auto cloner = makeCollectionCloner();
    cloner->setBatchSize_forTest(1);

    int numOperations = 2;

    // Set up responses to be returned from upstream node.
    setMockServerReplies(
        BSON("size" << 10),
        createCountResponse(numOperations),
        createCursorResponse(
            _nss.ns_forTest(),
            BSON_ARRAY(_secondaryIndexSpecs[0] << _idIndexSpec << _secondaryIndexSpecs[1])));

    // Set up documents to be returned from upstream node.
    for (int i = 0; i < numOperations; ++i) {
        _mockServer->insert(_nss, BSON("_id" << i));
    }

    ASSERT_OK(cloner->run());

    // Check the count stage correctly updated the number of documents to copy.
    ASSERT_EQ(numOperations, cloner->getStats().documentToCopy);

    // Check the listIndexes stage correctly found the indexes to copy.
    ASSERT_BSONOBJ_EQ(_idIndexSpec, getIdIndexSpec(cloner.get()));
    ASSERT_EQ(_secondaryIndexSpecs.size(), getIndexSpecs(cloner.get()).size());
    ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[0], getIndexSpecs(cloner.get())[0]);
    ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[1], getIndexSpecs(cloner.get())[1]);
    // Account for the _id index as well
    ASSERT_EQ(_secondaryIndexSpecs.size() + 1, cloner->getStats().indexes);

    // Check the documents to copy were inserted correctly.
    ASSERT_EQUALS(numOperations, _collectionStats->insertCount);
    ASSERT_TRUE(_collectionStats->commitCalled);
    auto stats = cloner->getStats();
    ASSERT_EQUALS(2u, stats.receivedBatches);
    ASSERT_EQ(numOperations, cloner->getStats().documentsCopied);
}

}  // namespace repl
}  // namespace mongo
