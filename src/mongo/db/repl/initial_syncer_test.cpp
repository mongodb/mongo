/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <iosfwd>
#include <memory>
#include <ostream>

#include "mongo/client/fetcher.h"
#include "mongo/db/client.h"
#include "mongo/db/json.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/repl/base_cloner_test_fixture.h"
#include "mongo/db/repl/data_replicator_external_state_mock.h"
#include "mongo/db/repl/initial_syncer.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/reporter.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/repl/sync_source_selector_mock.h"
#include "mongo/db/repl/task_executor_mock.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/old_thread_pool.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {

/**
 * Insertion operator for InitialSyncer::State. Formats initial syncer state for output stream.
 */
std::ostream& operator<<(std::ostream& os, const InitialSyncer::State& state) {
    switch (state) {
        case InitialSyncer::State::kPreStart:
            return os << "PreStart";
        case InitialSyncer::State::kRunning:
            return os << "Running";
        case InitialSyncer::State::kShuttingDown:
            return os << "ShuttingDown";
        case InitialSyncer::State::kComplete:
            return os << "Complete";
    }
    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo


namespace {

using namespace mongo;
using namespace mongo::repl;

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using unittest::log;

using LockGuard = stdx::lock_guard<stdx::mutex>;
using NetworkGuard = executor::NetworkInterfaceMock::InNetworkGuard;
using UniqueLock = stdx::unique_lock<stdx::mutex>;

struct CollectionCloneInfo {
    CollectionMockStats stats;
    CollectionBulkLoaderMock* loader = nullptr;
    Status status{ErrorCodes::NotYetInitialized, ""};
};

class InitialSyncerTest : public executor::ThreadPoolExecutorTest, public SyncSourceSelector {
public:
    InitialSyncerTest() {}

    executor::ThreadPoolMock::Options makeThreadPoolMockOptions() const override;

    /**
     * clear/reset state
     */
    void reset() {
        _setMyLastOptime = [this](const OpTime& opTime) { _myLastOpTime = opTime; };
        _myLastOpTime = OpTime();
        _syncSourceSelector = stdx::make_unique<SyncSourceSelectorMock>();
    }

    // SyncSourceSelector
    void clearSyncSourceBlacklist() override {
        _syncSourceSelector->clearSyncSourceBlacklist();
    }
    HostAndPort chooseNewSyncSource(const OpTime& ot) override {
        return _syncSourceSelector->chooseNewSyncSource(ot);
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {
        _syncSourceSelector->blacklistSyncSource(host, until);
    }
    bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                const rpc::ReplSetMetadata& replMetadata,
                                boost::optional<rpc::OplogQueryMetadata> oqMetadata) override {
        return _syncSourceSelector->shouldChangeSyncSource(currentSource, replMetadata, oqMetadata);
    }

    void scheduleNetworkResponse(std::string cmdName, const BSONObj& obj) {
        NetworkInterfaceMock* net = getNet();
        if (!net->hasReadyRequests()) {
            log() << "The network doesn't have a request to process for this response: " << obj;
        }
        verifyNextRequestCommandName(cmdName);
        scheduleNetworkResponse(net->getNextReadyRequest(), obj);
    }

    void scheduleNetworkResponse(NetworkInterfaceMock::NetworkOperationIterator noi,
                                 const BSONObj& obj) {
        NetworkInterfaceMock* net = getNet();
        Milliseconds millis(0);
        RemoteCommandResponse response(obj, BSONObj(), millis);
        log() << "Sending response for network request:";
        log() << "     req: " << noi->getRequest().dbname << "." << noi->getRequest().cmdObj;
        log() << "     resp:" << response;

        net->scheduleResponse(noi, net->now(), response);
    }

    void scheduleNetworkResponse(std::string cmdName, Status errorStatus) {
        NetworkInterfaceMock* net = getNet();
        if (!getNet()->hasReadyRequests()) {
            log() << "The network doesn't have a request to process for the error: " << errorStatus;
        }
        verifyNextRequestCommandName(cmdName);
        net->scheduleResponse(net->getNextReadyRequest(), net->now(), errorStatus);
    }

    void processNetworkResponse(std::string cmdName, const BSONObj& obj) {
        scheduleNetworkResponse(cmdName, obj);
        finishProcessingNetworkResponse();
    }

    void processNetworkResponse(std::string cmdName, Status errorStatus) {
        scheduleNetworkResponse(cmdName, errorStatus);
        finishProcessingNetworkResponse();
    }

    /**
     * Schedules and processes a successful response to the network request sent by InitialSyncer's
     * last oplog entry fetcher. Also validates the find command arguments in the request.
     */
    void processSuccessfulLastOplogEntryFetcherResponse(std::vector<BSONObj> docs);

    void finishProcessingNetworkResponse() {
        getNet()->runReadyNetworkOperations();
        if (getNet()->hasReadyRequests()) {
            log() << "The network has unexpected requests to process, next req:";
            NetworkInterfaceMock::NetworkOperation req = *getNet()->getNextReadyRequest();
            log() << req.getDiagnosticString();
        }
        ASSERT_FALSE(getNet()->hasReadyRequests());
    }

    InitialSyncer& getInitialSyncer() {
        return *_initialSyncer;
    }

    DataReplicatorExternalStateMock* getExternalState() {
        return _externalState;
    }

    StorageInterface& getStorage() {
        return *_storageInterface;
    }

    OldThreadPool& getDbWorkThreadPool() {
        return *_dbWorkThreadPool;
    }

protected:
    struct StorageInterfaceResults {
        bool createOplogCalled = false;
        bool insertedOplogEntries = false;
        int oplogEntriesInserted = 0;
        bool droppedUserDBs = false;
        std::vector<std::string> droppedCollections;
        int documentsInsertedCount = 0;
    };

    stdx::mutex _storageInterfaceWorkDoneMutex;  // protects _storageInterfaceWorkDone.
    StorageInterfaceResults _storageInterfaceWorkDone;

    void setUp() override {
        executor::ThreadPoolExecutorTest::setUp();
        _storageInterface = stdx::make_unique<StorageInterfaceMock>();
        _storageInterface->createOplogFn = [this](OperationContext* opCtx,
                                                  const NamespaceString& nss) {
            LockGuard lock(_storageInterfaceWorkDoneMutex);
            _storageInterfaceWorkDone.createOplogCalled = true;
            return Status::OK();
        };
        _storageInterface->insertDocumentFn =
            [this](OperationContext* opCtx, const NamespaceString& nss, const BSONObj& doc) {
                LockGuard lock(_storageInterfaceWorkDoneMutex);
                ++_storageInterfaceWorkDone.documentsInsertedCount;
                return Status::OK();
            };
        _storageInterface->insertDocumentsFn = [this](
            OperationContext* opCtx, const NamespaceString& nss, const std::vector<BSONObj>& ops) {
            LockGuard lock(_storageInterfaceWorkDoneMutex);
            _storageInterfaceWorkDone.insertedOplogEntries = true;
            ++_storageInterfaceWorkDone.oplogEntriesInserted;
            return Status::OK();
        };
        _storageInterface->dropCollFn = [this](OperationContext* opCtx,
                                               const NamespaceString& nss) {
            LockGuard lock(_storageInterfaceWorkDoneMutex);
            _storageInterfaceWorkDone.droppedCollections.push_back(nss.ns());
            return Status::OK();
        };
        _storageInterface->dropUserDBsFn = [this](OperationContext* opCtx) {
            LockGuard lock(_storageInterfaceWorkDoneMutex);
            _storageInterfaceWorkDone.droppedUserDBs = true;
            return Status::OK();
        };
        _storageInterface->createCollectionForBulkFn =
            [this](const NamespaceString& nss,
                   const CollectionOptions& options,
                   const BSONObj idIndexSpec,
                   const std::vector<BSONObj>& secondaryIndexSpecs) {
                // Get collection info from map.
                const auto collInfo = &_collections[nss];
                if (collInfo->stats.initCalled) {
                    log() << "reusing collection during test which may cause problems, ns:" << nss;
                }
                (collInfo->loader = new CollectionBulkLoaderMock(&collInfo->stats))
                    ->init(nullptr, secondaryIndexSpecs);

                return StatusWith<std::unique_ptr<CollectionBulkLoader>>(
                    std::unique_ptr<CollectionBulkLoader>(collInfo->loader));
            };

        _dbWorkThreadPool = stdx::make_unique<OldThreadPool>(1);

        Client::initThreadIfNotAlready();
        reset();

        launchExecutorThread();

        _executorProxy = stdx::make_unique<TaskExecutorMock>(&getExecutor());

        _myLastOpTime = OpTime({3, 0}, 1);

        InitialSyncerOptions options;
        options.initialSyncRetryWait = Milliseconds(1);
        options.getMyLastOptime = [this]() { return _myLastOpTime; };
        options.setMyLastOptime = [this](const OpTime& opTime) { _setMyLastOptime(opTime); };
        options.getSlaveDelay = [this]() { return Seconds(0); };
        options.syncSourceSelector = this;

        _options = options;

        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.poolName = "replication";
        threadPoolOptions.minThreads = 1U;
        threadPoolOptions.maxThreads = 1U;
        threadPoolOptions.onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName.c_str());
        };

        auto dataReplicatorExternalState = stdx::make_unique<DataReplicatorExternalStateMock>();
        dataReplicatorExternalState->taskExecutor = _executorProxy.get();
        dataReplicatorExternalState->dbWorkThreadPool = &getDbWorkThreadPool();
        dataReplicatorExternalState->currentTerm = 1LL;
        dataReplicatorExternalState->lastCommittedOpTime = _myLastOpTime;
        {
            ReplSetConfig config;
            ASSERT_OK(config.initialize(BSON("_id"
                                             << "myset"
                                             << "version"
                                             << 1
                                             << "protocolVersion"
                                             << 1
                                             << "members"
                                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                      << "localhost:12345"))
                                             << "settings"
                                             << BSON("electionTimeoutMillis" << 10000))));
            dataReplicatorExternalState->replSetConfigResult = config;
        }
        _externalState = dataReplicatorExternalState.get();

        _lastApplied = getDetectableErrorStatus();
        _onCompletion = [this](const StatusWith<OpTimeWithHash>& lastApplied) {
            _lastApplied = lastApplied;
        };

        try {
            // When creating InitialSyncer, we wrap _onCompletion so that we can override the
            // InitialSyncer's callback behavior post-construction.
            // See InitialSyncerTransitionsToCompleteWhenFinishCallbackThrowsException.
            _initialSyncer = stdx::make_unique<InitialSyncer>(
                options,
                std::move(dataReplicatorExternalState),
                _storageInterface.get(),
                [this](const StatusWith<OpTimeWithHash>& lastApplied) {
                    _onCompletion(lastApplied);
                });
            _initialSyncer->setScheduleDbWorkFn_forTest(
                [this](const executor::TaskExecutor::CallbackFn& work) {
                    return getExecutor().scheduleWork(work);
                });

        } catch (...) {
            ASSERT_OK(exceptionToStatus());
        }
    }

    void tearDownExecutorThread() {
        if (_executorThreadShutdownComplete) {
            return;
        }
        executor::ThreadPoolExecutorTest::shutdownExecutorThread();
        executor::ThreadPoolExecutorTest::joinExecutorThread();
        _executorThreadShutdownComplete = true;
    }

    void tearDown() override {
        tearDownExecutorThread();
        _initialSyncer.reset();
        _dbWorkThreadPool->join();
        _dbWorkThreadPool.reset();
        _storageInterface.reset();

        // tearDown() destroys the task executor which was referenced by the initial syncer.
        executor::ThreadPoolExecutorTest::tearDown();
    }

    /**
     * Note: An empty cmdName will skip validation.
     */
    void verifyNextRequestCommandName(std::string cmdName) {
        const auto net = getNet();
        ASSERT_TRUE(net->hasReadyRequests());

        if (cmdName != "") {
            const NetworkInterfaceMock::NetworkOperationIterator req =
                net->getFrontOfUnscheduledQueue();
            const BSONObj reqBSON = req->getRequest().cmdObj;
            const BSONElement cmdElem = reqBSON.firstElement();
            auto reqCmdName = cmdElem.fieldNameStringData();
            ASSERT_EQ(cmdName, reqCmdName);
        }
    }

    std::unique_ptr<TaskExecutorMock> _executorProxy;

    InitialSyncerOptions _options;
    InitialSyncerOptions::SetMyLastOptimeFn _setMyLastOptime;
    OpTime _myLastOpTime;
    std::unique_ptr<SyncSourceSelectorMock> _syncSourceSelector;
    std::unique_ptr<StorageInterfaceMock> _storageInterface;
    std::unique_ptr<OldThreadPool> _dbWorkThreadPool;
    std::map<NamespaceString, CollectionMockStats> _collectionStats;
    std::map<NamespaceString, CollectionCloneInfo> _collections;

    StatusWith<OpTimeWithHash> _lastApplied = Status(ErrorCodes::NotYetInitialized, "");
    InitialSyncer::OnCompletionFn _onCompletion;

