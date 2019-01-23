/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <memory>
#include <vector>

#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/base_cloner_test_fixture.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/task_executor_proxy.h"
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

class FailableMockDBClientConnection : public MockDBClientConnection {
public:
    FailableMockDBClientConnection(MockRemoteDBServer* remote, executor::NetworkInterfaceMock* net)
        : MockDBClientConnection(remote), _net(net) {}

    virtual ~FailableMockDBClientConnection() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _paused = false;
        _cond.notify_all();
        _cond.wait(lk, [this] { return !_resuming; });
    }

    Status connect(const HostAndPort& host, StringData applicationName) override {
        if (!_failureForConnect.isOK())
            return _failureForConnect;
        return MockDBClientConnection::connect(host, applicationName);
    }

    using MockDBClientConnection::query;  // This avoids warnings from -Woverloaded-virtual
    unsigned long long query(stdx::function<void(mongo::DBClientCursorBatchIterator&)> f,
                             const NamespaceStringOrUUID& nsOrUuid,
                             mongo::Query query,
                             const mongo::BSONObj* fieldsToReturn,
                             int queryOptions,
                             int batchSize) override {
        ON_BLOCK_EXIT([this]() {
            {
                stdx::lock_guard<stdx::mutex> lk(_mutex);
                _queryCount++;
            }
            _cond.notify_all();
        });
        {
            stdx::unique_lock<stdx::mutex> lk(_mutex);
            _waiting = _paused;
            _cond.notify_all();
            while (_paused) {
                lk.unlock();
                _net->waitForWork();
                lk.lock();
            }
            _waiting = false;
        }
        auto result = MockDBClientConnection::query(
            f, nsOrUuid, query, fieldsToReturn, queryOptions, batchSize);
        uassertStatusOK(_failureForQuery);
        return result;
    }

    void setFailureForConnect(Status failure) {
        _failureForConnect = failure;
    }

    void setFailureForQuery(Status failure) {
        _failureForQuery = failure;
    }

    void pause() {
        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            _paused = true;
        }
        _cond.notify_all();
    }
    void resume() {
        {
            stdx::unique_lock<stdx::mutex> lk(_mutex);
            _resuming = true;
            _resume(&lk);
            _resuming = false;
            _cond.notify_all();
        }
    }

    // Waits for the next query after pause() is called to start.
    void waitForPausedQuery() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _cond.wait(lk, [this] { return _waiting; });
    }

    // Resumes, then waits for the next query to run after resume() is called to complete.
    void resumeAndWaitForResumedQuery() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _resuming = true;
        _resume(&lk);
        _cond.notify_all();  // This is to wake up the paused thread.
        _cond.wait(lk, [this] { return _resumedQueryCount != _queryCount; });
        _resuming = false;
        _cond.notify_all();  // This potentially wakes up the destructor.
    }

private:
    executor::NetworkInterfaceMock* _net;
    stdx::mutex _mutex;
    stdx::condition_variable _cond;
    bool _paused = false;
    bool _waiting = false;
    bool _resuming = false;
    int _queryCount = 0;
    int _resumedQueryCount = 0;
    Status _failureForConnect = Status::OK();
    Status _failureForQuery = Status::OK();

    void _resume(stdx::unique_lock<stdx::mutex>* lk) {
        invariant(lk->owns_lock());
        _paused = false;
        _resumedQueryCount = _queryCount;
        while (_waiting) {
            lk->unlock();
            _net->signalWorkAvailable();
            mongo::sleepmillis(10);
            lk->lock();
        }
    }
};

// RAII class to pause the client; since tests are very exception-heavy this prevents them
// from hanging on failure.
class MockClientPauser {
    MONGO_DISALLOW_COPYING(MockClientPauser);

public:
    MockClientPauser(FailableMockDBClientConnection* client) : _client(client) {
        _client->pause();
    };
    ~MockClientPauser() {
        resume();
    }
    void resume() {
        if (_client)
            _client->resume();
        _client = nullptr;
    }

    void resumeAndWaitForResumedQuery() {
        if (_client)
            _client->resumeAndWaitForResumedQuery();
        _client = nullptr;
    }

private:
    FailableMockDBClientConnection* _client;
};

class CollectionClonerTest : public BaseClonerTest {
public:
    BaseCloner* getCloner() const override;

protected:
    auto setStatusCallback() {
        return [this](const Status& s) { setStatus(s); };
    }

    void setUp() override;
    void tearDown() override;

    virtual CollectionOptions getCollectionOptions() const {
        CollectionOptions options;
        options.uuid = UUID::gen();
        return options;
    }

    virtual const NamespaceString& getStartupNss() const {
        return nss;
    };


    std::vector<BSONObj> makeSecondaryIndexSpecs(const NamespaceString& nss);

    // A simple arbitrary value to use as the default batch size.
    const int defaultBatchSize = 1024;

    CollectionOptions options;
    std::unique_ptr<CollectionCloner> collectionCloner;
    std::shared_ptr<CollectionMockStats> collectionStats;  // Used by the _loader.
    CollectionBulkLoaderMock* _loader;                     // Owned by CollectionCloner.
    bool _clientCreated = false;
    FailableMockDBClientConnection* _client;  // owned by the CollectionCloner once created.
    std::unique_ptr<MockRemoteDBServer> _server;
};

