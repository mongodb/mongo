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

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/tenant_cloner_test_fixture.h"
#include "mongo/db/repl/tenant_collection_cloner.h"
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

/**
 * Mock OpObserver that tracks storage events.
 */
class TenantCollectionClonerTestOpObserver final : public OpObserverNoop {
public:
    TenantCollectionClonerTestOpObserver(const NamespaceString& nss) : nssToCapture(nss) {}

    void onCreateCollection(OperationContext* opCtx,
                            const CollectionPtr& coll,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex,
                            const OplogSlot& createOpTime,
                            bool fromMigrate) final {
        if (collectionName == nssToCapture) {
            collCreated = true;
            collectionOptions = options;
            idIndexCreated = !idIndex.isEmpty();
        }
    }

    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const UUID& uuid,
                       BSONObj indexDoc,
                       bool fromMigrate) final {
        if (nss == nssToCapture) {
            secondaryIndexSpecs.emplace_back(indexDoc);
        }
    }

    void onInserts(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const UUID& uuid,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   bool fromMigrate) final {
        if (nss == nssToCapture) {
            numDocsInserted += std::distance(begin, end);
        }
    }

    const NamespaceString nssToCapture;
    bool collCreated = false;
    CollectionOptions collectionOptions;
    bool idIndexCreated = false;
    std::vector<BSONObj> secondaryIndexSpecs;
    size_t numDocsInserted{0};
};

class TenantCollectionClonerTest : public TenantClonerTestFixture {
public:
    TenantCollectionClonerTest() {}

protected:
    void setUp() override {
        TenantClonerTestFixture::setUp();

        _mockServer->assignCollectionUuid(_nss.ns(), _collUuid);
        _mockServer->setCommandReply("dbStats", StatusWith<BSONObj>(BSON("dataSize" << 1)));
        _mockServer->setCommandReply("collStats", BSON("size" << 1));

        _mockClient->setOperationTime(_operationTime);

        {
            auto serviceContext = getServiceContext();
            auto opCtx = cc().makeOperationContext();

            ReplicationCoordinator::set(
                serviceContext, std::make_unique<ReplicationCoordinatorMock>(serviceContext));

            repl::createOplog(opCtx.get());

            // Need real (non-mock) storage for the test.
            StorageInterface::set(serviceContext, std::make_unique<StorageInterfaceImpl>());

            // Register mock observer.
            auto opObserver = std::make_unique<TenantCollectionClonerTestOpObserver>(_nss);
            _opObserver = opObserver.get();
            auto opObserverRegistry =
                dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
            opObserverRegistry->addObserver(std::move(opObserver));

            // step up
            auto replCoord = ReplicationCoordinator::get(serviceContext);
            _term++;
            ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));
            ASSERT_OK(replCoord->updateTerm(opCtx.get(), _term));
            replCoord->setMyLastAppliedOpTimeAndWallTime(
                OpTimeAndWallTime(OpTime(Timestamp(1, 1), _term), Date_t()));
        }
    }
    std::unique_ptr<TenantCollectionCloner> makeCollectionCloner(
        CollectionOptions options = CollectionOptions(),
        TenantMigrationSharedData* sharedData = nullptr) {
        options.uuid = _collUuid;
        _options = options;
        return std::make_unique<TenantCollectionCloner>(
            _nss,
            options,
            sharedData ? sharedData : getSharedData(),
            _source,
            _mockClient.get(),
            repl::StorageInterface::get(getServiceContext()),
            _dbWorkThreadPool.get(),
            _tenantId);
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

    ProgressMeter& getProgressMeter(TenantCollectionCloner* cloner) {
        return cloner->_progressMeter;
    }

    std::vector<BSONObj> getIndexSpecs(TenantCollectionCloner* cloner) {
        return cloner->_readyIndexSpecs;
    }

    BSONObj& getIdIndexSpec(TenantCollectionCloner* cloner) {
        return cloner->_idIndexSpec;
    }

    BSONObj& getLastDocId(TenantCollectionCloner* cloner) {
        return cloner->_lastDocId;
    }

    long long _term = 0;
    const TenantCollectionClonerTestOpObserver* _opObserver =
        nullptr;  // Owned by service context opObserverRegistry
    CollectionOptions _options;

    UUID _collUuid = UUID::gen();
    BSONObj _idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                    << "_id_");

    std::vector<BSONObj> _secondaryIndexSpecs{BSON("v" << 1 << "key" << BSON("a" << 1) << "name"
                                                       << "a_1"),
                                              BSON("v" << 1 << "key" << BSON("b" << 1) << "name"
                                                       << "b_1")};
    const NamespaceString _nss = {_tenantId + "_testDb", "testcoll"};
};