private:
    DataReplicatorExternalStateMock* _externalState;
    std::unique_ptr<InitialSyncer> _initialSyncer;
    bool _executorThreadShutdownComplete = false;
};

executor::ThreadPoolMock::Options InitialSyncerTest::makeThreadPoolMockOptions() const {
    executor::ThreadPoolMock::Options options;
    options.onCreateThread = []() { Client::initThread("InitialSyncerTest"); };
    return options;
}

void advanceClock(NetworkInterfaceMock* net, Milliseconds duration) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    auto when = net->now() + duration;
    ASSERT_EQUALS(when, net->runUntil(when));
}

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

/**
 * Generates a replSetGetRBID response.
 */
BSONObj makeRollbackCheckerResponse(int rollbackId) {
    return BSON("ok" << 1 << "rbid" << rollbackId);
}

/**
 * Generates a cursor response for a Fetcher to consume.
 */
RemoteCommandResponse makeCursorResponse(CursorId cursorId,
                                         const NamespaceString& nss,
                                         std::vector<BSONObj> docs,
                                         bool isFirstBatch = true,
                                         int rbid = 1) {
    OpTime futureOpTime(Timestamp(1000, 1000), 1000);
    rpc::OplogQueryMetadata oqMetadata(futureOpTime, futureOpTime, rbid, 0, 0);
    BSONObjBuilder metadataBob;
    ASSERT_OK(oqMetadata.writeToMetadata(&metadataBob));
    auto metadataObj = metadataBob.obj();

    BSONObjBuilder bob;
    {
        BSONObjBuilder cursorBob(bob.subobjStart("cursor"));
        cursorBob.append("id", cursorId);
        cursorBob.append("ns", nss.toString());
        {
            BSONArrayBuilder batchBob(
                cursorBob.subarrayStart(isFirstBatch ? "firstBatch" : "nextBatch"));
            for (const auto& doc : docs) {
                batchBob.append(doc);
            }
        }
    }
    bob.append("ok", 1);
    return {bob.obj(), metadataObj, Milliseconds(0)};
}

/**
 * Generates a listDatabases response for a DatabasesCloner to consume.
 */
BSONObj makeListDatabasesResponse(std::vector<std::string> databaseNames) {
    BSONObjBuilder bob;
    {
        BSONArrayBuilder databasesBob(bob.subarrayStart("databases"));
        for (const auto& name : databaseNames) {
            BSONObjBuilder nameBob(databasesBob.subobjStart());
            nameBob.append("name", name);
        }
    }
    bob.append("ok", 1);
    return bob.obj();
}

/**
 * Generates oplog entries with the given number used for the timestamp.
 */
BSONObj makeOplogEntry(int t, const char* opType = "i", int version = OplogEntry::kOplogVersion) {
    return BSON("ts" << Timestamp(t, 1) << "h" << static_cast<long long>(t) << "ns"
                     << "a.a"
                     << "v"
                     << version
                     << "op"
                     << opType
                     << "o"
                     << BSON("_id" << t << "a" << t));
}

void InitialSyncerTest::processSuccessfulLastOplogEntryFetcherResponse(std::vector<BSONObj> docs) {
    auto net = getNet();
    auto request = assertRemoteCommandNameEquals(
        "find",
        net->scheduleSuccessfulResponse(makeCursorResponse(0LL, _options.localOplogNS, docs)));
    ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
    ASSERT_TRUE(request.cmdObj.hasField("sort"));
    ASSERT_EQUALS(mongo::BSONType::Object, request.cmdObj["sort"].type());
    ASSERT_BSONOBJ_EQ(BSON("$natural" << -1), request.cmdObj.getObjectField("sort"));
    net->runReadyNetworkOperations();
}

TEST_F(InitialSyncerTest, InvalidConstruction) {
    InitialSyncerOptions options;
    options.getMyLastOptime = []() { return OpTime(); };
    options.setMyLastOptime = [](const OpTime&) {};
    options.getSlaveDelay = []() { return Seconds(0); };
    options.syncSourceSelector = this;
    auto callback = [](const StatusWith<OpTimeWithHash>&) {};

    // Null task executor in external state.
    {
        auto dataReplicatorExternalState = stdx::make_unique<DataReplicatorExternalStateMock>();
        ASSERT_THROWS_CODE_AND_WHAT(
            InitialSyncer(
                options, std::move(dataReplicatorExternalState), _storageInterface.get(), callback),
            UserException,
            ErrorCodes::BadValue,
            "task executor cannot be null");
    }

    // Null callback function.
    {
        auto dataReplicatorExternalState = stdx::make_unique<DataReplicatorExternalStateMock>();
        dataReplicatorExternalState->taskExecutor = &getExecutor();
        ASSERT_THROWS_CODE_AND_WHAT(InitialSyncer(options,
                                                  std::move(dataReplicatorExternalState),
                                                  _storageInterface.get(),
                                                  InitialSyncer::OnCompletionFn()),
                                    UserException,
                                    ErrorCodes::BadValue,
                                    "callback function cannot be null");
    }
}

TEST_F(InitialSyncerTest, CreateDestroy) {}

const std::uint32_t maxAttempts = 1U;

TEST_F(InitialSyncerTest, StartupReturnsIllegalOperationIfAlreadyActive) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();
    ASSERT_FALSE(initialSyncer->isActive());
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));
    ASSERT_TRUE(initialSyncer->isActive());
    ASSERT_EQUALS(ErrorCodes::IllegalOperation, initialSyncer->startup(opCtx.get(), maxAttempts));
    ASSERT_TRUE(initialSyncer->isActive());
}

TEST_F(InitialSyncerTest, StartupReturnsShutdownInProgressIfInitialSyncerIsShuttingDown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();
    ASSERT_FALSE(initialSyncer->isActive());
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));
    ASSERT_TRUE(initialSyncer->isActive());
    // SyncSourceSelector returns an invalid sync source so InitialSyncer is stuck waiting for
    // another sync source in 'Options::syncSourceRetryWait' ms.
    ASSERT_OK(initialSyncer->shutdown());
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, initialSyncer->startup(opCtx.get(), maxAttempts));
}

TEST_F(InitialSyncerTest, StartupReturnsShutdownInProgressIfExecutorIsShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();
    getExecutor().shutdown();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, initialSyncer->startup(opCtx.get(), maxAttempts));
    ASSERT_FALSE(initialSyncer->isActive());

    // Cannot startup initial syncer again since it's in the Complete state.
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, initialSyncer->startup(opCtx.get(), maxAttempts));
}

TEST_F(InitialSyncerTest, ShutdownTransitionsStateToCompleteIfCalledBeforeStartup) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();
    ASSERT_OK(initialSyncer->shutdown());
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, initialSyncer->startup(opCtx.get(), maxAttempts));
    // Initial syncer is inactive when it's in the Complete state.
    ASSERT_FALSE(initialSyncer->isActive());
}

TEST_F(InitialSyncerTest, StartupSetsInitialSyncFlagOnSuccess) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Initial sync flag should not be set before starting.
    ASSERT_FALSE(getStorage().getInitialSyncFlag(opCtx.get()));

    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));
    ASSERT_TRUE(initialSyncer->isActive());

    // Initial sync flag should be set.
    ASSERT_TRUE(getStorage().getInitialSyncFlag(opCtx.get()));
}

TEST_F(InitialSyncerTest, InitialSyncerReturnsCallbackCanceledIfShutdownImmediatelyAfterStartup) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    // This will cancel the _startInitialSyncAttemptCallback() task scheduled by startup().
    ASSERT_OK(initialSyncer->shutdown());

    // Depending on which InitialSyncer stage (_chooseSyncSource or _rollbackCheckerResetCallback)
    // was interrupted by shutdown(), we may have to request the network interface to deliver
    // cancellation signals to the InitialSyncer callbacks in for InitialSyncer to run to
    // completion.
    executor::NetworkInterfaceMock::InNetworkGuard(getNet())->runReadyNetworkOperations();

    initialSyncer->join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerRetriesSyncSourceSelectionIfChooseNewSyncSourceReturnsInvalidSyncSource) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Override chooseNewSyncSource() result in SyncSourceSelectorMock before calling startup()
    // because InitialSyncer will look for a valid sync source immediately after startup.
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());

    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    // Run first sync source selection attempt.
    executor::NetworkInterfaceMock::InNetworkGuard(getNet())->runReadyNetworkOperations();

    // InitialSyncer will not drop user databases while looking for a valid sync source.
    ASSERT_FALSE(_storageInterfaceWorkDone.droppedUserDBs);

    // First sync source selection attempt failed. Update SyncSourceSelectorMock to return valid
    // sync source next time chooseNewSyncSource() is called.
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));

    // Advance clock until the next sync source selection attempt.
    advanceClock(getNet(), _options.syncSourceRetryWait);

    // DataReplictor drops user databases after obtaining a valid sync source.
    ASSERT_TRUE(_storageInterfaceWorkDone.droppedUserDBs);
}