void CollectionClonerTest::setUp() {
    BaseClonerTest::setUp();
    options = getCollectionOptions();
    collectionCloner.reset(nullptr);
    collectionCloner = std::make_unique<CollectionCloner>(&getExecutor(),
                                                          dbWorkThreadPool.get(),
                                                          target,
                                                          getStartupNss(),
                                                          options,
                                                          setStatusCallback(),
                                                          storageInterface.get(),
                                                          defaultBatchSize);
    collectionStats = std::make_shared<CollectionMockStats>();
    storageInterface->createCollectionForBulkFn =
        [this](const NamespaceString& nss,
               const CollectionOptions& options,
               const BSONObj idIndexSpec,
               const std::vector<BSONObj>& nonIdIndexSpecs)
        -> StatusWith<std::unique_ptr<CollectionBulkLoaderMock>> {
            auto localLoader = std::make_unique<CollectionBulkLoaderMock>(collectionStats);
            Status result = localLoader->init(nonIdIndexSpecs);
            if (!result.isOK())
                return result;

            _loader = localLoader.get();

            return std::move(localLoader);
        };
    _server = std::make_unique<MockRemoteDBServer>(target.toString());
    _server->assignCollectionUuid(nss.ns(), *options.uuid);
    _client = new FailableMockDBClientConnection(_server.get(), getNet());
    collectionCloner->setCreateClientFn_forTest([this]() {
        _clientCreated = true;
        return std::unique_ptr<DBClientConnection>(_client);
    });
}

// Return index specs to use for secondary indexes.
std::vector<BSONObj> CollectionClonerTest::makeSecondaryIndexSpecs(const NamespaceString& nss) {
    return {BSON("v" << 1 << "key" << BSON("a" << 1) << "name"
                     << "a_1"
                     << "ns"
                     << nss.ns()),
            BSON("v" << 1 << "key" << BSON("b" << 1) << "name"
                     << "b_1"
                     << "ns"
                     << nss.ns())};
}

void CollectionClonerTest::tearDown() {
    BaseClonerTest::tearDown();
    // Executor may still invoke collection cloner's callback before shutting down.
    collectionCloner.reset();
    if (!_clientCreated)
        delete _client;
    _clientCreated = false;
    _server.reset();
    options = {};
}

BaseCloner* CollectionClonerTest::getCloner() const {
    return collectionCloner.get();
}


TEST_F(CollectionClonerTest, InvalidConstruction) {
    executor::TaskExecutor& executor = getExecutor();
    auto pool = dbWorkThreadPool.get();

    const auto& cb = [](const Status&) { FAIL("should not reach here"); };

    // Null executor -- error from Fetcher, not CollectionCloner.
    {
        StorageInterface* si = storageInterface.get();
        ASSERT_THROWS_CODE_AND_WHAT(
            CollectionCloner(nullptr, pool, target, nss, options, cb, si, defaultBatchSize),
            AssertionException,
            ErrorCodes::BadValue,
            "task executor cannot be null");
    }

    // Null storage interface
    ASSERT_THROWS_CODE_AND_WHAT(
        CollectionCloner(&executor, pool, target, nss, options, cb, nullptr, defaultBatchSize),
        AssertionException,
        ErrorCodes::BadValue,
        "storage interface cannot be null");

    // Invalid namespace.
    {
        NamespaceString badNss("db.");
        StorageInterface* si = storageInterface.get();
        ASSERT_THROWS_CODE_AND_WHAT(
            CollectionCloner(&executor, pool, target, badNss, options, cb, si, defaultBatchSize),
            AssertionException,
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
            CollectionCloner(
                &executor, pool, target, nss, invalidOptions, cb, si, defaultBatchSize),
            AssertionException,
            ErrorCodes::BadValue,
            "'storageEngine.storageEngine1' has to be an embedded document.");
    }

    // UUID must be present.
    {
        CollectionOptions invalidOptions = options;
        invalidOptions.uuid = boost::none;
        StorageInterface* si = storageInterface.get();
        ASSERT_THROWS_CODE_AND_WHAT(
            CollectionCloner(
                &executor, pool, target, nss, invalidOptions, cb, si, defaultBatchSize),
            AssertionException,
            50953,
            "Missing collection UUID in CollectionCloner, collection name: db.coll");
    }

    // Callback function cannot be null.
    {
        CollectionCloner::CallbackFn nullCb;
        StorageInterface* si = storageInterface.get();
        ASSERT_THROWS_CODE_AND_WHAT(
            CollectionCloner(&executor, pool, target, nss, options, nullCb, si, defaultBatchSize),
            AssertionException,
            ErrorCodes::BadValue,
            "callback function cannot be null");
    }

    // Batch size must be non-negative.
    {
        StorageInterface* si = storageInterface.get();
        constexpr int kInvalidBatchSize = -1;
        ASSERT_THROWS_CODE_AND_WHAT(
            CollectionCloner(&executor, pool, target, nss, options, cb, si, kInvalidBatchSize),
            AssertionException,
            50954,
            "collectionClonerBatchSize must be non-negative.");
    }
}

TEST_F(CollectionClonerTest, ClonerLifeCycle) {
    testLifeCycle();
}

TEST_F(CollectionClonerTest, FirstRemoteCommand) {
    ASSERT_OK(collectionCloner->startup());

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
    ASSERT_TRUE(net->hasReadyRequests());
    NetworkOperationIterator noi = net->getNextReadyRequest();
    auto&& noiRequest = noi->getRequest();
    ASSERT_EQUALS(nss.db().toString(), noiRequest.dbname);
    ASSERT_EQUALS("count", std::string(noiRequest.cmdObj.firstElementFieldName()));
    auto requestUUID = uassertStatusOK(UUID::parse(noiRequest.cmdObj.firstElement()));
    ASSERT_EQUALS(options.uuid.get(), requestUUID);

    ASSERT_FALSE(net->hasReadyRequests());
    ASSERT_TRUE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, CollectionClonerSetsDocumentCountInStatsFromCountCommandResult) {
    ASSERT_OK(collectionCloner->startup());

    ASSERT_EQUALS(0U, collectionCloner->getStats().documentToCopy);
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(100));
    }
    getExecutor().shutdown();
    collectionCloner->join();
    ASSERT_EQUALS(100U, collectionCloner->getStats().documentToCopy);
}