TEST_F(TenantCollectionClonerTest, CountStage) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("count");
    _mockServer->setCommandReply("count", createCountResponse(100));
    ASSERT_OK(cloner->run());
    ASSERT_EQ(100, getProgressMeter(cloner.get()).total());
}

// On a negative count, the CollectionCloner should use a zero count.
TEST_F(TenantCollectionClonerTest, CountStageNegativeCount) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("count");
    _mockServer->setCommandReply("count", createCountResponse(-100));
    ASSERT_OK(cloner->run());
    ASSERT_EQ(0, getProgressMeter(cloner.get()).total());
}

TEST_F(TenantCollectionClonerTest, CollectionClonerPassesThroughNonRetriableErrorFromCountCommand) {
    auto cloner = makeCollectionCloner();
    _mockServer->setCommandReply("count", Status(ErrorCodes::OperationFailed, ""));
    ASSERT_EQUALS(ErrorCodes::OperationFailed, cloner->run());
}

TEST_F(TenantCollectionClonerTest,
       CollectionClonerReturnsNoSuchKeyOnMissingDocumentCountFieldName) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("count");
    _mockServer->setCommandReply("count", BSON("ok" << 1));
    auto status = cloner->run();
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status);
}

TEST_F(TenantCollectionClonerTest, ListIndexesReturnedNoIndexesShouldFail) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("listIndexes");
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), BSONArray()));
    _mockServer->setCommandReply("find", createFindResponse());

    ASSERT_EQUALS(ErrorCodes::IllegalOperation, cloner->run());
    ASSERT(getIdIndexSpec(cloner.get()).isEmpty());
    ASSERT(getIndexSpecs(cloner.get()).empty());
    ASSERT_EQ(0, cloner->getStats().indexes);
}

TEST_F(TenantCollectionClonerTest, ListIndexesHasResults) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("listIndexes");
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply(
        "listIndexes",
        createCursorResponse(
            _nss.ns(),
            BSON_ARRAY(_secondaryIndexSpecs[0] << _idIndexSpec << _secondaryIndexSpecs[1])));
    _mockServer->setCommandReply("find", createFindResponse());
    ASSERT_OK(cloner->run());
    ASSERT_BSONOBJ_EQ(_idIndexSpec, getIdIndexSpec(cloner.get()));
    ASSERT_EQ(2, getIndexSpecs(cloner.get()).size());
    ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[0], getIndexSpecs(cloner.get())[0]);
    ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[1], getIndexSpecs(cloner.get())[1]);
    ASSERT_EQ(3, cloner->getStats().indexes);
}

TEST_F(TenantCollectionClonerTest, ListIndexesNonRetriableError) {
    auto cloner = makeCollectionCloner();
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes", Status(ErrorCodes::OperationFailed, ""));
    ASSERT_EQUALS(ErrorCodes::OperationFailed, cloner->run());
}

TEST_F(TenantCollectionClonerTest, ListIndexesRemoteUnreachableBeforeMajorityFind) {
    auto cloner = makeCollectionCloner();
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));

    auto clonerOperationTimeFP =
        globalFailPointRegistry().find("tenantCollectionClonerHangAfterGettingOperationTime");
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

TEST_F(TenantCollectionClonerTest, ListIndexesRecordsCorrectOperationTime) {
    auto cloner = makeCollectionCloner();
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());

    auto clonerOperationTimeFP =
        globalFailPointRegistry().find("tenantCollectionClonerHangAfterGettingOperationTime");
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

TEST_F(TenantCollectionClonerTest, BeginCollection) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("createCollection");
    _mockServer->setCommandReply("count", createCountResponse(1));
    BSONArrayBuilder indexSpecs;
    indexSpecs.append(_idIndexSpec);
    for (const auto& secondaryIndexSpec : _secondaryIndexSpecs) {
        indexSpecs.append(secondaryIndexSpec);
    }
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), indexSpecs.arr()));
    _mockServer->setCommandReply("find", createFindResponse());

    ASSERT_EQUALS(Status::OK(), cloner->run());

    ASSERT_EQUALS(_nss.ns(), _opObserver->nssToCapture.ns());
    ASSERT_TRUE(_opObserver->collCreated);
    ASSERT_BSONOBJ_EQ(_options.toBSON(), _opObserver->collectionOptions.toBSON());

    ASSERT_TRUE(_opObserver->idIndexCreated);

    ASSERT_EQUALS(_secondaryIndexSpecs.size(), _opObserver->secondaryIndexSpecs.size());
    for (std::vector<BSONObj>::size_type i = 0; i < _secondaryIndexSpecs.size(); ++i) {
        ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[i], _opObserver->secondaryIndexSpecs[i]);
    }
}