const std::uint32_t chooseSyncSourceMaxAttempts = 10U;

/**
 * Advances executor clock so that InitialSyncer exhausts all 'chooseSyncSourceMaxAttempts' (server
 * parameter numInitialSyncConnectAttempts) sync source selection attempts.
 * If SyncSourceSelectorMock keeps returning an invalid sync source, InitialSyncer will retry every
 * '_options.syncSourceRetryWait' ms up to a maximum of 'chooseSyncSourceMaxAttempts' attempts.
 */
void _simulateChooseSyncSourceFailure(executor::NetworkInterfaceMock* net,
                                      Milliseconds syncSourceRetryWait) {
    advanceClock(net, int(chooseSyncSourceMaxAttempts - 1) * syncSourceRetryWait);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerReturnsInitialSyncOplogSourceMissingIfNoValidSyncSourceCanBeFoundAfterTenFailedChooseSyncSourceAttempts) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Override chooseNewSyncSource() result in SyncSourceSelectorMock before calling startup()
    // because InitialSyncer will look for a valid sync source immediately after startup.
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());

    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    _simulateChooseSyncSourceFailure(getNet(), _options.syncSourceRetryWait);

    initialSyncer->join();

    ASSERT_EQUALS(ErrorCodes::InitialSyncOplogSourceMissing, _lastApplied);
}

// Confirms that InitialSyncer keeps retrying initial sync.
// Make every initial sync attempt fail early by having the sync source selector always return an
// invalid sync source.
TEST_F(InitialSyncerTest,
       InitialSyncerRetriesInitialSyncUpToMaxAttemptsAndReturnsLastAttemptError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());

    const std::uint32_t initialSyncMaxAttempts = 3U;
    ASSERT_OK(initialSyncer->startup(opCtx.get(), initialSyncMaxAttempts));

    auto net = getNet();
    for (std::uint32_t i = 0; i < initialSyncMaxAttempts; ++i) {
        _simulateChooseSyncSourceFailure(net, _options.syncSourceRetryWait);
        advanceClock(net, _options.initialSyncRetryWait);
    }

    initialSyncer->join();

    ASSERT_EQUALS(ErrorCodes::InitialSyncOplogSourceMissing, _lastApplied);

    // Check number of failed attempts in stats.
    auto progress = initialSyncer->getInitialSyncProgress();
    unittest::log() << "Progress after " << initialSyncMaxAttempts
                    << " failed attempts: " << progress;
    ASSERT_EQUALS(progress.getIntField("failedInitialSyncAttempts"), int(initialSyncMaxAttempts))
        << progress;
    ASSERT_EQUALS(progress.getIntField("maxFailedInitialSyncAttempts"), int(initialSyncMaxAttempts))
        << progress;
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsCallbackCanceledIfShutdownWhileRetryingSyncSourceSelection) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        auto when = net->now() + _options.syncSourceRetryWait / 2;
        ASSERT_GREATER_THAN(when, net->now());
        ASSERT_EQUALS(when, net->runUntil(when));
    }

    // This will cancel the _chooseSyncSourceCallback() task scheduled at getNet()->now() +
    // '_options.syncSourceRetryWait'.
    ASSERT_OK(initialSyncer->shutdown());

    initialSyncer->join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsScheduleErrorIfTaskExecutorFailsToScheduleNextChooseSyncSourceCallback) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());
    _executorProxy->shouldFailScheduleWorkAtRequest = []() { return true; };
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    initialSyncer->join();

    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsScheduleErrorIfTaskExecutorFailsToScheduleNextInitialSyncAttempt) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());

    ASSERT_EQUALS(InitialSyncer::State::kPreStart, initialSyncer->getState_forTest());

    ASSERT_OK(initialSyncer->startup(opCtx.get(), 2U));
    ASSERT_EQUALS(InitialSyncer::State::kRunning, initialSyncer->getState_forTest());

    // Advance clock so that we run all but the last sync source callback.
    auto net = getNet();
    advanceClock(net, int(chooseSyncSourceMaxAttempts - 2) * _options.syncSourceRetryWait);

    // Last choose sync source attempt should now be scheduled. Advance clock so we fail last
    // choose sync source attempt which cause the next initial sync attempt to be scheduled.
    _executorProxy->shouldFailScheduleWorkAtRequest = []() { return true; };
    advanceClock(net, _options.syncSourceRetryWait);

    initialSyncer->join();

    ASSERT_EQUALS(InitialSyncer::State::kComplete, initialSyncer->getState_forTest());
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

// This test verifies that the initial syncer will still transition to a complete state even if
// the completion callback function throws an exception.
TEST_F(InitialSyncerTest, InitialSyncerTransitionsToCompleteWhenFinishCallbackThrowsException) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _onCompletion = [this](const StatusWith<OpTimeWithHash>& lastApplied) {
        _lastApplied = lastApplied;
        uassert(ErrorCodes::InternalError, "", false);
    };

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort());
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_OK(initialSyncer->shutdown());
    initialSyncer->join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

class SharedCallbackState {
    MONGO_DISALLOW_COPYING(SharedCallbackState);

public:
    explicit SharedCallbackState(bool* sharedCallbackStateDestroyed)
        : _sharedCallbackStateDestroyed(sharedCallbackStateDestroyed) {}
    ~SharedCallbackState() {
        *_sharedCallbackStateDestroyed = true;
    }

private:
    bool* _sharedCallbackStateDestroyed;
};

TEST_F(InitialSyncerTest, InitialSyncerResetsOnCompletionCallbackFunctionPointerUponCompletion) {
    bool sharedCallbackStateDestroyed = false;
    auto sharedCallbackData = std::make_shared<SharedCallbackState>(&sharedCallbackStateDestroyed);
    decltype(_lastApplied) lastApplied = getDetectableErrorStatus();

    auto dataReplicatorExternalState = stdx::make_unique<DataReplicatorExternalStateMock>();
    dataReplicatorExternalState->taskExecutor = &getExecutor();
    auto initialSyncer = stdx::make_unique<InitialSyncer>(
        _options,
        std::move(dataReplicatorExternalState),
        _storageInterface.get(),
        [&lastApplied, sharedCallbackData](const StatusWith<OpTimeWithHash>& result) {
            lastApplied = result;
        });
    ON_BLOCK_EXIT([this]() { getExecutor().shutdown(); });

    auto opCtx = makeOpCtx();

    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    sharedCallbackData.reset();
    ASSERT_FALSE(sharedCallbackStateDestroyed);

    ASSERT_OK(initialSyncer->shutdown());

    // Depending on which InitialSyncer stage (_chooseSyncSource or _rollbackCheckerResetCallback)
    // was interrupted by shutdown(), we may have to request the network interface to deliver
    // cancellation signals to the InitialSyncer callbacks in for InitialSyncer to run to
    // completion.
    executor::NetworkInterfaceMock::InNetworkGuard(getNet())->runReadyNetworkOperations();

    initialSyncer->join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, lastApplied);

    // InitialSyncer should reset 'InitialSyncer::_onCompletion' after running callback function
    // for the last time before becoming inactive.
    // This ensures that we release resources associated with 'InitialSyncer::_onCompletion'.
    ASSERT_TRUE(sharedCallbackStateDestroyed);
}