TEST_F(CollectionClonerTest, CollectionClonerPassesThroughNonRetriableErrorFromCountCommand) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(ErrorCodes::OperationFailed, "");
    }
    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus());
}

TEST_F(CollectionClonerTest, CollectionClonerPassesThroughCommandStatusErrorFromCountCommand) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(BSON("ok" << 0 << "errmsg"
                                         << "count error"
                                         << "code"
                                         << int(ErrorCodes::OperationFailed)));
    }
    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus());
    ASSERT_STRING_CONTAINS(getStatus().reason(), "count error");
}

TEST_F(CollectionClonerTest, CollectionClonerResendsCountCommandOnRetriableError) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(ErrorCodes::HostNotFound, "");
        processNetworkResponse(ErrorCodes::NetworkTimeout, "");
        processNetworkResponse(createCountResponse(100));
    }
    getExecutor().shutdown();
    collectionCloner->join();
    ASSERT_EQUALS(100U, collectionCloner->getStats().documentToCopy);
}

TEST_F(CollectionClonerTest, CollectionClonerReturnsLastRetriableErrorOnExceedingCountAttempts) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(ErrorCodes::HostNotFound, "");
        processNetworkResponse(ErrorCodes::NetworkTimeout, "");
        processNetworkResponse(ErrorCodes::NotMaster, "");
    }
    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::NotMaster, getStatus());
}

TEST_F(CollectionClonerTest, CollectionClonerReturnsNoSuchKeyOnMissingDocumentCountFieldName) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(BSON("ok" << 1));
    }
    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, getStatus());
}

TEST_F(CollectionClonerTest, CollectionClonerReturnsBadValueOnNegativeDocumentCount) {
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(-1));
    }
    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::BadValue, getStatus());
}

class TaskExecutorWithFailureInScheduleRemoteCommand : public unittest::TaskExecutorProxy {
public:
    using ShouldFailRequestFn = stdx::function<bool(const executor::RemoteCommandRequest&)>;

    TaskExecutorWithFailureInScheduleRemoteCommand(executor::TaskExecutor* executor,
                                                   ShouldFailRequestFn shouldFailRequest)
        : unittest::TaskExecutorProxy(executor), _shouldFailRequest(shouldFailRequest) {}

    StatusWith<CallbackHandle> scheduleRemoteCommand(const executor::RemoteCommandRequest& request,
                                                     const RemoteCommandCallbackFn& cb,
                                                     const BatonHandle& baton = nullptr) override {
        if (_shouldFailRequest(request)) {
            return Status(ErrorCodes::OperationFailed, "failed to schedule remote command");
        }
        return getExecutor()->scheduleRemoteCommand(request, cb, baton);
    }

private:
    ShouldFailRequestFn _shouldFailRequest;
};

TEST_F(CollectionClonerTest,
       CollectionClonerReturnsScheduleErrorOnFailingToScheduleListIndexesCommand) {
    TaskExecutorWithFailureInScheduleRemoteCommand _executorProxy(
        &getExecutor(), [](const executor::RemoteCommandRequest& request) {
            return str::equals("listIndexes", request.cmdObj.firstElementFieldName());
        });

    collectionCloner = stdx::make_unique<CollectionCloner>(&_executorProxy,
                                                           dbWorkThreadPool.get(),
                                                           target,
                                                           nss,
                                                           options,
                                                           setStatusCallback(),
                                                           storageInterface.get(),
                                                           defaultBatchSize);

    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(100));
    }
    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus());
}

class CollectionClonerNoAutoIndexTest : public CollectionClonerTest {
protected:
    CollectionOptions getCollectionOptions() const override {
        CollectionOptions options = CollectionClonerTest::getCollectionOptions();
        options.autoIndexId = CollectionOptions::NO;
        return options;
    }
};

TEST_F(CollectionClonerNoAutoIndexTest, DoNotCreateIDIndexIfAutoIndexIdUsed) {
    NamespaceString collNss;
    CollectionOptions collOptions;
    std::vector<BSONObj> collIndexSpecs{BSON("fakeindexkeys" << 1)};  // init with one doc.
    storageInterface->createCollectionForBulkFn = [&,
                                                   this](const NamespaceString& theNss,
                                                         const CollectionOptions& theOptions,
                                                         const BSONObj idIndexSpec,
                                                         const std::vector<BSONObj>& theIndexSpecs)
        -> StatusWith<std::unique_ptr<CollectionBulkLoader>> {
            auto loader = std::make_unique<CollectionBulkLoaderMock>(collectionStats);
            collNss = theNss;
            collOptions = theOptions;
            collIndexSpecs = theIndexSpecs;
            const auto status = loader->init(theIndexSpecs);
            if (!status.isOK())
                return status;
            return std::move(loader);
        };

    const BSONObj doc = BSON("_id" << 1);
    _server->insert(nss.ns(), doc);
    // Pause the CollectionCloner before executing the query so we can verify events which are
    // supposed to happen before the query.
    MockClientPauser pauser(_client);
    ASSERT_OK(collectionCloner->startup());
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(1));
        processNetworkResponse(createListIndexesResponse(0, BSONArray()));
    }
    ASSERT_TRUE(collectionCloner->isActive());

    _client->waitForPausedQuery();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats->initCalled);

    pauser.resume();
    collectionCloner->join();
    ASSERT_EQUALS(1, collectionStats->insertCount);
    ASSERT_TRUE(collectionStats->commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
    ASSERT_EQ(collOptions.autoIndexId, CollectionOptions::NO);
    ASSERT_EQ(0UL, collIndexSpecs.size());
    ASSERT_EQ(collNss, nss);
}