TEST_F(TenantCollectionClonerTest, BeginCollectionFailed) {
    auto createCollectionFp =
        globalFailPointRegistry().find("hangAndFailAfterCreateCollectionReservesOpTime");
    auto initialTimesEntered =
        createCollectionFp->setMode(FailPoint::alwaysOn, 0, BSON("nss" << _nss.toString()));

    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("createCollection");
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        auto status = cloner->run();
        ASSERT_EQUALS(51267, status.code());
    });

    // Wait for the failpoint to be reached
    createCollectionFp->waitForTimesEntered(initialTimesEntered + 1);
    createCollectionFp->setMode(FailPoint::off);

    clonerThread.join();

    ASSERT_EQUALS(_nss.ns(), _opObserver->nssToCapture.ns());
    ASSERT_FALSE(_opObserver->collCreated);
}

TEST_F(TenantCollectionClonerTest, InsertDocumentsSingleBatch) {
    // Set up data for preliminary stages
    _mockServer->setCommandReply("count", createCountResponse(2));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));

    auto cloner = makeCollectionCloner();
    ASSERT_OK(cloner->run());

    ASSERT_EQUALS(_nss.ns(), _opObserver->nssToCapture.ns());
    ASSERT_EQUALS(2, _opObserver->numDocsInserted);

    auto stats = cloner->getStats();
    ASSERT_EQUALS(2u, stats.documentsCopied);
    ASSERT_EQUALS(1u, stats.receivedBatches);
}

TEST_F(TenantCollectionClonerTest, BatchSizeStoredInConstructor) {
    auto batchSizeDefault = collectionClonerBatchSize;
    collectionClonerBatchSize = 3;
    ON_BLOCK_EXIT([&]() { collectionClonerBatchSize = batchSizeDefault; });

    // Set up data for preliminary stages.
    _mockServer->setCommandReply("count", createCountResponse(7));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());

    // Set up documents to be returned from upstream node. It should take 3 batches to clone the
    // documents.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));
    _mockServer->insert(_nss.ns(), BSON("_id" << 3));
    _mockServer->insert(_nss.ns(), BSON("_id" << 4));
    _mockServer->insert(_nss.ns(), BSON("_id" << 5));
    _mockServer->insert(_nss.ns(), BSON("_id" << 6));
    _mockServer->insert(_nss.ns(), BSON("_id" << 7));

    auto cloner = makeCollectionCloner();
    ASSERT_OK(cloner->run());

    ASSERT_EQUALS(_nss.ns(), _opObserver->nssToCapture.ns());
    ASSERT_EQUALS(7, _opObserver->numDocsInserted);

    auto stats = cloner->getStats();
    ASSERT_EQUALS(7u, stats.documentsCopied);
    ASSERT_EQUALS(3u, stats.receivedBatches);
}

TEST_F(TenantCollectionClonerTest, InsertDocumentsMultipleBatches) {
    // Set up data for preliminary stages
    _mockServer->setCommandReply("count", createCountResponse(5));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));
    _mockServer->insert(_nss.ns(), BSON("_id" << 3));
    _mockServer->insert(_nss.ns(), BSON("_id" << 4));
    _mockServer->insert(_nss.ns(), BSON("_id" << 5));

    auto cloner = makeCollectionCloner();
    cloner->setBatchSize_forTest(2);
    ASSERT_OK(cloner->run());

    ASSERT_EQUALS(_nss.ns(), _opObserver->nssToCapture.ns());
    ASSERT_EQUALS(5, _opObserver->numDocsInserted);

    auto stats = cloner->getStats();
    ASSERT_EQUALS(5u, stats.documentsCopied);
    ASSERT_EQUALS(3u, stats.receivedBatches);
}

TEST_F(TenantCollectionClonerTest, InsertDocumentsScheduleDBWorkFailed) {
    // Set up data for preliminary stages
    _mockServer->setCommandReply("count", createCountResponse(3));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());

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
        fromjson("{cloner: 'TenantCollectionCloner', stage: 'query', nss: '" + _nss.ns() + "'}"));

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

TEST_F(TenantCollectionClonerTest, InsertDocumentsCallbackCanceled) {
    // Set up data for preliminary stages
    _mockServer->setCommandReply("count", createCountResponse(3));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());

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
        fromjson("{cloner: 'TenantCollectionCloner', stage: 'query', nss: '" + _nss.ns() + "'}"));

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

TEST_F(TenantCollectionClonerTest, InsertDocumentsFailed) {
    // Set up data for preliminary stages
    _mockServer->setCommandReply("count", createCountResponse(3));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));
    _mockServer->insert(_nss.ns(), BSON("_id" << 3));

    auto cloner = makeCollectionCloner();

    // Enable failpoint to make collection inserts to fail.
    FailPointEnableBlock fp("failCollectionInserts", BSON("collectionNS" << _nss.toString()));

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_EQUALS(ErrorCodes::FailPointEnabled, cloner->run());
    });

    clonerThread.join();
}