TEST_F(InitialSyncerTest, InitialSyncerRecreatesOplogAndDropsReplicatedDatabases) {
    // We are not interested in proceeding beyond the oplog creation stage so we inject a failure
    // after setting '_storageInterfaceWorkDone.createOplogCalled' to true.
    auto oldCreateOplogFn = _storageInterface->createOplogFn;
    _storageInterface->createOplogFn = [oldCreateOplogFn](OperationContext* opCtx,
                                                          const NamespaceString& nss) {
        oldCreateOplogFn(opCtx, nss);
        return Status(ErrorCodes::OperationFailed, "oplog creation failed");
    };

    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);

    LockGuard lock(_storageInterfaceWorkDoneMutex);
    ASSERT_TRUE(_storageInterfaceWorkDone.droppedUserDBs);
    ASSERT_TRUE(_storageInterfaceWorkDone.createOplogCalled);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughGetRollbackIdScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // replSetGetRBID is the first remote command to be scheduled by the initial syncer after
    // creating the oplog collection.
    executor::RemoteCommandRequest request;
    _executorProxy->shouldFailScheduleRemoteCommandRequest =
        [&request](const executor::RemoteCommandRequest& requestToSend) {
            request = requestToSend;
            return true;
        };

    HostAndPort syncSource("localhost", 12345);
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(syncSource);
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);

    ASSERT_EQUALS("admin", request.dbname);
    assertRemoteCommandNameEquals("replSetGetRBID", request);
    ASSERT_EQUALS(syncSource, request.target);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerReturnsShutdownInProgressIfSchedulingRollbackCheckerFailedDueToExecutorShutdown) {
    // The rollback id request is sent immediately after oplog creation. We shut the task executor
    // down before returning from createOplog() to make the scheduleRemoteCommand() call for
    // replSetGetRBID fail.
    auto oldCreateOplogFn = _storageInterface->createOplogFn;
    _storageInterface->createOplogFn = [oldCreateOplogFn, this](OperationContext* opCtx,
                                                                const NamespaceString& nss) {
        auto status = oldCreateOplogFn(opCtx, nss);
        getExecutor().shutdown();
        return status;
    };

    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, _lastApplied);

    LockGuard lock(_storageInterfaceWorkDoneMutex);
    ASSERT_TRUE(_storageInterfaceWorkDone.createOplogCalled);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsRollbackCheckerOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    HostAndPort syncSource("localhost", 12345);
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(syncSource);

    ASSERT_EQUALS(InitialSyncer::State::kPreStart, initialSyncer->getState_forTest());

    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));
    ASSERT_EQUALS(InitialSyncer::State::kRunning, initialSyncer->getState_forTest());

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        ASSERT_TRUE(net->hasReadyRequests());
        auto noi = net->getNextReadyRequest();
        const auto& request = assertRemoteCommandNameEquals("replSetGetRBID", noi->getRequest());
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_EQUALS(syncSource, request.target);
        net->blackHole(noi);
    }

    ASSERT_OK(initialSyncer->shutdown());
    // Since we need to request the NetworkInterfaceMock to deliver the cancellation event,
    // the InitialSyncer has to be in a pre-completion state (ie. ShuttingDown).
    ASSERT_EQUALS(InitialSyncer::State::kShuttingDown, initialSyncer->getState_forTest());

    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(InitialSyncer::State::kComplete, initialSyncer->getState_forTest());

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughRollbackCheckerCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        assertRemoteCommandNameEquals(
            "replSetGetRBID",
            net->scheduleErrorResponse(
                Status(ErrorCodes::OperationFailed, "replSetGetRBID failed at sync source")));
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughLastOplogEntryFetcherScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // The last oplog entry fetcher is the first component that sends a find command so we reject
    // any find commands and save the request for inspection at the end of this test case.
    executor::RemoteCommandRequest request;
    _executorProxy->shouldFailScheduleRemoteCommandRequest =
        [&request](const executor::RemoteCommandRequest& requestToSend) {
            request = requestToSend;
            return "find" == requestToSend.cmdObj.firstElement().fieldNameStringData();
        };

    HostAndPort syncSource("localhost", 12345);
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(syncSource);
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);

    ASSERT_EQUALS(syncSource, request.target);
    ASSERT_EQUALS(_options.localOplogNS.db(), request.dbname);
    assertRemoteCommandNameEquals("find", request);
    ASSERT_BSONOBJ_EQ(BSON("$natural" << -1), request.cmdObj.getObjectField("sort"));
    ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughLastOplogEntryFetcherCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        assertRemoteCommandNameEquals(
            "find",
            net->scheduleErrorResponse(
                Status(ErrorCodes::OperationFailed, "find command failed at sync source")));
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsLastOplogEntryFetcherOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        ASSERT_TRUE(net->hasReadyRequests());
    }

    ASSERT_OK(initialSyncer->shutdown());
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsNoMatchingDocumentIfLastOplogEntryFetcherReturnsEmptyBatchOfDocuments) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({});
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerResendsFindCommandIfLastOplogEntryFetcherReturnsRetriableError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);

    // Base rollback ID.
    net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
    net->runReadyNetworkOperations();

    // Last oplog entry first attempt - retriable error.
    assertRemoteCommandNameEquals("find",
                                  net->scheduleErrorResponse(Status(ErrorCodes::HostNotFound, "")));
    net->runReadyNetworkOperations();

    // InitialSyncer stays active because it resends the find request for the last oplog entry.
    ASSERT_TRUE(initialSyncer->isActive());

    // Last oplog entry second attempt.
    processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsNoSuchKeyIfLastOplogEntryFetcherReturnsEntryWithMissingHash) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({BSONObj()});
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsNoSuchKeyIfLastOplogEntryFetcherReturnsEntryWithMissingTimestamp) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({BSON("h" << 1LL)});
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerPassesThroughErrorFromDataReplicatorExternalStateGetCurrentConfig) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    getExternalState()->replSetConfigResult = Status(ErrorCodes::OperationFailed, "");

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughOplogFetcherScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Make the tailable oplog query fail. Allow all other requests to be scheduled.
    executor::RemoteCommandRequest request;
    _executorProxy->shouldFailScheduleRemoteCommandRequest =
        [&request](const executor::RemoteCommandRequest& requestToSend) {
            if ("find" == requestToSend.cmdObj.firstElement().fieldNameStringData() &&
                requestToSend.cmdObj.getBoolField("tailable")) {
                request = requestToSend;
                return true;
            }
            return false;
        };

    HostAndPort syncSource("localhost", 12345);
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(syncSource);
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);

    ASSERT_EQUALS(syncSource, request.target);
    ASSERT_EQUALS(_options.localOplogNS.db(), request.dbname);
    assertRemoteCommandNameEquals("find", request);
    ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
    ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughOplogFetcherCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, _options.localOplogNS, {makeOplogEntry(1)}));
        net->runReadyNetworkOperations();

        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // Oplog tailing query.
        auto request = assertRemoteCommandNameEquals(
            "find", net->scheduleErrorResponse(Status(ErrorCodes::OperationFailed, "dead cursor")));
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->runReadyNetworkOperations();


        // OplogFetcher will shut down DatabasesCloner on error after setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _databasesClonerCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerSucceedsOnEarlyOplogFetcherCompletionIfThereAreNoOperationsToApply) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        auto request =
            assertRemoteCommandNameEquals("find",
                                          net->scheduleSuccessfulResponse(makeCursorResponse(
                                              0LL, _options.localOplogNS, {makeOplogEntry(1)})));
        ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
        net->runReadyNetworkOperations();

        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // Oplog tailing query.
        // Simulate cursor closing on sync source.
        request =
            assertRemoteCommandNameEquals("find",
                                          net->scheduleSuccessfulResponse(makeCursorResponse(
                                              0LL, _options.localOplogNS, {makeOplogEntry(1)})));
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->runReadyNetworkOperations();

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Last rollback checker replSetGetRBID command.
        assertRemoteCommandNameEquals(
            "replSetGetRBID", net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1)));
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(OplogEntry(makeOplogEntry(1)).getOpTime(),
                  unittest::assertGet(_lastApplied).opTime);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerSucceedsOnEarlyOplogFetcherCompletionIfThereAreEnoughOperationsInTheOplogBufferToReachEndTimestamp) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // Oplog tailing query.
        // Simulate cursor closing on sync source.
        auto request = assertRemoteCommandNameEquals(
            "find",
            net->scheduleSuccessfulResponse(makeCursorResponse(
                0LL,
                _options.localOplogNS,
                {makeOplogEntry(1), makeOplogEntry(2, "c"), makeOplogEntry(3, "c")})));
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->runReadyNetworkOperations();

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(3)});

        // Last rollback checker replSetGetRBID command.
        assertRemoteCommandNameEquals(
            "replSetGetRBID", net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1)));
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(OplogEntry(makeOplogEntry(3)).getOpTime(),
                  unittest::assertGet(_lastApplied).opTime);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerReturnsRemoteResultsUnavailableOnEarlyOplogFetcherCompletionIfThereAreNotEnoughOperationsInTheOplogBufferToReachEndTimestamp) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // Oplog tailing query.
        // Simulate cursor closing on sync source.
        auto request = assertRemoteCommandNameEquals(
            "find",
            net->scheduleSuccessfulResponse(makeCursorResponse(
                0LL,
                _options.localOplogNS,
                {makeOplogEntry(1), makeOplogEntry(2, "c"), makeOplogEntry(3, "c")})));
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->runReadyNetworkOperations();

        // Second last oplog entry fetcher.
        // Return an oplog entry with an optime that is more recent than what the completed
        // OplogFetcher has read from the sync source.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(4)});
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::RemoteResultsUnavailable, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerPassesThroughDatabasesClonerScheduleErrorAndCancelsOplogFetcher) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Make the listDatabases command fail. Allow all other requests to be scheduled.
    executor::RemoteCommandRequest request;
    _executorProxy->shouldFailScheduleRemoteCommandRequest =
        [&request](const executor::RemoteCommandRequest& requestToSend) {
            if ("listDatabases" == requestToSend.cmdObj.firstElement().fieldNameStringData()) {
                request = requestToSend;
                return true;
            }
            return false;
        };

    HostAndPort syncSource("localhost", 12345);
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(syncSource);
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // InitialSyncer shuts down OplogFetcher when it fails to schedule DatabasesCloner
        // so we should not expect any network requests in the queue.
        ASSERT_FALSE(net->hasReadyRequests());

        // OplogFetcher is shutting down but we still need to call runReadyNetworkOperations()
        // to deliver the cancellation status to the 'InitialSyncer::_oplogFetcherCallback'
        // callback.
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);

    ASSERT_EQUALS(syncSource, request.target);
    ASSERT_EQUALS("admin", request.dbname);
    assertRemoteCommandNameEquals("listDatabases", request);
}