// A collection may have no indexes. The cloner will produce a warning but
// will still proceed with cloning.
TEST_F(CollectionClonerTest, ListIndexesReturnedNoIndexes) {
    ASSERT_OK(collectionCloner->startup());

    // Using a non-zero cursor to ensure that
    // the cloner stops the fetcher from retrieving more results.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
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
    ASSERT_OK(collectionCloner->startup());

    bool collectionCreated = false;
    bool writesAreReplicatedOnOpCtx = false;
    NamespaceString collNss;
    storageInterface->createCollFn = [&collNss, &collectionCreated, &writesAreReplicatedOnOpCtx](
        OperationContext* opCtx, const NamespaceString& nss, const CollectionOptions& options) {
        writesAreReplicatedOnOpCtx = opCtx->writesAreReplicated();
        collectionCreated = true;
        collNss = nss;
        return Status::OK();
    };
    // Using a non-zero cursor to ensure that
    // the cloner stops the fetcher from retrieving more results.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(ErrorCodes::NamespaceNotFound, "The collection doesn't exist.");
    }

    collectionCloner->join();
    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
    ASSERT_TRUE(collectionCreated);
    ASSERT_FALSE(writesAreReplicatedOnOpCtx);
    ASSERT_EQ(collNss, nss);
}


TEST_F(CollectionClonerTest, CollectionClonerResendsListIndexesCommandOnRetriableError) {
    ASSERT_OK(collectionCloner->startup());

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);

    // First request sent by CollectionCloner. CollectionCollection sends listIndexes request
    // irrespective of collection size in a successful count response.
    assertRemoteCommandNameEquals("count", net->scheduleSuccessfulResponse(createCountResponse(0)));
    net->runReadyNetworkOperations();

    // Respond to first listIndexes request with a retriable error.
    assertRemoteCommandNameEquals("listIndexes",
                                  net->scheduleErrorResponse(Status(ErrorCodes::HostNotFound, "")));
    net->runReadyNetworkOperations();
    ASSERT_TRUE(collectionCloner->isActive());

    // Confirm that CollectionCloner resends the listIndexes request.
    auto noi = net->getNextReadyRequest();
    assertRemoteCommandNameEquals("listIndexes", noi->getRequest());
    net->blackHole(noi);
}

TEST_F(CollectionClonerTest,
       ListIndexesReturnedNamespaceNotFoundAndCreateCollectionCallbackCanceled) {
    ASSERT_OK(collectionCloner->startup());

    // Replace scheduleDbWork function to schedule the create collection task with an injected error
    // status.
    auto exec = &getExecutor();
    collectionCloner->setScheduleDbWorkFn_forTest([exec](
        executor::TaskExecutor::CallbackFn workFn) {
        auto wrappedTask = [workFn = std::move(workFn)](
            const executor::TaskExecutor::CallbackArgs& cbd) {
            workFn(executor::TaskExecutor::CallbackArgs(
                cbd.executor, cbd.myHandle, Status(ErrorCodes::CallbackCanceled, ""), cbd.opCtx));
        };
        return exec->scheduleWork(std::move(wrappedTask));
    });

    bool collectionCreated = false;
    storageInterface->createCollFn = [&collectionCreated](
        OperationContext*, const NamespaceString& nss, const CollectionOptions&) {
        collectionCreated = true;
        return Status::OK();
    };

    // Using a non-zero cursor to ensure that
    // the cloner stops the fetcher from retrieving more results.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(ErrorCodes::NamespaceNotFound, "The collection doesn't exist.");
    }

    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
    ASSERT_FALSE(collectionCreated);
}