TEST_F(TenantCollectionClonerTest, QueryFailure) {
    // Set up data for preliminary stages
    auto idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                << "_id_");
    _mockServer->setCommandReply("count", createCountResponse(3));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());

    auto beforeStageFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto timesEnteredBeforeStage = beforeStageFailPoint->setMode(
        FailPoint::alwaysOn, 0, fromjson("{cloner: 'TenantCollectionCloner', stage: 'query'}"));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));
    _mockServer->insert(_nss.ns(), BSON("_id" << 3));

    auto cloner = makeCollectionCloner();

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_NOT_OK(cloner->run());
    });

    // Wait until we get to the query stage.
    beforeStageFailPoint->waitForTimesEntered(timesEnteredBeforeStage + 1);

    // Bring the server down.
    _mockServer->shutdown();

    // Let us begin with the query stage.
    beforeStageFailPoint->setMode(FailPoint::off, 0);

    clonerThread.join();
}

// On NamespaceNotFound, the TenantCollectionCloner should exit without doing anything.
TEST_F(TenantCollectionClonerTest, CountStageNamespaceNotFound) {
    auto cloner = makeCollectionCloner();
    // The tenant collection cloner pre-stage makes a remote call to collStats to store in-progress
    // metrics.
    _mockServer->setCommandReply("collStats", BSON("size" << 10000));
    _mockServer->setCommandReply("count", Status(ErrorCodes::NamespaceNotFound, "NoSuchUuid"));
    ASSERT_OK(cloner->run());
}

// NamespaceNotFound is treated the same as no indexes.
TEST_F(TenantCollectionClonerTest, ListIndexesReturnedNamespaceNotFound) {
    auto cloner = makeCollectionCloner();
    _mockServer->setCommandReply("collStats", BSON("size" << 10));
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes",
                                 Status(ErrorCodes::NamespaceNotFound, "No indexes here."));

    // We expect the collection to *not* be created.
    ASSERT_OK(cloner->run());
    ASSERT_FALSE(_opObserver->collCreated);
    ASSERT(getIdIndexSpec(cloner.get()).isEmpty());
    ASSERT(getIndexSpecs(cloner.get()).empty());
    ASSERT_EQ(0, cloner->getStats().indexes);
}

TEST_F(TenantCollectionClonerTest, QueryStageNamespaceNotFoundOnFirstBatch) {
    // Set up data for preliminary stages.
    _mockServer->setCommandReply("count", createCountResponse(2));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());  // majority read after listIndexes

    // Set up before-stage failpoint.
    auto beforeStageFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto timesEnteredBeforeStage = beforeStageFailPoint->setMode(
        FailPoint::alwaysOn, 0, fromjson("{cloner: 'TenantCollectionCloner', stage: 'query'}"));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));
    _mockServer->insert(_nss.ns(), BSON("_id" << 3));

    auto cloner = makeCollectionCloner();

    // Run the cloner in a separate thread. The cloner should detect the drop at the beginning
    // of the query stage and exit normally, without copying over any documents.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_OK(cloner->run());
        ASSERT_EQ(0, cloner->getStats().documentsCopied);
    });

    // Wait until we get to the query stage.
    beforeStageFailPoint->waitForTimesEntered(timesEnteredBeforeStage + 1);

    // Verify we've made no progress yet.
    auto stats = cloner->getStats();
    ASSERT_EQUALS(0, stats.receivedBatches);

    // Despite the name, this will also trigger on the initial batch.
    auto failNextBatch = globalFailPointRegistry().find("mockCursorThrowErrorOnGetMore");
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'NamespaceNotFound'}"));

    // Proceed with the query stage.
    beforeStageFailPoint->setMode(FailPoint::off, 0);
    clonerThread.join();
    ASSERT_EQUALS(0, _opObserver->numDocsInserted);
}