TEST_F(InitialSyncerTest,
       InitialSyncerPassesThroughDatabasesClonerCallbackErrorAndCancelsOplogFetcher) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // DatabasesCloner's first remote command - listDatabases
        assertRemoteCommandNameEquals(
            "listDatabases",
            net->scheduleErrorResponse(Status(ErrorCodes::FailedToParse, "listDatabases failed")));
        net->runReadyNetworkOperations();

        // DatabasesCloner will shut down OplogFetcher on error after setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::FailedToParse, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerIgnoresLocalDatabasesWhenCloningDatabases) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // DatabasesCloner's first remote command - listDatabases
        assertRemoteCommandNameEquals(
            "listDatabases",
            net->scheduleSuccessfulResponse(makeListDatabasesResponse({"a", "local", "b"})));
        net->runReadyNetworkOperations();

        // Oplog tailing query.
        auto noi = net->getNextReadyRequest();
        auto request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // DatabasesCloner should only send listCollections requests for databases 'a' and 'b'.
        request = assertRemoteCommandNameEquals(
            "listCollections",
            net->scheduleSuccessfulResponse(
                makeCursorResponse(0LL, NamespaceString::makeListCollectionsNSS("a"), {})));
        ASSERT_EQUALS("a", request.dbname);

        request = assertRemoteCommandNameEquals(
            "listCollections",
            net->scheduleSuccessfulResponse(
                makeCursorResponse(0LL, NamespaceString::makeListCollectionsNSS("b"), {})));
        ASSERT_EQUALS("b", request.dbname);

        // After processing all the database names and returning empty lists of collections for each
        // database, data cloning should run to completion and we should expect to see a last oplog
        // entry fetcher request.
        request = assertRemoteCommandNameEquals(
            "find",
            net->scheduleSuccessfulResponse(
                makeCursorResponse(0LL, NamespaceString::makeListCollectionsNSS("b"), {})));
        ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
    }

    getExecutor().shutdown();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerIgnoresDatabaseInfoDocumentWithoutNameFieldWhenCloningDatabases) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // DatabasesCloner's first remote command - listDatabases
        assertRemoteCommandNameEquals(
            "listDatabases",
            net->scheduleSuccessfulResponse(BSON("databases" << BSON_ARRAY(BSON("name"
                                                                                << "a")
                                                                           << BSON("bad"
                                                                                   << "dbinfo")
                                                                           << BSON("name"
                                                                                   << "b"))
                                                             << "ok"
                                                             << 1)));
        net->runReadyNetworkOperations();

        // Oplog tailing query.
        auto noi = net->getNextReadyRequest();
        auto request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // DatabasesCloner should only send listCollections requests for databases 'a' and 'b'.
        request = assertRemoteCommandNameEquals(
            "listCollections",
            net->scheduleSuccessfulResponse(
                makeCursorResponse(0LL, NamespaceString::makeListCollectionsNSS("a"), {})));
        ASSERT_EQUALS("a", request.dbname);

        request = assertRemoteCommandNameEquals(
            "listCollections",
            net->scheduleSuccessfulResponse(
                makeCursorResponse(0LL, NamespaceString::makeListCollectionsNSS("b"), {})));
        ASSERT_EQUALS("b", request.dbname);

        // After processing all the database names and returning empty lists of collections for each
        // database, data cloning should run to completion and we should expect to see a last oplog
        // entry fetcher request.
        request = assertRemoteCommandNameEquals(
            "find",
            net->scheduleSuccessfulResponse(
                makeCursorResponse(0LL, NamespaceString::makeListCollectionsNSS("b"), {})));
        ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
    }

    getExecutor().shutdown();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsBothOplogFetcherAndDatabasesClonerOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});
    }

    ASSERT_OK(initialSyncer->shutdown());
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerPassesThroughSecondLastOplogEntryFetcherScheduleErrorAndCancelsOplogFetcher) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Make the second last oplog entry fetcher command fail. Allow all other requests to be
    // scheduled.
    executor::RemoteCommandRequest request;
    bool first = true;
    _executorProxy->shouldFailScheduleRemoteCommandRequest =
        [&first, &request](const executor::RemoteCommandRequest& requestToSend) {
            if ("find" == requestToSend.cmdObj.firstElement().fieldNameStringData() &&
                requestToSend.cmdObj.hasField("sort") &&
                1 == requestToSend.cmdObj.getIntField("limit")) {
                if (first) {
                    first = false;
                    return false;
                }
                request = requestToSend;
                return true;
            }
            return false;
        };

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // DatabasesCloner will shut down the OplogFetcher on failing to schedule the last entry
        // oplog fetcher after setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerPassesThroughSecondLastOplogEntryFetcherCallbackErrorAndCancelsOplogFetcher) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and move
        // on to the DatabasesCloner's request.
        auto noi = net->getNextReadyRequest();
        auto request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // Second last oplog entry fetcher.
        request = assertRemoteCommandNameEquals(
            "find",
            net->scheduleErrorResponse(
                Status(ErrorCodes::OperationFailed, "second last oplog entry fetcher failed")));
        ASSERT_TRUE(request.cmdObj.hasField("sort"));
        ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
        net->runReadyNetworkOperations();

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerCancelsBothSecondLastOplogEntryFetcherAndOplogFetcherOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        auto request = assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and move
        // on to the DatabasesCloner's request.
        auto noi = net->getNextReadyRequest();
        request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // Second last oplog entry fetcher.
        noi = net->getNextReadyRequest();
        request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.hasField("sort"));
        ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
        net->blackHole(noi);
    }

    initialSyncer->shutdown();
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerCancelsSecondLastOplogEntryFetcherOnOplogFetcherCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // Save request for OplogFetcher's oplog tailing query. This request will be canceled.
        auto noi = net->getNextReadyRequest();
        auto request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        auto oplogFetcherNetworkOperationIterator = noi;

        // Second last oplog entry fetcher.
        // Blackhole this request which will be canceled when oplog fetcher fails.
        noi = net->getNextReadyRequest();
        request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.hasField("sort"));
        ASSERT_EQUALS(1, request.cmdObj.getIntField("limit"));
        net->blackHole(noi);

        // Make oplog fetcher fail.
        net->scheduleErrorResponse(oplogFetcherNetworkOperationIterator,
                                   Status(ErrorCodes::OperationFailed, "oplog fetcher failed"));
        net->runReadyNetworkOperations();

        // _oplogFetcherCallback() will shut down the '_lastOplogEntryFetcher' after setting the
        // completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _lastOplogEntryFetcherCallbackAfterCloningData().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerReturnsTypeMismatchErrorWhenSecondLastOplogEntryFetcherReturnsMalformedDocument) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntry(1);
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and move
        // on to the DatabasesCloner's request.
        auto noi = net->getNextReadyRequest();
        auto request = noi->getRequest();
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({BSON("ts" << Timestamp(1) << "t" << 1 << "h"
                                                                  << "not a hash")});

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsOplogOutOfOrderIfStopTimestampPrecedesBeginTimestamp) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(2)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and move
        // on to the DatabasesCloner's request.
        auto noi = net->getNextReadyRequest();
        auto request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OplogOutOfOrder, _lastApplied);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerPassesThroughInsertOplogSeedDocumentErrorAfterDataCloningFinishesWithNoOperationsToApply) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    NamespaceString insertDocumentNss;
    BSONObj insertDocumentDoc;
    _storageInterface->insertDocumentFn = [&insertDocumentDoc, &insertDocumentNss](
        OperationContext*, const NamespaceString& nss, const BSONObj& doc) {
        insertDocumentNss = nss;
        insertDocumentDoc = doc;
        return Status(ErrorCodes::OperationFailed, "failed to insert oplog entry");
    };

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntry(1);
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and move
        // on to the DatabasesCloner's request.
        auto noi = net->getNextReadyRequest();
        auto request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
    ASSERT_EQUALS(_options.localOplogNS, insertDocumentNss);
    ASSERT_BSONOBJ_EQ(oplogEntry, insertDocumentDoc);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerReturnsCallbackCanceledAndDoesNotScheduleRollbackCheckerIfShutdownAfterInsertingInsertOplogSeedDocument) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    NamespaceString insertDocumentNss;
    BSONObj insertDocumentDoc;
    _storageInterface->insertDocumentFn = [initialSyncer, &insertDocumentDoc, &insertDocumentNss](
        OperationContext*, const NamespaceString& nss, const BSONObj& doc) {
        insertDocumentNss = nss;
        insertDocumentDoc = doc;
        initialSyncer->shutdown();
        return Status::OK();
    };

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntry(1);
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and move
        // on to the DatabasesCloner's request.
        auto noi = net->getNextReadyRequest();
        auto request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
    ASSERT_EQUALS(_options.localOplogNS, insertDocumentNss);
    ASSERT_BSONOBJ_EQ(oplogEntry, insertDocumentDoc);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerPassesThroughRollbackCheckerScheduleErrorAfterCloningFinishesWithNoOperationsToApply) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Make the second replSetGetRBID command fail. Allow all other requests to be scheduled.
    executor::RemoteCommandRequest request;
    bool first = true;
    _executorProxy->shouldFailScheduleRemoteCommandRequest =
        [&first, &request](const executor::RemoteCommandRequest& requestToSend) {
            if ("replSetGetRBID" == requestToSend.cmdObj.firstElement().fieldNameStringData()) {
                if (first) {
                    first = false;
                    return false;
                }
                request = requestToSend;
                return true;
            }
            return false;
        };

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntry(1);
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and move
        // on to the DatabasesCloner's request.
        auto noi = net->getNextReadyRequest();
        auto request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerPassesThroughRollbackCheckerCallbackErrorAfterCloningFinishesWithNoOperationsToApply) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntry(1);
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and move
        // on to the DatabasesCloner's request.
        auto noi = net->getNextReadyRequest();
        auto request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Last rollback checker replSetGetRBID command.
        assertRemoteCommandNameEquals(
            "replSetGetRBID",
            net->scheduleErrorResponse(
                Status(ErrorCodes::OperationFailed, "replSetGetRBID command failed")));
        net->runReadyNetworkOperations();

        // _rollbackCheckerCheckForRollbackCallback() will shut down the OplogFetcher after setting
        // the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsLastRollbackCheckerOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntry(1);
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and move
        // on to the DatabasesCloner's request.
        auto noi = net->getNextReadyRequest();
        auto request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Last rollback checker replSetGetRBID command.
        noi = net->getNextReadyRequest();
        assertRemoteCommandNameEquals("replSetGetRBID", noi->getRequest());
        net->blackHole(noi);

        // _rollbackCheckerCheckForRollbackCallback() will shut down the OplogFetcher after setting
        // the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    ASSERT_OK(initialSyncer->shutdown());
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsLastRollbackCheckerOnOplogFetcherCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntry(1);
    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(1));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // Save request for OplogFetcher's oplog tailing query. This request will be canceled.
        auto noi = net->getNextReadyRequest();
        auto request = assertRemoteCommandNameEquals("find", noi->getRequest());
        ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        auto oplogFetcherNetworkOperationIterator = noi;

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Last rollback checker replSetGetRBID command.
        noi = net->getNextReadyRequest();
        request = noi->getRequest();
        assertRemoteCommandNameEquals("replSetGetRBID", request);
        net->blackHole(noi);

        // Make oplog fetcher fail.
        net->scheduleErrorResponse(oplogFetcherNetworkOperationIterator,
                                   Status(ErrorCodes::OperationFailed, "oplog fetcher failed"));
        net->runReadyNetworkOperations();

        // _oplogFetcherCallback() will shut down the last rollback checker after setting the
        // completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _rollbackCheckerCheckForRollbackCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsUnrecoverableRollbackErrorIfSyncSourceRolledBackAfterCloningData) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto oplogEntry = makeOplogEntry(1);
    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and move
        // on to the DatabasesCloner's request.
        auto noi = net->getNextReadyRequest();
        auto request = noi->getRequest();
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Last rollback checker replSetGetRBID command.
        request = net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId + 1));
        net->runReadyNetworkOperations();
        assertRemoteCommandNameEquals("replSetGetRBID", request);
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, _lastApplied);
}