TEST_F(CollectionClonerTest, BeginCollectionScheduleDbWorkFailed) {
    ASSERT_OK(collectionCloner->startup());

    // Replace scheduleDbWork function so that cloner will fail to schedule DB work after
    // getting index specs.
    collectionCloner->setScheduleDbWorkFn_forTest(
        [](const executor::TaskExecutor::CallbackFn& workFn) {
            return StatusWith<executor::TaskExecutor::CallbackHandle>(ErrorCodes::UnknownError, "");
        });

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    ASSERT_EQUALS(ErrorCodes::UnknownError, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, BeginCollectionCallbackCanceled) {
    ASSERT_OK(collectionCloner->startup());

    // Replace scheduleDbWork function so that the callback runs with a cancelled status.
    auto&& executor = getExecutor();
    collectionCloner->setScheduleDbWorkFn_forTest(
        [&](const executor::TaskExecutor::CallbackFn& workFn) {
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
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, BeginCollectionFailed) {
    ASSERT_OK(collectionCloner->startup());

    storageInterface->createCollectionForBulkFn = [&](const NamespaceString& theNss,
                                                      const CollectionOptions& theOptions,
                                                      const BSONObj idIndexSpec,
                                                      const std::vector<BSONObj>& theIndexSpecs) {
        return Status(ErrorCodes::OperationFailed, "");
    };

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();

    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, BeginCollection) {
    // Pause the CollectionCloner before executing the query so we can verify state after
    // the listIndexes call.
    MockClientPauser pauser(_client);
    ASSERT_OK(collectionCloner->startup());

    auto stats = std::make_shared<CollectionMockStats>();
    auto loader = std::make_unique<CollectionBulkLoaderMock>(stats);
    NamespaceString collNss;
    CollectionOptions collOptions;
    std::vector<BSONObj> collIndexSpecs;
    storageInterface->createCollectionForBulkFn =
        [&](const NamespaceString& theNss,
            const CollectionOptions& theOptions,
            const BSONObj idIndexSpec,
            const std::vector<BSONObj>& theIndexSpecs) -> std::unique_ptr<CollectionBulkLoader> {
        collNss = theNss;
        collOptions = theOptions;
        collIndexSpecs = theIndexSpecs;
        return std::move(loader);
    };

    // Split listIndexes response into 2 batches: first batch contains idIndexSpec and
    // second batch contains specs
    auto nonIdIndexSpecs = makeSecondaryIndexSpecs(nss);

    // First batch contains the _id_ index spec.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(1, BSON_ARRAY(idIndexSpec)));
    }

    // 'status' should not be modified because cloning is not finished.
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    // Second batch contains the other index specs.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createListIndexesResponse(
            0, BSON_ARRAY(nonIdIndexSpecs[0] << nonIdIndexSpecs[1]), "nextBatch"));
    }

    collectionCloner->waitForDbWorker();

    // 'status' will be set if listIndexes fails.
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());

    ASSERT_EQUALS(nss.ns(), collNss.ns());
    ASSERT_BSONOBJ_EQ(options.toBSON(), collOptions.toBSON());
    ASSERT_EQUALS(nonIdIndexSpecs.size(), collIndexSpecs.size());
    for (std::vector<BSONObj>::size_type i = 0; i < nonIdIndexSpecs.size(); ++i) {
        ASSERT_BSONOBJ_EQ(nonIdIndexSpecs[i], collIndexSpecs[i]);
    }

    // Cloner is still active because it has to read the documents from the source collection.
    ASSERT_TRUE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, FindFetcherScheduleFailed) {
    ASSERT_OK(collectionCloner->startup());

    // Shut down executor while in beginCollection callback.
    // This will cause the fetcher to fail to schedule the find command.
    auto stats = std::make_shared<CollectionMockStats>();
    auto loader = std::make_unique<CollectionBulkLoaderMock>(stats);
    bool collectionCreated = false;
    storageInterface->createCollectionForBulkFn =
        [&](const NamespaceString& theNss,
            const CollectionOptions& theOptions,
            const BSONObj idIndexSpec,
            const std::vector<BSONObj>& theIndexSpecs) -> std::unique_ptr<CollectionBulkLoader> {
        collectionCreated = true;
        getExecutor().shutdown();
        return std::move(loader);
    };

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCreated);

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, QueryAfterCreateCollection) {
    // Pause the CollectionCloner before executing the query so we can verify the collection is
    // created before the query.
    MockClientPauser pauser(_client);
    ASSERT_OK(collectionCloner->startup());

    auto stats = std::make_shared<CollectionMockStats>();
    auto loader = std::make_unique<CollectionBulkLoaderMock>(stats);
    bool collectionCreated = false;
    storageInterface->createCollectionForBulkFn =
        [&](const NamespaceString& theNss,
            const CollectionOptions& theOptions,
            const BSONObj idIndexSpec,
            const std::vector<BSONObj>& theIndexSpecs) -> std::unique_ptr<CollectionBulkLoader> {
        collectionCreated = true;
        return std::move(loader);
    };

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();
    ASSERT_TRUE(collectionCreated);
    // Make sure the query starts.
    _client->waitForPausedQuery();
}

TEST_F(CollectionClonerTest, QueryFailed) {
    // For this test to work properly, the error cannot be one of the special codes
    // (OperationFailed or CursorNotFound) which trigger an attempt to see if the collection
    // was deleted.
    _client->setFailureForQuery({ErrorCodes::UnknownError, "QueryFailedTest UnknownError"});
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::UnknownError, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsScheduleDbWorkFailed) {
    // Set up documents to be returned from upstream node.
    _server->insert(nss.ns(), BSON("_id" << 1));

    // Pause the client so we can set up the failure.
    MockClientPauser pauser(_client);
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(1));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();

    // Replace scheduleDbWork function so that cloner will fail to schedule DB work after
    // getting documents.
    collectionCloner->setScheduleDbWorkFn_forTest(
        [](const executor::TaskExecutor::CallbackFn& workFn) {
            return StatusWith<executor::TaskExecutor::CallbackHandle>(ErrorCodes::UnknownError, "");
        });

    pauser.resume();
    collectionCloner->join();

    ASSERT_EQUALS(ErrorCodes::UnknownError, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsCallbackCanceled) {
    // Set up documents to be returned from upstream node.
    _server->insert(nss.ns(), BSON("_id" << 1));

    // Pause the client so we can set up the failure.
    MockClientPauser pauser(_client);
    ASSERT_OK(collectionCloner->startup());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(1));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }

    collectionCloner->waitForDbWorker();

    // Replace scheduleDbWork function so that the callback runs with a cancelled status.
    auto&& executor = getExecutor();
    collectionCloner->setScheduleDbWorkFn_forTest(
        [&](const executor::TaskExecutor::CallbackFn& workFn) {
            executor::TaskExecutor::CallbackHandle handle(std::make_shared<MockCallbackState>());
            mongo::executor::TaskExecutor::CallbackArgs args{
                &executor,
                handle,
                {ErrorCodes::CallbackCanceled, "Never run, but treat like cancelled."}};
            workFn(args);
            return StatusWith<executor::TaskExecutor::CallbackHandle>(handle);
        });

    pauser.resume();
    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, getStatus().code());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsFailed) {
    // Set up documents to be returned from upstream node.
    _server->insert(nss.ns(), BSON("_id" << 1));

    // Pause the client so we can set up the failure.
    MockClientPauser pauser(_client);
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(1));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    ASSERT_TRUE(collectionCloner->isActive());
    getNet()->logQueues();

    _client->waitForPausedQuery();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats->initCalled);

    ASSERT(_loader != nullptr);
    _loader->insertDocsFn = [](const std::vector<BSONObj>::const_iterator begin,
                               const std::vector<BSONObj>::const_iterator end) {
        return Status(ErrorCodes::OperationFailed, "");
    };
    ASSERT_TRUE(collectionCloner->isActive());
    pauser.resume();

    collectionCloner->join();
    ASSERT_FALSE(collectionCloner->isActive());
    ASSERT_EQUALS(0, collectionStats->insertCount);

    ASSERT_EQUALS(ErrorCodes::OperationFailed, getStatus().code());
}