TEST_F(TenantCollectionClonerTest, QueryStageNamespaceNotFoundOnSubsequentBatch) {
    // Set up data for preliminary stages.
    _mockServer->setCommandReply("count", createCountResponse(2));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());  // majority read after listIndexes

    // Set up after-first-batch failpoint.
    auto afterBatchFailpoint = globalFailPointRegistry().find(
        "tenantMigrationHangCollectionClonerAfterHandlingBatchResponse");
    auto timesEnteredAfterBatch = afterBatchFailpoint->setMode(FailPoint::alwaysOn, 0);

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));
    _mockServer->insert(_nss.ns(), BSON("_id" << 3));
    _mockServer->insert(_nss.ns(), BSON("_id" << 4));
    _mockServer->insert(_nss.ns(), BSON("_id" << 5));

    auto cloner = makeCollectionCloner();
    cloner->setBatchSize_forTest(2);

    // Run the cloner in a separate thread. The cloner should detect the drop on the second query
    // batch and exit normally.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_OK(cloner->run());
        // We cloned two documents before we registered the drop.
        ASSERT_EQ(2, cloner->getStats().documentsCopied);
    });

    // Wait until we get to the query stage.
    afterBatchFailpoint->waitForTimesEntered(timesEnteredAfterBatch + 1);

    // Verify we've processed exactly one batch.
    auto stats = cloner->getStats();
    ASSERT_EQUALS(1, stats.receivedBatches);

    // Trigger drop before second batch.
    auto failNextBatch = globalFailPointRegistry().find("mockCursorThrowErrorOnGetMore");
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'NamespaceNotFound'}"));

    // Proceed with the query stage.
    afterBatchFailpoint->setMode(FailPoint::off, 0);
    clonerThread.join();
    ASSERT_EQUALS(2, _opObserver->numDocsInserted);
}

// We receive a QueryPlanKilled error, then a NamespaceNotFound error, indicating that the
// collection no longer exists in the database.
TEST_F(TenantCollectionClonerTest, QueryPlanKilledCheckIfDonorCollectionIsEmptyStage) {
    // Set up data for preliminary stages.
    _mockServer->setCommandReply("count", createCountResponse(3));

    // Set up failpoints.
    auto beforeStageFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto timesEnteredBeforeStage = beforeStageFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'TenantCollectionCloner', stage: 'checkIfDonorCollectionIsEmpty'}"));
    auto beforeRetryFailPoint = globalFailPointRegistry().find("hangBeforeRetryingClonerStage");
    auto timesEnteredBeforeRetry = beforeRetryFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'TenantCollectionCloner', stage: 'checkIfDonorCollectionIsEmpty'}"));

    auto cloner = makeCollectionCloner();
    cloner->setBatchSize_forTest(2);

    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_OK(cloner->run());
        ASSERT_EQ(0, cloner->getStats().documentsCopied);
    });

    // Wait until we get to the 'checkIfDonorCollectionIsEmpty' stage.
    beforeStageFailPoint->waitForTimesEntered(timesEnteredBeforeStage + 1);

    // Despite the name, this will also trigger on the initial batch.
    auto failNextBatch = globalFailPointRegistry().find("mockCursorThrowErrorOnGetMore");
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'QueryPlanKilled'}"));

    // Proceed with the 'checkIfDonorCollectionIsEmpty' stage.
    beforeStageFailPoint->setMode(FailPoint::off, 0);
    beforeRetryFailPoint->waitForTimesEntered(timesEnteredBeforeRetry + 1);

    // Follow-up the QueryPlanKilled error with a NamespaceNotFound.
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'NamespaceNotFound'}"));

    beforeRetryFailPoint->setMode(FailPoint::off, 0);
    clonerThread.join();

    ASSERT_EQUALS(0, _opObserver->numDocsInserted);
}

// We receive a QueryPlanKilled error, then a NamespaceNotFound error, indicating that the
// collection no longer exists in the database.
TEST_F(TenantCollectionClonerTest, QueryPlanKilledThenNamespaceNotFoundFirstBatch) {
    // Set up data for preliminary stages.
    _mockServer->setCommandReply("count", createCountResponse(3));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());  // majority read after listIndexes

    // Set up failpoints.
    auto beforeStageFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto timesEnteredBeforeStage = beforeStageFailPoint->setMode(
        FailPoint::alwaysOn, 0, fromjson("{cloner: 'TenantCollectionCloner', stage: 'query'}"));
    auto beforeRetryFailPoint = globalFailPointRegistry().find("hangBeforeRetryingClonerStage");
    auto timesEnteredBeforeRetry = beforeRetryFailPoint->setMode(
        FailPoint::alwaysOn, 0, fromjson("{cloner: 'TenantCollectionCloner', stage: 'query'}"));

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));
    _mockServer->insert(_nss.ns(), BSON("_id" << 3));

    auto cloner = makeCollectionCloner();
    cloner->setBatchSize_forTest(2);

    // Run the cloner in a separate thread. The cloner should detect the drop at the beginning
    // of the query stage and exit normally, without copying over any documents.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_OK(cloner->run());
        ASSERT_EQ(0, cloner->getStats().documentsCopied);
    });

    // Wait until we get to the query stage.
    beforeStageFailPoint->waitForTimesEntered(timesEnteredBeforeStage + 1);

    // Verify we've made no progress yet.
    auto stats = cloner->getStats();
    ASSERT_EQUALS(0, stats.receivedBatches);

    // Despite the name, this will also trigger on the initial batch.
    auto failNextBatch = globalFailPointRegistry().find("mockCursorThrowErrorOnGetMore");
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'QueryPlanKilled'}"));

    // Proceed with the query stage.
    beforeStageFailPoint->setMode(FailPoint::off, 0);
    beforeRetryFailPoint->waitForTimesEntered(timesEnteredBeforeRetry + 1);

    // Follow-up the QueryPlanKilled error with a NamespaceNotFound.
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'NamespaceNotFound'}"));

    beforeRetryFailPoint->setMode(FailPoint::off, 0);
    clonerThread.join();

    ASSERT_EQUALS(0, _opObserver->numDocsInserted);
}