TEST_F(InitialSyncerTest, LastOpTimeShouldBeSetEvenIfNoOperationsAreAppliedAfterCloning) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_TRUE(_storageInterface->getInitialSyncFlag(opCtx.get()));

    auto oplogEntry = makeOplogEntry(1);
    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Instead of fast forwarding to DatabasesCloner completion by returning an empty list of
        // database names, we'll simulate copying a single database with a single collection on the
        // sync source.
        NamespaceString nss("a.a");
        auto request =
            net->scheduleSuccessfulResponse(makeListDatabasesResponse({nss.db().toString()}));
        assertRemoteCommandNameEquals("listDatabases", request);
        net->runReadyNetworkOperations();

        // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and move
        // on to the DatabasesCloner's request.
        auto noi = net->getNextReadyRequest();
        request = noi->getRequest();
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // listCollections for "a"
        request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, nss, {BSON("name" << nss.coll() << "options" << BSONObj())}));
        assertRemoteCommandNameEquals("listCollections", request);

        // count:a
        request = assertRemoteCommandNameEquals(
            "count", net->scheduleSuccessfulResponse(BSON("n" << 1 << "ok" << 1)));
        ASSERT_EQUALS(nss.coll(), request.cmdObj.firstElement().String());
        ASSERT_EQUALS(nss.db(), request.dbname);

        // listIndexes:a
        request = assertRemoteCommandNameEquals(
            "listIndexes",
            net->scheduleSuccessfulResponse(makeCursorResponse(
                0LL,
                NamespaceString(nss.getCommandNS()),
                {BSON("v" << OplogEntry::kOplogVersion << "key" << BSON("_id" << 1) << "name"
                          << "_id_"
                          << "ns"
                          << nss.ns())})));
        ASSERT_EQUALS(nss.coll(), request.cmdObj.firstElement().String());
        ASSERT_EQUALS(nss.db(), request.dbname);

        // find:a
        request = assertRemoteCommandNameEquals("find",
                                                net->scheduleSuccessfulResponse(makeCursorResponse(
                                                    0LL, nss, {BSON("_id" << 1 << "a" << 1)})));
        ASSERT_EQUALS(nss.coll(), request.cmdObj.firstElement().String());
        ASSERT_EQUALS(nss.db(), request.dbname);

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({oplogEntry});

        // Last rollback checker replSetGetRBID command.
        request = assertRemoteCommandNameEquals(
            "replSetGetRBID",
            net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId)));
        net->runReadyNetworkOperations();

        // Deliver cancellation to OplogFetcher.
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(OplogEntry(oplogEntry).getOpTime(), unittest::assertGet(_lastApplied).opTime);
    ASSERT_EQUALS(oplogEntry["h"].Long(), unittest::assertGet(_lastApplied).value);
    ASSERT_FALSE(_storageInterface->getInitialSyncFlag(opCtx.get()));
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughGetNextApplierBatchScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_TRUE(_storageInterface->getInitialSyncFlag(opCtx.get()));

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and move
        // on to the DatabasesCloner's request.
        auto noi = net->getNextReadyRequest();
        auto request = noi->getRequest();
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // Before processing scheduled last oplog entry fetcher response, set flag in
        // TaskExecutorMock so that InitialSyncer will fail to schedule
        // _getNextApplierBatchCallback().
        _executorProxy->shouldFailScheduleWorkRequest = []() { return true; };

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(2)});

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughSecondGetNextApplierBatchScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_TRUE(_storageInterface->getInitialSyncFlag(opCtx.get()));

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and move
        // on to the DatabasesCloner's request.
        auto noi = net->getNextReadyRequest();
        auto request = noi->getRequest();
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // Before processing scheduled last oplog entry fetcher response, set flag in
        // TaskExecutorMock so that InitialSyncer will fail to schedule second
        // _getNextApplierBatchCallback() at (now + options.getApplierBatchCallbackRetryWait).
        _executorProxy->shouldFailScheduleWorkAtRequest = []() { return true; };

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(2)});

        // _lastOplogEntryFetcherCallbackAfterCloningData() will shut down the OplogFetcher after
        // setting the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsGetNextApplierBatchOnShutdown) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_TRUE(_storageInterface->getInitialSyncFlag(opCtx.get()));

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // We do not have to respond to the OplogFetcher's oplog tailing query. Blackhole and move
        // on to the DatabasesCloner's request.
        auto noi = net->getNextReadyRequest();
        auto request = noi->getRequest();
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("tailable"));
        net->blackHole(noi);

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(2)});

        // Since we black holed OplogFetcher's find request, _getNextApplierBatch_inlock() will
        // not return any operations for us to apply, leading to _getNextApplierBatchCallback()
        // rescheduling itself at new->now() + _options.getApplierBatchCallbackRetryWait.
    }

    ASSERT_OK(initialSyncer->shutdown());
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughGetNextApplierBatchInLockError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_TRUE(_storageInterface->getInitialSyncFlag(opCtx.get()));

    // _getNextApplierBatch_inlock() returns BadValue when it gets an oplog entry with an unexpected
    // version (not OplogEntry::kOplogVersion).
    auto oplogEntry = makeOplogEntry(1);
    auto oplogEntryWithInconsistentVersion =
        makeOplogEntry(2, "i", OplogEntry::kOplogVersion + 100);

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // OplogFetcher's oplog tailing query. Return bad oplog entry that will be added to the
        // oplog buffer and processed by _getNextApplierBatch_inlock().
        auto request = assertRemoteCommandNameEquals(
            "find",
            net->scheduleSuccessfulResponse(makeCursorResponse(
                1LL, _options.localOplogNS, {oplogEntry, oplogEntryWithInconsistentVersion})));
        ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
        net->runReadyNetworkOperations();

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(2)});

        // _getNextApplierBatchCallback() will shut down the OplogFetcher after setting the
        // completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::BadValue, _lastApplied);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerReturnsEmptyBatchFromGetNextApplierBatchInLockIfRsSyncApplyStopFailPointIsEnabled) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_TRUE(_storageInterface->getInitialSyncFlag(opCtx.get()));

    // _getNextApplierBatch_inlock() returns BadValue when it gets an oplog entry with an unexpected
    // version (not OplogEntry::kOplogVersion).
    auto oplogEntry = makeOplogEntry(1);
    auto oplogEntryWithInconsistentVersion =
        makeOplogEntry(2, "i", OplogEntry::kOplogVersion + 100);

    // Enable 'rsSyncApplyStop' so that _getNextApplierBatch_inlock() returns an empty batch of
    // operations instead of a batch containing an oplog entry with a bad version.
    auto failPoint = getGlobalFailPointRegistry()->getFailPoint("rsSyncApplyStop");
    failPoint->setMode(FailPoint::alwaysOn);
    ON_BLOCK_EXIT([failPoint]() { failPoint->setMode(FailPoint::off); });

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // OplogFetcher's oplog tailing query. Return bad oplog entry that will be added to the
        // oplog buffer and processed by _getNextApplierBatch_inlock().
        auto request = net->scheduleSuccessfulResponse(makeCursorResponse(
            1LL, _options.localOplogNS, {oplogEntry, oplogEntryWithInconsistentVersion}));
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
        net->runReadyNetworkOperations();

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(2)});

        // Since the 'rsSyncApplyStop' fail point is enabled, InitialSyncer will get an empty
        // batch of operations from _getNextApplierBatch_inlock() even though the oplog buffer
        // is not empty.
    }

    // If the fail point is not working, the initial sync status will be set to BadValue (due to the
    // bad oplog entry in the oplog buffer) and shutdown() will not be able to overwrite this status
    // with CallbackCanceled.
    // Otherwise, shutdown() will cancel both the OplogFetcher and the scheduled
    // _getNextApplierBatchCallback() task. The final initial sync status will be CallbackCanceled.
    ASSERT_OK(initialSyncer->shutdown());
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughMultiApplierScheduleError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    ASSERT_TRUE(_storageInterface->getInitialSyncFlag(opCtx.get()));

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // OplogFetcher's oplog tailing query. Save for later.
        auto noi = net->getNextReadyRequest();
        auto request = noi->getRequest();
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
        auto oplogFetcherNoi = noi;

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(2)});

        // _getNextApplierBatchCallback() should have rescheduled itself.
        // We'll insert some operations in the oplog buffer so that we'll attempt to schedule
        // MultiApplier next time _getNextApplierBatchCallback() runs.
        net->scheduleSuccessfulResponse(
            oplogFetcherNoi,
            makeCursorResponse(1LL, _options.localOplogNS, {makeOplogEntry(1), makeOplogEntry(2)}));
        net->runReadyNetworkOperations();

        // Ignore OplogFetcher's getMore request.
        noi = net->getNextReadyRequest();
        request = noi->getRequest();
        assertRemoteCommandNameEquals("getMore", request);

        // Make MultiApplier::startup() fail.
        _executorProxy->shouldFailScheduleWorkRequest = []() { return true; };

        // Advance clock until _getNextApplierBatchCallback() runs.
        auto when = net->now() + _options.getApplierBatchCallbackRetryWait;
        ASSERT_EQUALS(when, net->runUntil(when));

        // _getNextApplierBatchCallback() will shut down the OplogFetcher after setting the
        // completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerPassesThroughMultiApplierCallbackError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    getExternalState()->multiApplyFn =
        [](OperationContext*, const MultiApplier::Operations&, MultiApplier::ApplyOperationFn) {
            return Status(ErrorCodes::OperationFailed, "multiApply failed");
        };
    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // OplogFetcher's oplog tailing query. Provide enough operations to trigger MultiApplier.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(1LL, _options.localOplogNS, {makeOplogEntry(1), makeOplogEntry(2)}));
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
        net->runReadyNetworkOperations();

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(2)});

        // _multiApplierCallback() will shut down the OplogFetcher after setting the completion
        // status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest, InitialSyncerCancelsGetNextApplierBatchCallbackOnOplogFetcherError) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // OplogFetcher's oplog tailing query. Save for later.
        auto noi = net->getNextReadyRequest();
        auto request = noi->getRequest();
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
        auto oplogFetcherNoi = noi;

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(2)});

        // Send error to _oplogFetcherCallback().
        net->scheduleErrorResponse(oplogFetcherNoi,
                                   Status(ErrorCodes::OperationFailed, "oplog fetcher failed"));

        // _oplogFetcherCallback() will cancel the _getNextApplierBatchCallback() task after setting
        // the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _lastApplied);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsLastAppliedOnReachingStopTimestampAfterApplyingOneBatch) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto lastOp = makeOplogEntry(2);

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // OplogFetcher's oplog tailing query. Response has enough operations to reach
        // end timestamp.
        auto request = net->scheduleSuccessfulResponse(
            makeCursorResponse(1LL, _options.localOplogNS, {makeOplogEntry(1), lastOp}));
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
        net->runReadyNetworkOperations();

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({lastOp});

        // Black hole OplogFetcher's getMore request.
        auto noi = net->getNextReadyRequest();
        request = noi->getRequest();
        assertRemoteCommandNameEquals("getMore", request);
        net->blackHole(noi);

        // Last rollback ID.
        request = net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        assertRemoteCommandNameEquals("replSetGetRBID", request);
        net->runReadyNetworkOperations();

        // _multiApplierCallback() will cancel the _getNextApplierBatchCallback() task after setting
        // the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(OplogEntry(lastOp).getOpTime(), unittest::assertGet(_lastApplied).opTime);
    ASSERT_EQUALS(lastOp["h"].Long(), unittest::assertGet(_lastApplied).value);
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsLastAppliedOnReachingStopTimestampAfterApplyingMultipleBatches) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    // To make InitialSyncer apply multiple batches, we make the third and last operation a command
    // so that it will go into a separate batch from the second operation. First operation is the
    // last fetched entry before data cloning and is not applied.
    auto lastOp = makeOplogEntry(3, "c");

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Instead of fast forwarding to DatabasesCloner completion by returning an empty list of
        // database names, we'll simulate copying a single database with a single collection on the
        // sync source.
        NamespaceString nss("a.a");
        auto request =
            net->scheduleSuccessfulResponse(makeListDatabasesResponse({nss.db().toString()}));
        assertRemoteCommandNameEquals("listDatabases", request);
        net->runReadyNetworkOperations();

        // OplogFetcher's oplog tailing query. Response has enough operations to reach
        // end timestamp.
        request = net->scheduleSuccessfulResponse(makeCursorResponse(
            1LL, _options.localOplogNS, {makeOplogEntry(1), makeOplogEntry(2), lastOp}));
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
        net->runReadyNetworkOperations();

        // listCollections for "a"
        request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, nss, {BSON("name" << nss.coll() << "options" << BSONObj())}));
        assertRemoteCommandNameEquals("listCollections", request);

        // Black hole OplogFetcher's getMore request.
        auto noi = net->getNextReadyRequest();
        request = noi->getRequest();
        assertRemoteCommandNameEquals("getMore", request);
        net->blackHole(noi);

        // count:a
        request = net->scheduleSuccessfulResponse(BSON("n" << 1 << "ok" << 1));
        assertRemoteCommandNameEquals("count", request);
        ASSERT_EQUALS(nss.coll(), request.cmdObj.firstElement().String());
        ASSERT_EQUALS(nss.db(), request.dbname);

        // listIndexes:a
        request = net->scheduleSuccessfulResponse(makeCursorResponse(
            0LL,
            NamespaceString(nss.getCommandNS()),
            {BSON("v" << OplogEntry::kOplogVersion << "key" << BSON("_id" << 1) << "name"
                      << "_id_"
                      << "ns"
                      << nss.ns())}));
        assertRemoteCommandNameEquals("listIndexes", request);
        ASSERT_EQUALS(nss.coll(), request.cmdObj.firstElement().String());
        ASSERT_EQUALS(nss.db(), request.dbname);

        // find:a
        request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, nss, {BSON("_id" << 1 << "a" << 1)}));
        assertRemoteCommandNameEquals("find", request);
        ASSERT_EQUALS(nss.coll(), request.cmdObj.firstElement().String());
        ASSERT_EQUALS(nss.db(), request.dbname);

        // Second last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({lastOp});

        // Last rollback ID.
        request = net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        assertRemoteCommandNameEquals("replSetGetRBID", request);
        net->runReadyNetworkOperations();

        // _multiApplierCallback() will cancel the _getNextApplierBatchCallback() task after setting
        // the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(OplogEntry(lastOp).getOpTime(), unittest::assertGet(_lastApplied).opTime);
    ASSERT_EQUALS(lastOp["h"].Long(), unittest::assertGet(_lastApplied).value);
}

