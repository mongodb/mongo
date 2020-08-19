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

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/repl/cloner_test_fixture.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
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

class TenantCollectionClonerTest : public ClonerTestFixture {
public:
    TenantCollectionClonerTest() {}

protected:
    void setUp() override {
        ClonerTestFixture::setUp();
        _sharedData = std::make_unique<TenantMigrationSharedData>(kInitialRollbackId, &_clock);
        _standardCreateCollectionFn = [this](OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const CollectionOptions& options) -> Status {
            this->_collCreated = true;
            return Status::OK();
        };
        _storageInterface.createCollFn = _standardCreateCollectionFn;
        _standardCreateIndexesOnEmptyCollectionFn =
            [this](OperationContext* opCtx,
                   const NamespaceString& nss,
                   const std::vector<BSONObj>& secondaryIndexSpecs) -> Status {
            this->_numSecondaryIndexesCreated += secondaryIndexSpecs.size();
            return Status::OK();
        };
        _storageInterface.createIndexesOnEmptyCollFn = _standardCreateIndexesOnEmptyCollectionFn;
        _storageInterface.insertDocumentsFn = [this](OperationContext* opCtx,
                                                     const NamespaceStringOrUUID& nsOrUUID,
                                                     const std::vector<InsertStatement>& ops) {
            this->_numDocsInserted += ops.size();
            return Status::OK();
        };

        _mockServer->assignCollectionUuid(_nss.ns(), _collUuid);
        _mockServer->setCommandReply("replSetGetRBID",
                                     BSON("ok" << 1 << "rbid" << getSharedData()->getRollBackId()));
        _mockClient->setOperationTime(_operationTime);
    }
    std::unique_ptr<TenantCollectionCloner> makeCollectionCloner(
        CollectionOptions options = CollectionOptions()) {
        options.uuid = _collUuid;
        _options = options;
        return std::make_unique<TenantCollectionCloner>(_nss,
                                                        options,
                                                        getSharedData(),
                                                        _source,
                                                        _mockClient.get(),
                                                        &_storageInterface,
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

    TenantMigrationSharedData* getSharedData() {
        return checked_cast<TenantMigrationSharedData*>(_sharedData.get());
    }

    StorageInterfaceMock::CreateCollectionFn _standardCreateCollectionFn;
    StorageInterfaceMock::CreateIndexesOnEmptyCollectionFn
        _standardCreateIndexesOnEmptyCollectionFn;
    bool _collCreated = false;
    size_t _numSecondaryIndexesCreated{0};
    size_t _numDocsInserted{0};
    CollectionOptions _options;

    UUID _collUuid = UUID::gen();
    BSONObj _idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                    << "_id_");

    std::vector<BSONObj> _secondaryIndexSpecs{BSON("v" << 1 << "key" << BSON("a" << 1) << "name"
                                                       << "a_1"),
                                              BSON("v" << 1 << "key" << BSON("b" << 1) << "name"
                                                       << "b_1")};
    static std::string _tenantId;
    static NamespaceString _nss;
    static Timestamp _operationTime;
};

/* static */
std::string TenantCollectionClonerTest::_tenantId = "tenant42";
NamespaceString TenantCollectionClonerTest::_nss = {_tenantId + "_testDb", "testcoll"};
Timestamp TenantCollectionClonerTest::_operationTime = Timestamp(12345, 42);


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

TEST_F(TenantCollectionClonerTest, ListIndexesReturnedNoIndexes) {
    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("listIndexes");
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), BSONArray()));
    _mockServer->setCommandReply("find", createFindResponse());
    ASSERT_OK(cloner->run());
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
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), BSONArray()));

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
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), BSONArray()));
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
    NamespaceString collNss;
    CollectionOptions collOptions;
    BSONObj collIdIndexSpec;
    std::vector<BSONObj> collSecondaryIndexSpecs;

    _storageInterface.createCollFn =
        [&](OperationContext* opCtx, const NamespaceString& nss, const CollectionOptions& options) {
            collNss = nss;
            collOptions = options;
            return _standardCreateCollectionFn(opCtx, nss, options);
        };

    _storageInterface.createIndexesOnEmptyCollFn =
        [&](OperationContext* opCtx,
            const NamespaceString& nss,
            const std::vector<BSONObj>& secondaryIndexSpecs) {
            collSecondaryIndexSpecs = secondaryIndexSpecs;
            return _standardCreateIndexesOnEmptyCollectionFn(opCtx, nss, secondaryIndexSpecs);
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
    _mockServer->setCommandReply("find", createFindResponse());

    ASSERT_EQUALS(Status::OK(), cloner->run());

    ASSERT_EQUALS(_nss.ns(), collNss.ns());
    ASSERT_BSONOBJ_EQ(_options.toBSON(), collOptions.toBSON());
    ASSERT_EQUALS(_secondaryIndexSpecs.size(), collSecondaryIndexSpecs.size());
    for (std::vector<BSONObj>::size_type i = 0; i < _secondaryIndexSpecs.size(); ++i) {
        ASSERT_BSONOBJ_EQ(_secondaryIndexSpecs[i], collSecondaryIndexSpecs[i]);
    }
}