TEST_F(CollectionClonerTest, InsertDocumentsSingleBatch) {
    // Set up documents to be returned from upstream node.
    _server->insert(nss.ns(), BSON("_id" << 1));
    _server->insert(nss.ns(), BSON("_id" << 2));

    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(2));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    collectionCloner->join();
    // TODO: record the documents during insert and compare them
    //       -- maybe better done using a real storage engine, like ephemeral for test.
    ASSERT_EQUALS(2, collectionStats->insertCount);
    auto stats = collectionCloner->getStats();
    ASSERT_EQUALS(1u, stats.receivedBatches);
    ASSERT_TRUE(collectionStats->commitCalled);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, InsertDocumentsMultipleBatches) {
    // Set up documents to be returned from upstream node.
    _server->insert(nss.ns(), BSON("_id" << 1));
    _server->insert(nss.ns(), BSON("_id" << 2));
    _server->insert(nss.ns(), BSON("_id" << 3));

    collectionCloner->setBatchSize_forTest(2);
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(3));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    collectionCloner->join();
    // TODO: record the documents during insert and compare them
    //       -- maybe better done using a real storage engine, like ephemeral for test.
    ASSERT_EQUALS(3, collectionStats->insertCount);
    ASSERT_TRUE(collectionStats->commitCalled);
    auto stats = collectionCloner->getStats();
    ASSERT_EQUALS(2u, stats.receivedBatches);

    ASSERT_OK(getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

TEST_F(CollectionClonerTest, CollectionClonerTransitionsToCompleteIfShutdownBeforeStartup) {
    collectionCloner->shutdown();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, collectionCloner->startup());
}

/**
 * Start cloning.
 * Make it fail while copying collection.
 * Restarting cloning should fail with ErrorCodes::ShutdownInProgress error.
 */
TEST_F(CollectionClonerTest, CollectionClonerCannotBeRestartedAfterPreviousFailure) {
    // Set up document to return from upstream.
    _server->insert(nss.ns(), BSON("_id" << 1));

    // First cloning attempt - fails while reading documents from source collection.
    unittest::log() << "Starting first collection cloning attempt";
    _client->setFailureForQuery(
        {ErrorCodes::UnknownError, "failed to read remaining documents from source collection"});
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(1));
        processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
    }
    collectionCloner->join();

    ASSERT_EQUALS(ErrorCodes::UnknownError, getStatus());
    ASSERT_FALSE(collectionCloner->isActive());

    // Second cloning attempt - run to completion.
    unittest::log() << "Starting second collection cloning attempt - startup() should fail";
    *collectionStats = CollectionMockStats();
    setStatus(getDetectableErrorStatus());

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, collectionCloner->startup());
}

bool sharedCallbackStateDestroyed = false;
class SharedCallbackState {
    MONGO_DISALLOW_COPYING(SharedCallbackState);

public:
    SharedCallbackState() {}
    ~SharedCallbackState() {
        sharedCallbackStateDestroyed = true;
    }
};

TEST_F(CollectionClonerTest, CollectionClonerResetsOnCompletionCallbackFunctionAfterCompletion) {
    sharedCallbackStateDestroyed = false;
    auto sharedCallbackData = std::make_shared<SharedCallbackState>();

    Status result = getDetectableErrorStatus();
    collectionCloner =
        stdx::make_unique<CollectionCloner>(&getExecutor(),
                                            dbWorkThreadPool.get(),
                                            target,
                                            nss,
                                            options,
                                            [&result, sharedCallbackData](const Status& status) {
                                                log() << "setting result to " << status;
                                                result = status;
                                            },
                                            storageInterface.get(),
                                            defaultBatchSize);

    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    sharedCallbackData.reset();
    ASSERT_FALSE(sharedCallbackStateDestroyed);

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        auto request =
            net->scheduleErrorResponse(Status(ErrorCodes::OperationFailed, "count command failed"));
        ASSERT_EQUALS("count", request.cmdObj.firstElement().fieldNameStringData());
        net->runReadyNetworkOperations();
    }

    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, result);
    ASSERT_TRUE(sharedCallbackStateDestroyed);
}