TEST_F(
    InitialSyncerTest,
    InitialSyncerSchedulesLastOplogEntryFetcherToGetNewStopTimestampIfMissingDocumentsHaveBeenFetchedDuringMultiInitialSyncApply) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // Override DataReplicatorExternalState::_multiInitialSyncApply() so that it will also fetch a
    // missing document.
    // This forces InitialSyncer to evaluate its end timestamp for applying operations after each
    // batch.
    getExternalState()->multiApplyFn = [](OperationContext*,
                                          const MultiApplier::Operations& ops,
                                          MultiApplier::ApplyOperationFn applyOperation) {
        // 'OperationPtr*' is ignored by our overridden _multiInitialSyncApply().
        applyOperation(nullptr);
        return ops.back().getOpTime();
    };
    bool fetchCountIncremented = false;
    getExternalState()->multiInitialSyncApplyFn = [&fetchCountIncremented](
        MultiApplier::OperationPtrs*, const HostAndPort&, AtomicUInt32* fetchCount) {
        if (!fetchCountIncremented) {
            fetchCount->addAndFetch(1);
            fetchCountIncremented = true;
        }
        return Status::OK();
    };

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    // Use command for third and last operation to ensure we have two batches to apply.
    auto lastOp = makeOplogEntry(3, "c");

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Quickest path to a successful DatabasesCloner completion is to respond to the
        // listDatabases with an empty list of database names.
        assertRemoteCommandNameEquals(
            "listDatabases", net->scheduleSuccessfulResponse(makeListDatabasesResponse({})));
        net->runReadyNetworkOperations();

        // OplogFetcher's oplog tailing query. Response has enough operations to reach
        // end timestamp.
        auto request = net->scheduleSuccessfulResponse(makeCursorResponse(
            1LL, _options.localOplogNS, {makeOplogEntry(1), makeOplogEntry(2), lastOp}));
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
        net->runReadyNetworkOperations();

        // Second last oplog entry fetcher.
        // Send oplog entry with timestamp 2. InitialSyncer will update this end timestamp after
        // applying the first batch.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(2)});

        // Black hole OplogFetcher's getMore request.
        auto noi = net->getNextReadyRequest();
        request = noi->getRequest();
        assertRemoteCommandNameEquals("getMore", request);
        net->blackHole(noi);

        // Third last oplog entry fetcher.
        processSuccessfulLastOplogEntryFetcherResponse({lastOp});

        // Last rollback ID.
        request = net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        assertRemoteCommandNameEquals("replSetGetRBID", request);
        net->runReadyNetworkOperations();

        // _multiApplierCallback() will cancel the _getNextApplierBatchCallback() task after setting
        // the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(OplogEntry(lastOp).getOpTime(), unittest::assertGet(_lastApplied).opTime);
    ASSERT_EQUALS(lastOp["h"].Long(), unittest::assertGet(_lastApplied).value);

    ASSERT_TRUE(fetchCountIncremented);

    auto progress = initialSyncer->getInitialSyncProgress();
    log() << "Progress after failed initial sync attempt: " << progress;
    ASSERT_EQUALS(1, progress.getIntField("fetchedMissingDocs")) << progress;
}

TEST_F(InitialSyncerTest,
       InitialSyncerReturnsInvalidSyncSourceWhenFailInitialSyncWithBadHostFailpointIsEnabled) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    // This fail point makes chooseSyncSourceCallback fail with an InvalidSyncSource error.
    auto failPoint = getGlobalFailPointRegistry()->getFailPoint("failInitialSyncWithBadHost");
    failPoint->setMode(FailPoint::alwaysOn);
    ON_BLOCK_EXIT([failPoint]() { failPoint->setMode(FailPoint::off); });

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource, _lastApplied);
}

TEST_F(InitialSyncerTest, OplogOutOfOrderOnOplogFetchFinish) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 12345));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), maxAttempts));

    auto net = getNet();
    int baseRollbackId = 1;
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});

        // Ignore listDatabases request.
        auto noi = net->getNextReadyRequest();
        auto request = noi->getRequest();
        assertRemoteCommandNameEquals("listDatabases", request);
        net->blackHole(noi);

        // OplogFetcher's oplog tailing query.
        request = net->scheduleSuccessfulResponse(
            makeCursorResponse(1LL, _options.localOplogNS, {makeOplogEntry(1)}));
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
        net->runReadyNetworkOperations();

        // Ensure that OplogFetcher fails with an OplogOutOfOrder error by responding to the getMore
        // request with oplog entries containing the following timestamps (most recently processed
        // oplog entry has a timestamp of 1):
        //     (last=1), 5, 4
        request = net->scheduleSuccessfulResponse(makeCursorResponse(
            1LL, _options.localOplogNS, {makeOplogEntry(5), makeOplogEntry(4)}, false));
        assertRemoteCommandNameEquals("getMore", request);
        net->runReadyNetworkOperations();

        // Deliver cancellation signal to DatabasesCloner.
        net->runReadyNetworkOperations();
    }

    initialSyncer->join();
    ASSERT_EQUALS(ErrorCodes::OplogOutOfOrder, _lastApplied);
}