TEST_F(TenantCollectionClonerTest, BeginCollectionFailed) {
    _storageInterface.createCollFn =
        [&](OperationContext* opCtx, const NamespaceString& nss, const CollectionOptions& options) {
            return Status(ErrorCodes::OperationFailed, "");
        };

    auto cloner = makeCollectionCloner();
    cloner->setStopAfterStage_forTest("createCollection");
    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), BSONArray()));
    _mockServer->setCommandReply("find", createFindResponse());
    ASSERT_EQUALS(ErrorCodes::OperationFailed, cloner->run());
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

    ASSERT_EQUALS(2, _numDocsInserted);

    auto stats = cloner->getStats();
    ASSERT_EQUALS(1u, stats.receivedBatches);
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

    ASSERT_EQUALS(5, _numDocsInserted);

    auto stats = cloner->getStats();
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
    // Stop before running the query to set up the failure.
    auto collClonerBeforeFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto timesEntered = collClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'TenantCollectionCloner', stage: 'query', nss: '" + _nss.ns() + "'}"));

    // Run the cloner in a separate thread.
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_EQUALS(ErrorCodes::OperationFailed, cloner->run());
    });

    // Wait for the failpoint to be reached
    collClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);

    // Make the insertDocuments fail.
    _storageInterface.insertDocumentsFn = [this](OperationContext* opCtx,
                                                 const NamespaceStringOrUUID& nsOrUUID,
                                                 const std::vector<InsertStatement>& ops) {
        return Status(ErrorCodes::OperationFailed, "");
    };


    // Continue and finish. Final status is checked in the thread.
    collClonerBeforeFailPoint->setMode(FailPoint::off, 0);
    clonerThread.join();
}

TEST_F(TenantCollectionClonerTest, DoNotCreateIDIndexIfAutoIndexIdUsed) {
    NamespaceString collNss;
    CollectionOptions collOptions;
    // We initialize collIndexSpecs with fake information to ensure it is overwritten by an empty
    // vector.
    std::vector<BSONObj> collIndexSpecs{BSON("fakeindexkeys" << 1)};
    _storageInterface.createCollFn = [&, this](OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const CollectionOptions& options) -> Status {
        collNss = nss;
        collOptions = options;
        return _standardCreateCollectionFn(opCtx, nss, options);
    };

    _storageInterface.createIndexesOnEmptyCollFn =
        [&](OperationContext* opCtx,
            const NamespaceString& nss,
            const std::vector<BSONObj>& secondaryIndexSpecs) {
            collIndexSpecs = secondaryIndexSpecs;
            return _standardCreateIndexesOnEmptyCollectionFn(opCtx, nss, secondaryIndexSpecs);
        };

    const BSONObj doc = BSON("_id" << 1);
    _mockServer->insert(_nss.ns(), doc);

    _mockServer->setCommandReply("count", createCountResponse(1));
    _mockServer->setCommandReply("listIndexes", createCursorResponse(_nss.ns(), BSONArray()));
    _mockServer->setCommandReply("find", createFindResponse());

    CollectionOptions options;
    options.autoIndexId = CollectionOptions::NO;
    auto cloner = makeCollectionCloner(options);
    ASSERT_OK(cloner->run());
    ASSERT_EQUALS(1, _numDocsInserted);
    ASSERT_TRUE(_collCreated);
    ASSERT_EQ(collOptions.autoIndexId, CollectionOptions::NO);
    ASSERT_EQ(0UL, collIndexSpecs.size());
    ASSERT_EQ(collNss, _nss);
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

}  // namespace repl
}  // namespace mongo