// We receive a QueryPlanKilled error, then a NamespaceNotFound error, indicating that the
// collection no longer exists in the database.
TEST_F(TenantCollectionClonerTest, QueryPlanKilledThenNamespaceNotFoundSubsequentBatch) {
    // Set up data for preliminary stages.
    _mockServer->setCommandReply("count", createCountResponse(3));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());  // majority read after listIndexes

    // Set up failpoints.
    auto beforeRetryFailPoint = globalFailPointRegistry().find("hangBeforeRetryingClonerStage");
    auto timesEnteredBeforeRetry = beforeRetryFailPoint->setMode(
        FailPoint::alwaysOn, 0, fromjson("{cloner: 'TenantCollectionCloner', stage: 'query'}"));
    auto afterBatchFailpoint = globalFailPointRegistry().find(
        "tenantMigrationHangCollectionClonerAfterHandlingBatchResponse");
    auto timesEnteredAfterBatch = afterBatchFailpoint->setMode(FailPoint::alwaysOn, 0);

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));
    _mockServer->insert(_nss.ns(), BSON("_id" << 3));

    auto cloner = makeCollectionCloner();
    cloner->setBatchSize_forTest(2);

    // Run the cloner in a separate thread. The cloner should detect the drop during the query
    // stage. It will have copied over some documents before that.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_OK(cloner->run());
        ASSERT_EQ(2, cloner->getStats().documentsCopied);
    });

    // Wait for us to process the first batch.
    afterBatchFailpoint->waitForTimesEntered(timesEnteredAfterBatch + 1);

    // Verify we've only managed to store one batch.
    auto stats = cloner->getStats();
    ASSERT_EQUALS(1, stats.receivedBatches);

    // This will cause the next batch to fail once (transiently).
    auto failNextBatch = globalFailPointRegistry().find("mockCursorThrowErrorOnGetMore");
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'QueryPlanKilled'}"));

    afterBatchFailpoint->setMode(FailPoint::off, 0);
    beforeRetryFailPoint->waitForTimesEntered(timesEnteredBeforeRetry + 1);

    // Follow-up the previous error with NamespaceNotFound.
    failNextBatch->setMode(FailPoint::nTimes, 1, fromjson("{errorType: 'NamespaceNotFound'}"));
    beforeRetryFailPoint->setMode(FailPoint::off, 0);

    afterBatchFailpoint->waitForTimesEntered(timesEnteredAfterBatch + 1);
    afterBatchFailpoint->setMode(FailPoint::off, 0);
    clonerThread.join();
}

TEST_F(TenantCollectionClonerTest, ResumeFromEmptyCollectionMissingAllSecondaryIndexes) {
    TenantMigrationSharedData resumingSharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeCollectionCloner(CollectionOptions(), &resumingSharedData);

    // Simulate that the collection already exists with no data and no secondary index.
    ASSERT_OK(createCollection(_nss, _options));
    ASSERT_TRUE(_opObserver->collCreated);
    ASSERT_EQUALS(0, _opObserver->secondaryIndexSpecs.size());

    cloner->setStopAfterStage_forTest("createCollection");
    _mockServer->setCommandReply("count", createCountResponse(1));
    BSONArrayBuilder indexSpecs;
    indexSpecs.append(_idIndexSpec);
    for (const auto& secondaryIndexSpec : _secondaryIndexSpecs) {
        indexSpecs.append(secondaryIndexSpec);
    }
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), indexSpecs.arr()));
    _mockServer->setCommandReply("find", createFindResponse());

    ASSERT_EQUALS(Status::OK(), cloner->run());

    // We should create the missing secondary indexes on the empty collection.
    ASSERT_EQ(2, getIndexSpecs(cloner.get()).size());
    ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[0], getIndexSpecs(cloner.get())[0]);
    ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[1], getIndexSpecs(cloner.get())[1]);

    ASSERT_EQUALS(_secondaryIndexSpecs.size(), _opObserver->secondaryIndexSpecs.size());
    for (std::vector<BSONObj>::size_type i = 0; i < _secondaryIndexSpecs.size(); ++i) {
        ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[i], _opObserver->secondaryIndexSpecs[i]);
    }
}