TEST_F(CollectionClonerTest,
       CollectionClonerWaitsForPendingTasksToCompleteBeforeInvokingOnCompletionCallback) {
    MockClientPauser pauser(_client);
    ASSERT_OK(collectionCloner->startup());
    ASSERT_TRUE(collectionCloner->isActive());

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        assertRemoteCommandNameEquals("count",
                                      net->scheduleSuccessfulResponse(createCountResponse(0)));
        net->runReadyNetworkOperations();

        assertRemoteCommandNameEquals(
            "listIndexes",
            net->scheduleSuccessfulResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec))));
        net->runReadyNetworkOperations();
    }
    ASSERT_TRUE(collectionCloner->isActive());

    _client->waitForPausedQuery();
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_TRUE(collectionStats->initCalled);

    // At this point, the CollectionCloner is waiting for the query to complete.
    // We want to return the first batch of documents for the collection from the network so that
    // the CollectionCloner schedules the first _insertDocuments DB task and the getMore request for
    // the next batch of documents.

    // Store the scheduled CollectionCloner::_insertDocuments task but do not run it yet.
    executor::TaskExecutor::CallbackFn insertDocumentsFn;
    collectionCloner->setScheduleDbWorkFn_forTest([&](executor::TaskExecutor::CallbackFn workFn) {
        insertDocumentsFn = std::move(workFn);
        executor::TaskExecutor::CallbackHandle handle(std::make_shared<MockCallbackState>());
        return StatusWith<executor::TaskExecutor::CallbackHandle>(handle);
    });
    ASSERT_FALSE(insertDocumentsFn);

    // Return first batch of collection documents from remote server for the getMore request.
    const BSONObj doc = BSON("_id" << 1);
    _server->insert(nss.ns(), doc);
    _client->setFailureForQuery({ErrorCodes::UnknownError, "getMore failed"});
    // Wait for the _runQuery method to exit.  We can't get at it directly but we can wait
    // for a task scheduled after it to complete.
    auto& executor = getExecutor();
    // Schedule no-op task.
    auto nextTask =
        executor.scheduleWork([](const executor::TaskExecutor::CallbackArgs&) {}).getValue();
    pauser.resume();
    executor.wait(nextTask);

    // CollectionCloner should still be active because we have not finished processing the
    // insertDocuments task.
    ASSERT_TRUE(collectionCloner->isActive());
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());

    // Run the insertDocuments task. The final status of the CollectionCloner should match the first
    // error passed to the completion guard (ie. from the failed getMore request).
    executor::TaskExecutor::CallbackArgs callbackArgs(
        &getExecutor(), {}, Status(ErrorCodes::CallbackCanceled, ""));
    ASSERT_TRUE(insertDocumentsFn);
    insertDocumentsFn(callbackArgs);

    // Reset 'insertDocumentsFn' to release last reference count on completion guard.
    insertDocumentsFn = {};

    collectionCloner->join();

    ASSERT_FALSE(collectionCloner->isActive());
    ASSERT_EQUALS(ErrorCodes::UnknownError, getStatus());
}

class CollectionClonerRenamedBeforeStartTest : public CollectionClonerTest {
protected:
    // The CollectionCloner should deal gracefully with collections renamed before the cloner
    // was started, so start it with an alternate name.
    const NamespaceString alternateNss{"db", "alternateCollName"};
    const NamespaceString& getStartupNss() const override {
        return alternateNss;
    };

    /**
     * Sets up a test for the CollectionCloner that simulates the collection being dropped while
     * copying the documents by making a query return the given error code.
     *
     * The DBClientConnection returns 'code' to indicate a collection drop.
     */
    void setUpVerifyCollectionWasDroppedTest(ErrorCodes::Error code) {
        // Pause the query so we can reliably wait for it to complete.
        MockClientPauser pauser(_client);
        // Return error response from the query.
        _client->setFailureForQuery({code, "collection dropped while copying documents"});
        ASSERT_OK(collectionCloner->startup());
        {
            executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
            processNetworkResponse(createCountResponse(0));
            processNetworkResponse(createListIndexesResponse(0, BSON_ARRAY(idIndexSpec)));
        }
        ASSERT_TRUE(collectionCloner->isActive());

        _client->waitForPausedQuery();
        ASSERT_TRUE(collectionStats->initCalled);
        pauser.resumeAndWaitForResumedQuery();
    }

    /**
     * Returns the next ready request.
     * Ensures that the request was sent by the CollectionCloner to check if the collection was
     * dropped while copying documents.
     */
    executor::NetworkInterfaceMock::NetworkOperationIterator getVerifyCollectionDroppedRequest(
        executor::NetworkInterfaceMock* net) {
        ASSERT_TRUE(net->hasReadyRequests());
        auto noi = net->getNextReadyRequest();
        const auto& request = noi->getRequest();
        const auto& cmdObj = request.cmdObj;
        const auto firstElement = cmdObj.firstElement();
        ASSERT_EQUALS("find"_sd, firstElement.fieldNameStringData());
        ASSERT_EQUALS(*options.uuid, unittest::assertGet(UUID::parse(firstElement)));
        return noi;
    }

    /**
     * Start cloning. While copying collection, simulate a collection drop by having the
     * DBClientConnection return code 'collectionDropErrCode'.
     *
     * The CollectionCloner should run a find command on the collection by UUID. Simulate successful
     * find command with a drop-pending namespace in the response.  The CollectionCloner should
     * complete with a successful final status.
     */
    void runCloningSuccessfulWithCollectionDropTest(ErrorCodes::Error collectionDropErrCode) {
        setUpVerifyCollectionWasDroppedTest(collectionDropErrCode);

        // CollectionCloner should send a find command with the collection's UUID.
        {
            executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
            auto noi = getVerifyCollectionDroppedRequest(getNet());

            // Return a drop-pending namespace in the find response instead of the original
            // collection name passed to CollectionCloner at construction.
            repl::OpTime dropOpTime(Timestamp(Seconds(100), 0), 1LL);
            auto dpns = nss.makeDropPendingNamespace(dropOpTime);
            scheduleNetworkResponse(noi,
                                    createCursorResponse(0, dpns.ns(), BSONArray(), "firstBatch"));
            finishProcessingNetworkResponse();
        }

        // CollectionCloner treats a in collection state to drop-pending during cloning as a
        // successful
        // clone operation.
        collectionCloner->join();
        ASSERT_OK(getStatus());
        ASSERT_FALSE(collectionCloner->isActive());
    }
};

TEST_F(CollectionClonerRenamedBeforeStartTest, FirstRemoteCommandWithRenamedCollection) {
    ASSERT_OK(collectionCloner->startup());

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
    ASSERT_TRUE(net->hasReadyRequests());
    NetworkOperationIterator noi = net->getNextReadyRequest();
    auto&& noiRequest = noi->getRequest();
    ASSERT_EQUALS(nss.db().toString(), noiRequest.dbname);
    ASSERT_EQUALS("count", std::string(noiRequest.cmdObj.firstElementFieldName()));
    auto requestUUID = uassertStatusOK(UUID::parse(noiRequest.cmdObj.firstElement()));
    ASSERT_EQUALS(options.uuid.get(), requestUUID);

    ASSERT_FALSE(net->hasReadyRequests());
    ASSERT_TRUE(collectionCloner->isActive());
}