TEST_F(InitialSyncerTest, GetInitialSyncProgressReturnsCorrectProgress) {
    auto initialSyncer = &getInitialSyncer();
    auto opCtx = makeOpCtx();

    _syncSourceSelector->setChooseNewSyncSourceResult_forTest(HostAndPort("localhost", 27017));
    ASSERT_OK(initialSyncer->startup(opCtx.get(), 2U));

    auto net = getNet();
    int baseRollbackId = 1;

    // Play first 2 responses to ensure initial syncer has started the oplog fetcher.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Base rollback ID.
        net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});
    }

    log() << "Done playing first failed response";

    auto progress = initialSyncer->getInitialSyncProgress();
    log() << "Progress after first failed response: " << progress;
    ASSERT_EQUALS(progress.nFields(), 8) << progress;
    ASSERT_EQUALS(progress.getIntField("failedInitialSyncAttempts"), 0) << progress;
    ASSERT_EQUALS(progress.getIntField("maxFailedInitialSyncAttempts"), 2) << progress;
    ASSERT_EQUALS(progress["initialSyncStart"].type(), Date) << progress;
    ASSERT_EQUALS(progress["initialSyncOplogStart"].timestamp(), Timestamp(1, 1)) << progress;
    ASSERT_BSONOBJ_EQ(progress.getObjectField("initialSyncAttempts"), BSONObj());
    ASSERT_EQUALS(progress.getIntField("fetchedMissingDocs"), 0) << progress;
    ASSERT_EQUALS(progress.getIntField("appliedOps"), 0) << progress;
    ASSERT_BSONOBJ_EQ(progress.getObjectField("databases"), BSON("databasesCloned" << 0));

    // Play rest of the failed round of responses.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        auto request = net->scheduleErrorResponse(
            Status(ErrorCodes::FailedToParse, "fail on clone -- listDBs injected failure"));
        assertRemoteCommandNameEquals("listDatabases", request);
        net->runReadyNetworkOperations();

        // Deliver cancellation to OplogFetcher
        net->runReadyNetworkOperations();
    }

    log() << "Done playing failed responses";

    // Play the first 2 responses of the successful round of responses to ensure that the
    // initial syncer starts the oplog fetcher.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        auto when = net->now() + _options.initialSyncRetryWait;
        ASSERT_EQUALS(when, net->runUntil(when));

        // Base rollback ID.
        auto request = net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId));
        assertRemoteCommandNameEquals("replSetGetRBID", request);
        net->runReadyNetworkOperations();

        // Last oplog entry.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(1)});
    }

    log() << "Done playing first successful response";

    progress = initialSyncer->getInitialSyncProgress();
    log() << "Progress after failure: " << progress;
    ASSERT_EQUALS(progress.nFields(), 8) << progress;
    ASSERT_EQUALS(progress.getIntField("failedInitialSyncAttempts"), 1) << progress;
    ASSERT_EQUALS(progress.getIntField("maxFailedInitialSyncAttempts"), 2) << progress;
    ASSERT_EQUALS(progress["initialSyncStart"].type(), Date) << progress;
    ASSERT_EQUALS(progress["initialSyncOplogStart"].timestamp(), Timestamp(1, 1)) << progress;
    ASSERT_EQUALS(progress.getIntField("fetchedMissingDocs"), 0) << progress;
    ASSERT_EQUALS(progress.getIntField("appliedOps"), 0) << progress;
    ASSERT_BSONOBJ_EQ(progress.getObjectField("databases"), BSON("databasesCloned" << 0));

    BSONObj attempts = progress["initialSyncAttempts"].Obj();
    ASSERT_EQUALS(attempts.nFields(), 1) << attempts;
    BSONObj attempt0 = attempts["0"].Obj();
    ASSERT_EQUALS(attempt0.nFields(), 3) << attempt0;
    ASSERT_EQUALS(
        attempt0.getStringField("status"),
        std::string(
            "FailedToParse: error cloning databases: fail on clone -- listDBs injected failure"))
        << attempt0;
    ASSERT_EQUALS(attempt0["durationMillis"].type(), NumberInt) << attempt0;
    ASSERT_EQUALS(attempt0.getStringField("syncSource"), std::string("localhost:27017"))
        << attempt0;

    // Play all but last of the successful round of responses.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // listDatabases
        NamespaceString nss("a.a");
        auto request =
            net->scheduleSuccessfulResponse(makeListDatabasesResponse({nss.db().toString()}));
        assertRemoteCommandNameEquals("listDatabases", request);
        net->runReadyNetworkOperations();

        // Ignore oplog tailing query.
        request = net->scheduleSuccessfulResponse(makeCursorResponse(1LL,
                                                                     _options.localOplogNS,
                                                                     {makeOplogEntry(1),
                                                                      makeOplogEntry(2),
                                                                      makeOplogEntry(3),
                                                                      makeOplogEntry(4),
                                                                      makeOplogEntry(5),
                                                                      makeOplogEntry(6),
                                                                      makeOplogEntry(7)}));
        assertRemoteCommandNameEquals("find", request);
        ASSERT_TRUE(request.cmdObj.getBoolField("oplogReplay"));
        net->runReadyNetworkOperations();

        // listCollections for "a"
        request = net->scheduleSuccessfulResponse(
            makeCursorResponse(0LL, nss, {BSON("name" << nss.coll() << "options" << BSONObj())}));
        assertRemoteCommandNameEquals("listCollections", request);

        auto noi = net->getNextReadyRequest();
        request = noi->getRequest();
        assertRemoteCommandNameEquals("getMore", request);
        net->blackHole(noi);

        // count:a
        request = net->scheduleSuccessfulResponse(BSON("n" << 5 << "ok" << 1));
        assertRemoteCommandNameEquals("count", request);
        ASSERT_EQUALS(nss.coll(), request.cmdObj.firstElement().String());
        ASSERT_EQUALS(nss.db(), request.dbname);

        // listIndexes:a
        request = net->scheduleSuccessfulResponse(makeCursorResponse(
            0LL,
            NamespaceString(nss.getCommandNS()),
            {BSON("v" << OplogEntry::kOplogVersion << "key" << BSON("_id" << 1) << "name"
                      << "_id_"
                      << "ns"
                      << nss.ns())}));
        assertRemoteCommandNameEquals("listIndexes", request);
        ASSERT_EQUALS(nss.coll(), request.cmdObj.firstElement().String());
        ASSERT_EQUALS(nss.db(), request.dbname);

        // find:a - 5 batches
        for (int i = 1; i <= 5; ++i) {
            request = net->scheduleSuccessfulResponse(
                makeCursorResponse(i < 5 ? 2LL : 0LL, nss, {BSON("_id" << i << "a" << i)}, i == 1));
            ASSERT_EQUALS(i == 1 ? "find" : "getMore",
                          request.cmdObj.firstElement().fieldNameStringData());
            net->runReadyNetworkOperations();
        }

        // Second last oplog entry fetcher.
        // Send oplog entry with timestamp 2. InitialSyncer will update this end timestamp after
        // applying the first batch.
        processSuccessfulLastOplogEntryFetcherResponse({makeOplogEntry(7)});
    }
    log() << "Done playing all but last successful response";

    progress = initialSyncer->getInitialSyncProgress();
    log() << "Progress after all but last successful response: " << progress;
    ASSERT_EQUALS(progress.nFields(), 9) << progress;
    ASSERT_EQUALS(progress.getIntField("failedInitialSyncAttempts"), 1) << progress;
    ASSERT_EQUALS(progress.getIntField("maxFailedInitialSyncAttempts"), 2) << progress;
    ASSERT_EQUALS(progress["initialSyncOplogStart"].timestamp(), Timestamp(1, 1)) << progress;
    ASSERT_EQUALS(progress["initialSyncOplogEnd"].timestamp(), Timestamp(7, 1)) << progress;
    ASSERT_EQUALS(progress["initialSyncStart"].type(), Date) << progress;
    ASSERT_EQUALS(progress.getIntField("fetchedMissingDocs"), 0) << progress;
    // Expected applied ops to be a superset of this range: Timestamp(2,1) ... Timestamp(7,1).
    ASSERT_GREATER_THAN_OR_EQUALS(progress.getIntField("appliedOps"), 6) << progress;
    auto databasesProgress = progress.getObjectField("databases");
    ASSERT_EQUALS(1, databasesProgress.getIntField("databasesCloned")) << databasesProgress;
    auto dbProgress = databasesProgress.getObjectField("a");
    ASSERT_EQUALS(1, dbProgress.getIntField("collections")) << dbProgress;
    ASSERT_EQUALS(1, dbProgress.getIntField("clonedCollections")) << dbProgress;
    auto collectionProgress = dbProgress.getObjectField("a.a");
    ASSERT_EQUALS(
        5, collectionProgress.getIntField(CollectionCloner::Stats::kDocumentsToCopyFieldName))
        << collectionProgress;
    ASSERT_EQUALS(
        5, collectionProgress.getIntField(CollectionCloner::Stats::kDocumentsCopiedFieldName))
        << collectionProgress;
    ASSERT_EQUALS(1, collectionProgress.getIntField("indexes")) << collectionProgress;
    ASSERT_EQUALS(5, collectionProgress.getIntField("fetchedBatches")) << collectionProgress;

    attempts = progress["initialSyncAttempts"].Obj();
    ASSERT_EQUALS(attempts.nFields(), 1) << progress;
    attempt0 = attempts["0"].Obj();
    ASSERT_EQUALS(attempt0.nFields(), 3) << attempt0;
    ASSERT_EQUALS(
        attempt0.getStringField("status"),
        std::string(
            "FailedToParse: error cloning databases: fail on clone -- listDBs injected failure"))
        << attempt0;
    ASSERT_EQUALS(attempt0["durationMillis"].type(), NumberInt) << attempt0;
    ASSERT_EQUALS(attempt0.getStringField("syncSource"), std::string("localhost:27017"))
        << attempt0;

    // Play last successful response.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Last rollback ID.
        assertRemoteCommandNameEquals(
            "replSetGetRBID",
            net->scheduleSuccessfulResponse(makeRollbackCheckerResponse(baseRollbackId)));
        net->runReadyNetworkOperations();

        // _multiApplierCallback() will cancel the _getNextApplierBatchCallback() task after setting
        // the completion status.
        // We call runReadyNetworkOperations() again to deliver the cancellation status to
        // _oplogFetcherCallback().
        net->runReadyNetworkOperations();
    }

    log() << "waiting for initial sync to verify it completed OK";
    initialSyncer->join();
    ASSERT_EQUALS(OplogEntry(makeOplogEntry(7)).getOpTime(),
                  unittest::assertGet(_lastApplied).opTime);

    progress = initialSyncer->getInitialSyncProgress();
    log() << "Progress at end: " << progress;
    ASSERT_EQUALS(progress.nFields(), 11) << progress;
    ASSERT_EQUALS(progress.getIntField("failedInitialSyncAttempts"), 1) << progress;
    ASSERT_EQUALS(progress.getIntField("maxFailedInitialSyncAttempts"), 2) << progress;
    ASSERT_EQUALS(progress["initialSyncStart"].type(), Date) << progress;
    ASSERT_EQUALS(progress["initialSyncEnd"].type(), Date) << progress;
    ASSERT_EQUALS(progress["initialSyncOplogStart"].timestamp(), Timestamp(1, 1)) << progress;
    ASSERT_EQUALS(progress["initialSyncOplogEnd"].timestamp(), Timestamp(7, 1)) << progress;
    ASSERT_EQUALS(progress["initialSyncElapsedMillis"].type(), NumberInt) << progress;
    ASSERT_EQUALS(progress.getIntField("fetchedMissingDocs"), 0) << progress;
    // Expected applied ops to be a superset of this range: Timestamp(2,1) ... Timestamp(7,1).
    ASSERT_GREATER_THAN_OR_EQUALS(progress.getIntField("appliedOps"), 6) << progress;

    attempts = progress["initialSyncAttempts"].Obj();
    ASSERT_EQUALS(attempts.nFields(), 2) << attempts;

    attempt0 = attempts["0"].Obj();
    ASSERT_EQUALS(attempt0.nFields(), 3) << attempt0;
    ASSERT_EQUALS(
        attempt0.getStringField("status"),
        std::string(
            "FailedToParse: error cloning databases: fail on clone -- listDBs injected failure"))
        << attempt0;
    ASSERT_EQUALS(attempt0["durationMillis"].type(), NumberInt) << attempt0;
    ASSERT_EQUALS(attempt0.getStringField("syncSource"), std::string("localhost:27017"))
        << attempt0;

    BSONObj attempt1 = attempts["1"].Obj();
    ASSERT_EQUALS(attempt1.nFields(), 3) << attempt1;
    ASSERT_EQUALS(attempt1.getStringField("status"), std::string("OK")) << attempt1;
    ASSERT_EQUALS(attempt1["durationMillis"].type(), NumberInt) << attempt1;
    ASSERT_EQUALS(attempt1.getStringField("syncSource"), std::string("localhost:27017"))
        << attempt1;
}

}  // namespace