TEST_F(TenantCollectionClonerTest, ResumeFromEmptyCollectionMissingSomeSecondaryIndexes) {
    TenantMigrationSharedData resumingSharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeCollectionCloner(CollectionOptions(), &resumingSharedData);

    // Simulate that the collection already exists with no data and some secondary indexes.
    ASSERT_OK(createCollection(_nss, _options));
    ASSERT_OK(createIndexesOnEmptyCollection(_nss,
                                             {_secondaryIndexSpecs[0],
                                              // An index that has been dropped on the donor.
                                              BSON("v" << 1 << "key" << BSON("c" << 1) << "name"
                                                       << "c_1")}));
    ASSERT_TRUE(_opObserver->collCreated);
    ASSERT_EQUALS(2, _opObserver->secondaryIndexSpecs.size());

    cloner->setStopAfterStage_forTest("createCollection");
    _mockServer->setCommandReply("count", createCountResponse(1));
    BSONArrayBuilder indexSpecs;
    indexSpecs.append(_idIndexSpec);
    for (const auto& secondaryIndexSpec : _secondaryIndexSpecs) {
        indexSpecs.append(secondaryIndexSpec);
    }
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), indexSpecs.arr()));
    _mockServer->setCommandReply("find", createFindResponse());

    ASSERT_EQUALS(Status::OK(), cloner->run());

    // We should create the other missing secondary indexes on the empty collection.
    ASSERT_EQ(1, getIndexSpecs(cloner.get()).size());
    ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[1], getIndexSpecs(cloner.get())[0]);

    ASSERT_EQUALS(3, _opObserver->secondaryIndexSpecs.size());
    ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[1], _opObserver->secondaryIndexSpecs[2]);
}

TEST_F(TenantCollectionClonerTest, ResumeFromEmptyCollectionMissingNoSecondaryIndexes) {
    TenantMigrationSharedData resumingSharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeCollectionCloner(CollectionOptions(), &resumingSharedData);

    // Simulate that the collection already exists with no data and all matching secondary indexes.
    ASSERT_OK(createCollection(_nss, _options));
    ASSERT_OK(createIndexesOnEmptyCollection(_nss, _secondaryIndexSpecs));
    ASSERT_TRUE(_opObserver->collCreated);
    ASSERT_EQUALS(2, _opObserver->secondaryIndexSpecs.size());

    cloner->setStopAfterStage_forTest("createCollection");
    _mockServer->setCommandReply("count", createCountResponse(1));
    BSONArrayBuilder indexSpecs;
    indexSpecs.append(_idIndexSpec);
    for (const auto& secondaryIndexSpec : _secondaryIndexSpecs) {
        indexSpecs.append(secondaryIndexSpec);
    }
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), indexSpecs.arr()));
    _mockServer->setCommandReply("find", createFindResponse());

    ASSERT_EQUALS(Status::OK(), cloner->run());

    // We shouldn't need to create any secondary index.
    ASSERT_EQ(0, getIndexSpecs(cloner.get()).size());
}

TEST_F(TenantCollectionClonerTest, ResumeFromNonEmptyCollection) {
    TenantMigrationSharedData resumingSharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeCollectionCloner(CollectionOptions(), &resumingSharedData);

    // Simulate that the collection already exists with some data.
    ASSERT_OK(createCollection(_nss, _options));
    ASSERT_OK(createIndexesOnEmptyCollection(_nss, _secondaryIndexSpecs));
    {
        auto storage = StorageInterface::get(serviceContext);
        auto opCtx = cc().makeOperationContext();
        ASSERT_OK(storage->insertDocument(opCtx.get(), _nss, {BSON("_id" << 1)}, 0));
    }
    ASSERT_TRUE(_opObserver->collCreated);
    ASSERT_EQUALS(2, _opObserver->secondaryIndexSpecs.size());

    cloner->setStopAfterStage_forTest("createCollection");
    _mockServer->setCommandReply("count", createCountResponse(1));
    BSONArrayBuilder indexSpecs;
    indexSpecs.append(_idIndexSpec);
    for (const auto& secondaryIndexSpec : _secondaryIndexSpecs) {
        indexSpecs.append(secondaryIndexSpec);
    }
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), indexSpecs.arr()));
    _mockServer->setCommandReply("find", createFindResponse());

    ASSERT_EQUALS(Status::OK(), cloner->run());

    // We shouldn't need to create any secondary index.
    ASSERT_EQ(0, getIndexSpecs(cloner.get()).size());
    // Test that we have updated the stats.
    ASSERT_EQ(1, cloner->getStats().documentsCopied);
}