TEST_F(CollectionClonerRenamedBeforeStartTest, BeginCollectionWithUUID) {
    auto stats = std::make_shared<CollectionMockStats>();
    auto loader = std::make_unique<CollectionBulkLoaderMock>(stats);
    NamespaceString collNss;
    CollectionOptions collOptions;
    BSONObj collIdIndexSpec;
    std::vector<BSONObj> collSecondaryIndexSpecs;
    storageInterface->createCollectionForBulkFn =
        [&](const NamespaceString& theNss,
            const CollectionOptions& theOptions,
            const BSONObj idIndexSpec,
            const std::vector<BSONObj>& nonIdIndexSpecs) -> std::unique_ptr<CollectionBulkLoader> {
        collNss = theNss;
        collOptions = theOptions;
        collIdIndexSpec = idIndexSpec;
        collSecondaryIndexSpecs = nonIdIndexSpecs;
        return std::move(loader);
    };

    // Pause the client so the cloner stops in the fetcher.
    MockClientPauser pauser(_client);

    ASSERT_OK(collectionCloner->startup());

    // Split listIndexes response into 2 batches: first batch contains idIndexSpec and
    // second batch contains specs. We expect the collection cloner to fix up the collection names
    // (here from 'nss' to 'alternateNss') in the index specs, as the collection with the given UUID
    // may be known with a different name by the sync source due to a rename or two-phase drop.
    auto nonIdIndexSpecsToReturnBySyncSource = makeSecondaryIndexSpecs(nss);

    // First batch contains the _id_ index spec.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(createCountResponse(0));
        processNetworkResponse(createListIndexesResponse(1, BSON_ARRAY(idIndexSpec)));
    }

    // 'status' should not be modified because cloning is not finished.
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());
    ASSERT_TRUE(collectionCloner->isActive());

    // Second batch contains the other index specs.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        processNetworkResponse(
            createListIndexesResponse(0,
                                      BSON_ARRAY(nonIdIndexSpecsToReturnBySyncSource[0]
                                                 << nonIdIndexSpecsToReturnBySyncSource[1]),
                                      "nextBatch"));
    }

    collectionCloner->waitForDbWorker();

    // 'status' will be set if listIndexes fails.
    ASSERT_EQUALS(getDetectableErrorStatus(), getStatus());

    ASSERT_EQUALS(collNss.ns(), alternateNss.ns());
    ASSERT_BSONOBJ_EQ(options.toBSON(), collOptions.toBSON());

    BSONObj expectedIdIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                           << "_id_"
                                           << "ns"
                                           << alternateNss.ns());
    ASSERT_BSONOBJ_EQ(collIdIndexSpec, expectedIdIndexSpec);

    auto expectedNonIdIndexSpecs = makeSecondaryIndexSpecs(alternateNss);
    ASSERT_EQUALS(collSecondaryIndexSpecs.size(), expectedNonIdIndexSpecs.size());

    for (std::vector<BSONObj>::size_type i = 0; i < expectedNonIdIndexSpecs.size(); ++i) {
        ASSERT_BSONOBJ_EQ(collSecondaryIndexSpecs[i], expectedNonIdIndexSpecs[i]);
    }

    // Cloner is still active because it has to read the documents from the source collection.
    ASSERT_TRUE(collectionCloner->isActive());
}

TEST_F(CollectionClonerRenamedBeforeStartTest,
       CloningIsSuccessfulIfCollectionWasDroppedWithCursorNotFoundWhileCopyingDocuments) {
    runCloningSuccessfulWithCollectionDropTest(ErrorCodes::CursorNotFound);
}

TEST_F(CollectionClonerRenamedBeforeStartTest,
       CloningIsSuccessfulIfCollectionWasDroppedWithOperationFailedWhileCopyingDocuments) {
    runCloningSuccessfulWithCollectionDropTest(ErrorCodes::OperationFailed);
}

TEST_F(CollectionClonerRenamedBeforeStartTest,
       CloningIsSuccessfulIfCollectionWasDroppedWithQueryPlanKilledWhileCopyingDocuments) {
    runCloningSuccessfulWithCollectionDropTest(ErrorCodes::QueryPlanKilled);
}

/**
 * Start cloning.  While copying collection, simulate a collection drop by having the
 * DBClientConnection return a CursorNotFound error.
 *
 * The CollectionCloner should run a find command on the collection by UUID.  Shut the
 * CollectionCloner down.  The CollectionCloner should return final status corresponding to the
 * error code from the DBClientConnection.
 */
TEST_F(CollectionClonerRenamedBeforeStartTest,
       ShuttingDownCollectionClonerDuringCollectionDropVerificationReturnsCallbackCanceled) {
    setUpVerifyCollectionWasDroppedTest(ErrorCodes::CursorNotFound);

    // CollectionCloner should send a find command with the collection's UUID.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        auto noi = getVerifyCollectionDroppedRequest(getNet());

        // Ignore the find request.
        guard->blackHole(noi);
    }

    // Shut the CollectionCloner down. This should cancel the _verifyCollectionDropped() request.
    collectionCloner->shutdown();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        guard->runReadyNetworkOperations();
    }

    collectionCloner->join();
    ASSERT_EQUALS(ErrorCodes::CursorNotFound, getStatus());
    ASSERT_FALSE(collectionCloner->isActive());
}

}  // namespace