TEST_F(TenantCollectionClonerTest, ResumeFromRecreatedCollection) {
    TenantMigrationSharedData resumingSharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeCollectionCloner(CollectionOptions(), &resumingSharedData);

    // Simulate that the namespace already exists under a different uuid.
    CollectionOptions oldOptions;
    oldOptions.uuid = UUID::gen();  // A different uuid.
    ASSERT_OK(createCollection(_nss, oldOptions));

    _mockServer->setCommandReply("count", createCountResponse(3));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());  // majority read after listIndexes

    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));
    _mockServer->insert(_nss.ns(), BSON("_id" << 3));

    ASSERT_EQUALS(Status::OK(), cloner->run());

    // Test that the cloner correctly skips cloning this collection as it must have been dropped and
    // re-created on the donor. And the drop and the re-create will be covered by the oplog
    // application phase.
    ASSERT_EQUALS(_nss.ns(), _opObserver->nssToCapture.ns());
    ASSERT_EQUALS(0, _opObserver->numDocsInserted);
    auto stats = cloner->getStats();
    ASSERT_EQUALS(0, stats.documentsCopied);
    ASSERT_EQUALS(0, stats.receivedBatches);
}

TEST_F(TenantCollectionClonerTest, ResumeFromRenamedCollection) {
    TenantMigrationSharedData resumingSharedData(&_clock, _migrationId, ResumePhase::kDataSync);
    auto cloner = makeCollectionCloner(CollectionOptions(), &resumingSharedData);

    // Simulate that the collection already exists under a different name with no index and no data.
    const NamespaceString oldNss = {_nss.db(), "testcoll_old"};
    ASSERT_OK(createCollection(oldNss, _options));

    _mockServer->setCommandReply("count", createCountResponse(1));
    BSONArrayBuilder indexSpecs;
    indexSpecs.append(_idIndexSpec);
    for (const auto& secondaryIndexSpec : _secondaryIndexSpecs) {
        indexSpecs.append(secondaryIndexSpec);
    }
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), indexSpecs.arr()));
    _mockServer->setCommandReply("find", createFindResponse());

    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));

    auto opObserver = std::make_unique<TenantCollectionClonerTestOpObserver>(oldNss);
    auto oldNssOpObserver = opObserver.get();
    auto opObserverRegistry = dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
    opObserverRegistry->addObserver(std::move(opObserver));

    ASSERT_OK(cloner->run());

    // We should re-create the secondary indexes using the old ns.
    ASSERT_EQ(_secondaryIndexSpecs.size(), getIndexSpecs(cloner.get()).size());
    ASSERT_EQUALS(_secondaryIndexSpecs.size(), oldNssOpObserver->secondaryIndexSpecs.size());
    for (std::vector<BSONObj>::size_type i = 0; i < _secondaryIndexSpecs.size(); ++i) {
        ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[i], oldNssOpObserver->secondaryIndexSpecs[i]);
    }

    // We should insert documents into the old ns.
    ASSERT_EQ(2, oldNssOpObserver->numDocsInserted);
    ASSERT_EQ(2, cloner->getStats().documentsCopied);
}

// This test checks the condition where documents are inserted on the donor after the listIndexes
// call is made on the recipient, but before the query is done.  In that case we should skip the
// the query and never see the documents.
TEST_F(TenantCollectionClonerTest, NoDocumentsIfInsertedAfterListIndexes) {
    // Set up data for preliminary stages
    _mockServer->setCommandReply("count", createCountResponse(0));
    _mockServer->setCommandReply("listIndexes",
                                 createCursorResponse(_nss.ns(), BSON_ARRAY(_idIndexSpec)));
    _mockServer->setCommandReply("find", createFindResponse());

    auto collClonerAfterFailPoint = globalFailPointRegistry().find("hangAfterClonerStage");
    auto timesEntered = collClonerAfterFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'TenantCollectionCloner', stage: 'listIndexes', nss: '" + _nss.ns() +
                 "'}"));
    auto cloner = makeCollectionCloner();
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_OK(cloner->run());
        ASSERT_EQ(0, cloner->getStats().documentsCopied);
    });

    collClonerAfterFailPoint->waitForTimesEntered(timesEntered + 1);
    // Set up documents to be returned from upstream node.
    _mockServer->insert(_nss.ns(), BSON("_id" << 1));
    _mockServer->insert(_nss.ns(), BSON("_id" << 2));

    // Continue and finish. Final status is checked in the thread.
    collClonerAfterFailPoint->setMode(FailPoint::off, 0);
    clonerThread.join();

    ASSERT_EQUALS(0, _opObserver->numDocsInserted);

    auto stats = cloner->getStats();
    ASSERT_EQUALS(0u, stats.documentsCopied);
    ASSERT_EQUALS(0u, stats.receivedBatches);
}

}  // namespace repl
}  // namespace mongo
