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

#include <memory>

#include "mongo/client/fetcher.h"
#include "mongo/db/client.h"
#include "mongo/db/json.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/repl/base_cloner_test_fixture.h"
#include "mongo/db/repl/data_replicator.h"
#include "mongo/db/repl/data_replicator_external_state_mock.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/reporter.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/db/repl/sync_source_selector.h"
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

namespace {

using namespace mongo;
using namespace mongo::repl;

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using unittest::log;

using LockGuard = stdx::lock_guard<stdx::mutex>;
using NetworkGuard = executor::NetworkInterfaceMock::InNetworkGuard;
using ResponseStatus = executor::TaskExecutor::ResponseStatus;
using UniqueLock = stdx::unique_lock<stdx::mutex>;

struct CollectionCloneInfo {
    CollectionMockStats stats;
    CollectionBulkLoaderMock* loader = nullptr;
    Status status{ErrorCodes::NotYetInitialized, ""};
};

class SyncSourceSelectorMock : public SyncSourceSelector {
    MONGO_DISALLOW_COPYING(SyncSourceSelectorMock);

public:
    SyncSourceSelectorMock(const HostAndPort& syncSource) : _syncSource(syncSource) {}
    void clearSyncSourceBlacklist() override {}
    HostAndPort chooseNewSyncSource(const OpTime& ot) override {
        HostAndPort result = _syncSource;
        return result;
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {
        _blacklistedSource = host;
    }
    bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                const rpc::ReplSetMetadata& metadata) override {
        return false;
    }

    HostAndPort _syncSource;
    HostAndPort _blacklistedSource;
};

class DataReplicatorTest : public executor::ThreadPoolExecutorTest, public SyncSourceSelector {
public:
    DataReplicatorTest() {}

    executor::ThreadPoolMock::Options makeThreadPoolMockOptions() const override;

    /**
     * clear/reset state
     */
    void reset() {
        _setMyLastOptime = [this](const OpTime& opTime) { _myLastOpTime = opTime; };
        _myLastOpTime = OpTime();
        _syncSourceSelector.reset(new SyncSourceSelectorMock(HostAndPort("localhost", -1)));
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
                                const rpc::ReplSetMetadata& metadata) override {
        return _syncSourceSelector->shouldChangeSyncSource(currentSource, metadata);
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
        executor::TaskExecutor::ResponseStatus responseStatus(response);
        log() << "Sending response for network request:";
        log() << "     req: " << noi->getRequest().dbname << "." << noi->getRequest().cmdObj;
        log() << "     resp:" << response;

        net->scheduleResponse(noi, net->now(), responseStatus);
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

    void finishProcessingNetworkResponse() {
        getNet()->runReadyNetworkOperations();
        if (getNet()->hasReadyRequests()) {
            log() << "The network has unexpected requests to process, next req:";
            NetworkInterfaceMock::NetworkOperation req = *getNet()->getNextReadyRequest();
            log() << req.getDiagnosticString();
        }
        ASSERT_FALSE(getNet()->hasReadyRequests());
    }

    DataReplicator& getDR() {
        return *_dr;
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
        _storageInterface->createOplogFn = [this](OperationContext* txn,
                                                  const NamespaceString& nss) {
            LockGuard lock(_storageInterfaceWorkDoneMutex);
            _storageInterfaceWorkDone.createOplogCalled = true;
            return Status::OK();
        };
        _storageInterface->insertDocumentFn =
            [this](OperationContext* txn, const NamespaceString& nss, const BSONObj& doc) {
                LockGuard lock(_storageInterfaceWorkDoneMutex);
                ++_storageInterfaceWorkDone.documentsInsertedCount;
                return Status::OK();
            };
        _storageInterface->insertDocumentsFn = [this](
            OperationContext* txn, const NamespaceString& nss, const std::vector<BSONObj>& ops) {
            LockGuard lock(_storageInterfaceWorkDoneMutex);
            _storageInterfaceWorkDone.insertedOplogEntries = true;
            ++_storageInterfaceWorkDone.oplogEntriesInserted;
            return Status::OK();
        };
        _storageInterface->dropCollFn = [this](OperationContext* txn, const NamespaceString& nss) {
            LockGuard lock(_storageInterfaceWorkDoneMutex);
            _storageInterfaceWorkDone.droppedCollections.push_back(nss.ns());
            return Status::OK();
        };
        _storageInterface->dropUserDBsFn = [this](OperationContext* txn) {
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

        _myLastOpTime = OpTime({3, 0}, 1);

        DataReplicatorOptions options;
        options.initialSyncRetryWait = Milliseconds(0);
        options.getMyLastOptime = [this]() { return _myLastOpTime; };
        options.setMyLastOptime = [this](const OpTime& opTime) { _setMyLastOptime(opTime); };
        options.getSlaveDelay = [this]() { return Seconds(0); };
        options.syncSourceSelector = this;

        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.poolName = "replication";
        threadPoolOptions.minThreads = 1U;
        threadPoolOptions.maxThreads = 1U;
        threadPoolOptions.onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName.c_str());
        };

        auto dataReplicatorExternalState = stdx::make_unique<DataReplicatorExternalStateMock>();
        dataReplicatorExternalState->taskExecutor = &getExecutor();
        dataReplicatorExternalState->dbWorkThreadPool = &getDbWorkThreadPool();
        dataReplicatorExternalState->currentTerm = 1LL;
        dataReplicatorExternalState->lastCommittedOpTime = _myLastOpTime;
        {
            ReplicaSetConfig config;
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
            dataReplicatorExternalState->replSetConfig = config;
        }
        _externalState = dataReplicatorExternalState.get();


        try {
            _dr.reset(new DataReplicator(
                options, std::move(dataReplicatorExternalState), _storageInterface.get()));
            _dr->setScheduleDbWorkFn_forTest(
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
        _dr.reset();
        _dbWorkThreadPool->join();
        _dbWorkThreadPool.reset();
        _storageInterface.reset();

        // tearDown() destroys the task executor which was referenced by the data replicator.
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


    DataReplicatorOptions::SetMyLastOptimeFn _setMyLastOptime;
    OpTime _myLastOpTime;
    std::unique_ptr<SyncSourceSelector> _syncSourceSelector;
    std::unique_ptr<StorageInterfaceMock> _storageInterface;
    std::unique_ptr<OldThreadPool> _dbWorkThreadPool;
    std::map<NamespaceString, CollectionMockStats> _collectionStats;
    std::map<NamespaceString, CollectionCloneInfo> _collections;

private:
    DataReplicatorExternalStateMock* _externalState;
    std::unique_ptr<DataReplicator> _dr;
    bool _executorThreadShutdownComplete = false;
};

executor::ThreadPoolMock::Options DataReplicatorTest::makeThreadPoolMockOptions() const {
    executor::ThreadPoolMock::Options options;
    options.onCreateThread = []() { Client::initThread("DataReplicatorTest"); };
    return options;
}

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

TEST_F(DataReplicatorTest, CreateDestroy) {}

// Used to run a Initial Sync in a separate thread, to avoid blocking test execution.
class InitialSyncBackgroundRunner {
public:
    InitialSyncBackgroundRunner(DataReplicator* dr, std::size_t maxAttempts)
        : _dr(dr), _maxAttempts(maxAttempts) {}

    ~InitialSyncBackgroundRunner() {
        if (_thread) {
            _thread->join();
        }
    }

    // Could block if initial sync has not finished.
    StatusWith<OpTimeWithHash> getResult(NetworkInterfaceMock* net) {
        while (!isDone()) {
            NetworkGuard guard(net);
            //            if (net->hasReadyRequests()) {
            net->runReadyNetworkOperations();
            //            }
        }
        _thread->join();
        _thread.reset();

        LockGuard lk(_mutex);
        return _result;
    }

    bool isDone() {
        LockGuard lk(_mutex);
        return (_result.getStatus().code() != ErrorCodes::NotYetInitialized);
    }

    bool isActive() {
        return (_dr && _dr->getState() == DataReplicatorState::InitialSync) && !isDone();
    }

    void run() {
        UniqueLock lk(_mutex);
        _thread.reset(new stdx::thread(stdx::bind(&InitialSyncBackgroundRunner::_run, this)));
        _condVar.wait(lk);
    }

    BSONObj getInitialSyncProgress() {
        return _dr->getInitialSyncProgress();
    }

private:
    void _run() {
        setThreadName("InitialSyncRunner");
        Client::initThreadIfNotAlready();
        auto txn = getGlobalServiceContext()->makeOperationContext(&cc());

        // Synchonize this thread starting with the call in run() above.
        UniqueLock lk(_mutex);
        _condVar.notify_all();
        lk.unlock();

        auto result = _dr->doInitialSync(txn.get(), _maxAttempts);  // blocking

        lk.lock();
        _result = result;
    }

    stdx::mutex _mutex;  // protects _result.
    StatusWith<OpTimeWithHash> _result{ErrorCodes::NotYetInitialized, "InitialSync not started."};

    DataReplicator* _dr;
    const std::size_t _maxAttempts;
    std::unique_ptr<stdx::thread> _thread;
    stdx::condition_variable _condVar;
};

bool isOplogGetMore(const NetworkInterfaceMock::NetworkOperationIterator& noi) {
    const RemoteCommandRequest& req = noi->getRequest();
    const auto parsedGetMoreStatus = GetMoreRequest::parseFromBSON(req.dbname, req.cmdObj);
    if (!parsedGetMoreStatus.isOK()) {
        return false;
    }
    const auto getMoreReq = parsedGetMoreStatus.getValue();
    return (getMoreReq.nss.isOplog() && getMoreReq.cursorid == 1LL);
}

// Should match this: { killCursors: "oplog.rs", cursors: [ 1 ] }
bool isOplogKillCursor(const NetworkInterfaceMock::NetworkOperationIterator& noi) {
    const BSONObj reqBSON = noi->getRequest().cmdObj;
    const auto nsElem = reqBSON["killCursors"];
    const auto isOplogNS =
        nsElem && NamespaceString{"local.oplog.rs"}.coll().equalCaseInsensitive(nsElem.str());
    if (isOplogNS) {
        const auto cursorsVector = reqBSON["cursors"].Array();
        auto hasCursorId = false;
        std::for_each(
            cursorsVector.begin(), cursorsVector.end(), [&hasCursorId](const BSONElement& elem) {
                if (elem.safeNumberLong() == 1LL) {
                    hasCursorId = true;
                }
            });
        return isOplogNS && hasCursorId;
    }
    return false;
}

class InitialSyncTest : public DataReplicatorTest {
public:
    using Responses = std::vector<std::pair<std::string, BSONObj>>;
    InitialSyncTest(){};

protected:
    void setResponses(Responses resps) {
        _responses = resps;
    }

    void startSync(std::size_t maxAttempts) {
        DataReplicator* dr = &(getDR());
        _isbr.reset(new InitialSyncBackgroundRunner(dr, maxAttempts));
        _isbr->run();
    }

    void playResponses() {
        NetworkInterfaceMock* net = getNet();
        int processedRequests(0);
        const int expectedResponses(_responses.size());

        Date_t lastLog{Date_t::now()};
        while (true) {
            if (_isbr && _isbr->isDone()) {
                log() << "There are " << (expectedResponses - processedRequests)
                      << " responses left which were unprocessed.";
                return;
            }

            NetworkGuard guard(net);

            if (!net->hasReadyRequests()) {
                net->runReadyNetworkOperations();
                continue;
            }

            auto noi = net->getNextReadyRequest();
            if (isOplogGetMore(noi)) {
                // process getmore requests from the oplog fetcher
                int c = int(numGetMoreOplogEntries + 2);
                lastGetMoreOplogEntry = BSON("ts" << Timestamp(Seconds(c), 1) << "h" << 1LL << "ns"
                                                  << "test.a"
                                                  << "v"
                                                  << OplogEntry::kOplogVersion
                                                  << "op"
                                                  << "i"
                                                  << "o"
                                                  << BSON("_id" << c));
                ++numGetMoreOplogEntries;
                mongo::CursorId cursorId =
                    numGetMoreOplogEntries == numGetMoreOplogEntriesMax ? 0 : 1LL;
                auto respBSON =
                    BSON("ok" << 1 << "cursor" << BSON("id" << cursorId << "ns"
                                                            << "local.oplog.rs"
                                                            << "nextBatch"
                                                            << BSON_ARRAY(lastGetMoreOplogEntry)));
                net->scheduleResponse(
                    noi,
                    net->now(),
                    ResponseStatus(RemoteCommandResponse(respBSON, BSONObj(), Milliseconds(10))));

                log() << "Sending response for getMore network request:";
                log() << "     req: " << noi->getRequest().dbname << "."
                      << noi->getRequest().cmdObj;
                log() << "     resp:" << respBSON;

                if ((Date_t::now() - lastLog) > Seconds(1)) {
                    lastLog = Date_t::now();
                    log() << "processing oplog getmore, net:" << net->getDiagnosticString();
                    net->logQueues();
                }
                net->runReadyNetworkOperations();
                continue;
            } else if (isOplogKillCursor(noi)) {
                auto respBSON = BSON("ok" << 1.0);
                log() << "processing oplog killcursors req, net:" << net->getDiagnosticString();
                net->scheduleResponse(
                    noi,
                    net->now(),
                    ResponseStatus(RemoteCommandResponse(respBSON, BSONObj(), Milliseconds(10))));
                net->runReadyNetworkOperations();
                continue;
            }

            const BSONObj reqBSON = noi->getRequest().cmdObj;
            const BSONElement cmdElem = reqBSON.firstElement();
            auto cmdName = cmdElem.fieldNameStringData();
            auto expectedName = _responses[processedRequests].first;
            auto response = _responses[processedRequests].second;
            ASSERT(_responses[processedRequests].first == "" ||
                   cmdName.equalCaseInsensitive(expectedName))
                << "ERROR: response #" << processedRequests + 1 << ", expected '" << expectedName
                << "' command but the request was actually: '" << noi->getRequest().cmdObj
                << "' for resp: " << response;

            // process fixed set of responses
            log() << "Sending response for network request:";
            log() << "     req: " << noi->getRequest().dbname << "." << noi->getRequest().cmdObj;
            log() << "     resp:" << response;
            net->scheduleResponse(
                noi,
                net->now(),
                ResponseStatus(RemoteCommandResponse(response, BSONObj(), Milliseconds(10))));

            if ((Date_t::now() - lastLog) > Seconds(1)) {
                lastLog = Date_t();
                log() << net->getDiagnosticString();
                net->logQueues();
            }
            net->runReadyNetworkOperations();

            guard.dismiss();
            if (++processedRequests >= expectedResponses) {
                log() << "done processing expected requests ";
                break;  // once we have processed all requests, continue;
            }
        }
    }

    void verifySync(NetworkInterfaceMock* net, Status s = Status::OK()) {
        verifySync(net, s.code());
    }

    void verifySync(NetworkInterfaceMock* net, ErrorCodes::Error code) {
        // Check result
        const auto status = _isbr->getResult(net).getStatus();
        ASSERT_EQ(status.code(), code) << "status codes differ, status: " << status;
    }

    BSONObj getInitialSyncProgress() {
        return _isbr->getInitialSyncProgress();
    }

    // Generate at least one getMore response.
    std::size_t numGetMoreOplogEntries = 0;
    std::size_t numGetMoreOplogEntriesMax = 1;
    BSONObj lastGetMoreOplogEntry;

private:
    void tearDown() override;

    Responses _responses;
    std::unique_ptr<InitialSyncBackgroundRunner> _isbr{nullptr};
};

void InitialSyncTest::tearDown() {
    DataReplicatorTest::tearDownExecutorThread();
    _isbr.reset();
    DataReplicatorTest::tearDown();
}

TEST_F(InitialSyncTest, ShutdownImmediatelyAfterStartup) {
    startSync(1);
    auto txn = makeOpCtx();
    ASSERT_OK(getDR().shutdown());
    getExecutor().shutdown();
    verifySync(getNet(), ErrorCodes::ShutdownInProgress);
}

TEST_F(InitialSyncTest, Complete) {
    /**
     * Initial Sync will issue these query/commands
     *   - replSetGetRBID
     *   - startTS = oplog.rs->find().sort({$natural:-1}).limit(-1).next()["ts"]
     *   - listDatabases (foreach db do below)
     *   -- cloneDatabase (see DatabaseCloner tests).
     *   - endTS = oplog.rs->find().sort({$natural:-1}).limit(-1).next()["ts"]
     *   - ops = oplog.rs->find({ts:{$gte: startTS}}) (foreach op)
     *   -- if local doc is missing, getCollection(op.ns).findOne(_id:op.o2._id)
     *   - if any retries were done in the previous loop, endTS query again for minvalid
     *   - replSetGetRBID
     *
     */

    auto lastOpAfterClone = BSON(
        "ts" << Timestamp(Seconds(8), 1U) << "h" << 1LL << "v" << OplogEntry::kOplogVersion << "ns"
             << ""
             << "op"
             << "i"
             << "o"
             << BSON("_id" << 5 << "a" << 2));

    const Responses responses = {
        {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},
        // get latest oplog ts
        {"find",
         fromjson(
             str::stream() << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                              "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                           << OplogEntry::kOplogVersion
                           << ", op:'i', o:{_id:1, a:1}}]}}")},
        // oplog fetcher find
        {"find",
         fromjson(
             str::stream() << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs', firstBatch:["
                              "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                           << OplogEntry::kOplogVersion
                           << ", op:'i', o:{_id:1, a:1}}]}}")},
        // Clone Start
        // listDatabases
        {"listDatabases", fromjson("{ok:1, databases:[{name:'a'}]}")},
        // listCollections for "a"
        {"listCollections",
         fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listCollections', firstBatch:["
                  "{name:'a', options:{}} "
                  "]}}")},
        // count:a
        {"count", BSON("n" << 1 << "ok" << 1)},
        // listIndexes:a
        {
            "listIndexes",
            fromjson(str::stream()
                     << "{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listIndexes.a', firstBatch:["
                        "{v:"
                     << OplogEntry::kOplogVersion
                     << ", key:{_id:1}, name:'_id_', ns:'a.a'}]}}")},
        // find:a
        {"find",
         fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.a', firstBatch:["
                  "{_id:1, a:1} "
                  "]}}")},
        // Clone Done
        // get latest oplog ts
        {"find", BaseClonerTest::createCursorResponse(0, BSON_ARRAY(lastOpAfterClone))},
        {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},
        // Applier starts ...
    };

    // Initial sync flag should not be set before starting.
    auto txn = makeOpCtx();
    ASSERT_FALSE(getStorage().getInitialSyncFlag(txn.get()));

    startSync(1);

    // Play first response to ensure data replicator has entered initial sync state.
    setResponses({responses.begin(), responses.begin() + 1});
    numGetMoreOplogEntriesMax = responses.size();
    playResponses();

    // Initial sync flag should be set.
    ASSERT_TRUE(getStorage().getInitialSyncFlag(txn.get()));

    // Play rest of the responses after checking initial sync flag.
    setResponses({responses.begin() + 1, responses.end()});
    playResponses();
    log() << "done playing last responses";

    log() << "waiting for initial sync to verify it completed OK";
    verifySync(getNet());

    log() << "doing asserts";
    {
        LockGuard lock(_storageInterfaceWorkDoneMutex);
        ASSERT_TRUE(_storageInterfaceWorkDone.droppedUserDBs);
        ASSERT_TRUE(_storageInterfaceWorkDone.createOplogCalled);
        ASSERT_EQ(0, _storageInterfaceWorkDone.oplogEntriesInserted);
    }

    log() << "checking initial sync flag isn't set.";
    // Initial sync flag should not be set after completion.
    ASSERT_FALSE(getStorage().getInitialSyncFlag(txn.get()));

    // getMore responses are generated by playResponses().
    ASSERT_EQUALS(OplogEntry(lastOpAfterClone).getOpTime(), _myLastOpTime);
}

TEST_F(InitialSyncTest, LastOpTimeShouldBeSetEvenIfNoOperationsAreAppliedAfterCloning) {
    const Responses responses =
        {
            {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},
            // get latest oplog ts
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, a:1}}]}}")},
            // oplog fetcher find
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, a:1}}]}}")},
            // Clone Start
            // listDatabases
            {"listDatabases", fromjson("{ok:1, databases:[{name:'a'}]}")},
            // listCollections for "a"
            {"listCollections",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listCollections', firstBatch:["
                      "{name:'a', options:{}} "
                      "]}}")},
            // count:a
            {"count", BSON("n" << 1 << "ok" << 1)},
            // listIndexes:a
            {"listIndexes",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listIndexes.a', firstBatch:["
                         "{v:"
                      << OplogEntry::kOplogVersion
                      << ", key:{_id:1}, name:'_id_', ns:'a.a'}]}}")},
            // find:a
            {"find",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.a', firstBatch:["
                      "{_id:1, a:1} "
                      "]}}")},
            // Clone Done
            // get latest oplog ts
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(1,1), h:NumberLong(1), ns:'b.c', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, c:1}}]}}")},
            {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},
        };

    // Initial sync flag should not be set before starting.
    auto txn = makeOpCtx();
    ASSERT_FALSE(getStorage().getInitialSyncFlag(txn.get()));

    startSync(1);

    // Play first response to ensure data replicator has entered initial sync state.
    setResponses({responses.begin(), responses.begin() + 1});
    playResponses();

    // Initial sync flag should be set.
    ASSERT_TRUE(getStorage().getInitialSyncFlag(txn.get()));

    // Play rest of the responses after checking initial sync flag.
    setResponses({responses.begin() + 1, responses.end()});
    playResponses();
    log() << "done playing last responses";

    log() << "waiting for initial sync to verify it completed OK";
    verifySync(getNet());

    log() << "doing asserts";
    {
        LockGuard lock(_storageInterfaceWorkDoneMutex);
        ASSERT_TRUE(_storageInterfaceWorkDone.droppedUserDBs);
        ASSERT_TRUE(_storageInterfaceWorkDone.createOplogCalled);
        ASSERT_EQ(1, _storageInterfaceWorkDone.oplogEntriesInserted);
    }

    log() << "checking initial sync flag isn't set.";
    // Initial sync flag should not be set after completion.
    ASSERT_FALSE(getStorage().getInitialSyncFlag(txn.get()));

    ASSERT_EQUALS(OpTime(Timestamp(1, 1), OpTime::kUninitializedTerm), _myLastOpTime);
}

TEST_F(InitialSyncTest, Failpoint) {
    auto failPoint = getGlobalFailPointRegistry()->getFailPoint("failInitialSyncWithBadHost");
    failPoint->setMode(FailPoint::alwaysOn);
    ON_BLOCK_EXIT([failPoint]() { failPoint->setMode(FailPoint::off); });

    Timestamp time1(100, 1);
    OpTime opTime1(time1, OpTime::kInitialTerm);
    _myLastOpTime = opTime1;

    startSync(1);

    verifySync(getNet(), ErrorCodes::InvalidSyncSource);
}

TEST_F(InitialSyncTest, FailsOnClone) {
    const Responses responses = {
        {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},
        // get latest oplog ts
        {"find",
         fromjson(
             str::stream() << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                              "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                           << OplogEntry::kOplogVersion
                           << ", op:'i', o:{_id:1, a:1}}]}}")},
        // oplog fetcher find
        {"find",
         fromjson(
             str::stream() << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs', firstBatch:["
                              "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                           << OplogEntry::kOplogVersion
                           << ", op:'i', o:{_id:1, a:1}}]}}")},
        // Clone Start
        // listDatabases
        {"listDatabases",
         fromjson(
             str::stream() << "{ok:0, errmsg:'fail on clone -- listDBs injected failure', code: "
                           << int(ErrorCodes::FailedToParse)
                           << "}")},
        // rollback checker.
        {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},

    };
    startSync(1);
    setResponses(responses);
    playResponses();
    verifySync(getNet(), ErrorCodes::FailedToParse);
}

TEST_F(InitialSyncTest, FailOnRollback) {
    const Responses responses =
        {
            // get rollback id
            {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},
            // get latest oplog ts
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, a:1}}]}}")},
            // oplog fetcher find
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, a:1}}]}}")},
            // Clone Start
            // listDatabases
            {"listDatabases", fromjson("{ok:1, databases:[{name:'a'}]}")},
            // listCollections for "a"
            {"listCollections",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listCollections', firstBatch:["
                      "{name:'a', options:{}} "
                      "]}}")},
            // count:a
            {"count", BSON("n" << 1 << "ok" << 1)},
            // listIndexes:a
            {"listIndexes",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listIndexes.a', firstBatch:["
                         "{v:"
                      << OplogEntry::kOplogVersion
                      << ", key:{_id:1}, name:'_id_', ns:'a.a'}]}}")},
            // find:a
            {"find",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.a', firstBatch:["
                      "{_id:1, a:1} "
                      "]}}")},
            // Clone Done
            // get latest oplog ts
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(2,2), h:NumberLong(1), ns:'b.c', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, c:1}}]}}")},
            // Applier starts ...
            // check for rollback
            {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:2}")},
        };

    startSync(1);
    numGetMoreOplogEntriesMax = responses.size();
    setResponses(responses);
    playResponses();
    verifySync(getNet(), ErrorCodes::UnrecoverableRollbackError);
}

TEST_F(InitialSyncTest, DataReplicatorPassesThroughRollbackCheckerScheduleError) {
    const Responses responses =
        {
            // get rollback id
            {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},
            // get latest oplog ts
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, a:1}}]}}")},
            // oplog fetcher find
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, a:1}}]}}")},
            // Clone Start
            // listDatabases
            {"listDatabases", fromjson("{ok:1, databases:[{name:'a'}]}")},
            // listCollections for "a"
            {"listCollections",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listCollections', firstBatch:["
                      "{name:'a', options:{}} "
                      "]}}")},
            // count:a
            {"count", BSON("n" << 1 << "ok" << 1)},
            // listIndexes:a
            {"listIndexes",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listIndexes.a', firstBatch:["
                         "{v:"
                      << OplogEntry::kOplogVersion
                      << ", key:{_id:1}, name:'_id_', ns:'a.a'}]}}")},
            // find:a
            {"find",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.a', firstBatch:["
                      "{_id:1, a:1} "
                      "]}}")},
            // Clone Done
            // get latest oplog ts
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(2,2), h:NumberLong(1), ns:'b.c', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, c:1}}]}}")},
            // Response to replSetGetRBID request is left out so that we can cancel the request by
            // shutting the executor down.
        };

    startSync(1);
    numGetMoreOplogEntriesMax = responses.size();
    setResponses(responses);
    playResponses();
    getExecutor().shutdown();
    verifySync(getNet(), ErrorCodes::CallbackCanceled);
}

TEST_F(InitialSyncTest, DataReplicatorPassesThroughOplogFetcherFailure) {
    const Responses responses = {
        {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},
        // get latest oplog ts
        {"find",
         fromjson(
             str::stream() << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                              "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                           << OplogEntry::kOplogVersion
                           << ", op:'i', o:{_id:1, a:1}}]}}")},
        // oplog fetcher find
        {"find",
         fromjson(
             str::stream() << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs', firstBatch:["
                              "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                           << OplogEntry::kOplogVersion
                           << ", op:'i', o:{_id:1, a:1}}]}}")},
    };

    startSync(1);

    setResponses(responses);
    playResponses();
    log() << "done playing responses - oplog fetcher is active";

    {
        auto net = getNet();
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        ASSERT_TRUE(net->hasReadyRequests());
        auto noi = net->getNextReadyRequest();
        // Blackhole requests until we see a getMore.
        while (!isOplogGetMore(noi)) {
            log() << "Blackholing non-getMore request: " << noi->getRequest();
            net->blackHole(noi);
            ASSERT_TRUE(net->hasReadyRequests());
            noi = net->getNextReadyRequest();
        }
        log() << "Sending error response to getMore";
        net->scheduleErrorResponse(noi, {ErrorCodes::OperationFailed, "dead cursor"});
        net->runReadyNetworkOperations();
    }

    verifySync(getNet(), ErrorCodes::OperationFailed);
}

TEST_F(InitialSyncTest, OplogOutOfOrderOnOplogFetchFinish) {
    const Responses responses =
        {
            {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},
            // get latest oplog ts
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, a:1}}]}}")},
            // oplog fetcher find
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, a:1}}]}}")},
            // Clone Start
            // listDatabases
            {"listDatabases", fromjson("{ok:1, databases:[{name:'a'}]}")},
            // listCollections for "a"
            {"listCollections",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listCollections', firstBatch:["
                      "{name:'a', options:{}} "
                      "]}}")},
            // count:a
            {"count", BSON("n" << 1 << "ok" << 1)},
            // listIndexes:a
            {"listIndexes",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listIndexes.a', firstBatch:["
                         "{v:"
                      << OplogEntry::kOplogVersion
                      << ", key:{_id:1}, name:'_id_', ns:'a.a'}]}}")},
            // find:a - first batch
            {"find",
             fromjson("{ok:1, cursor:{id:NumberLong(2), ns:'a.a', firstBatch:["
                      "{_id:1, a:1} "
                      "]}}")},
            // getMore:a - second batch
            {"getMore",
             fromjson("{ok:1, cursor:{id:NumberLong(2), ns:'a.a', nextBatch:["
                      "{_id:2, a:2} "
                      "]}}")},
            // getMore:a - third batch
            {"getMore",
             fromjson("{ok:1, cursor:{id:NumberLong(2), ns:'a.a', nextBatch:["
                      "{_id:3, a:3} "
                      "]}}")},
            // getMore:a - last batch
            {"getMore",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.a', nextBatch:["
                      "{_id:4, a:4} "
                      "]}}")},
            // Clone Done
            // get latest oplog ts
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(7,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:5, a:2}}]}}")},
            {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},
            // Applier starts ...
        };

    startSync(1);

    numGetMoreOplogEntriesMax = responses.size();
    setResponses({responses.begin(), responses.end() - 4});
    playResponses();
    log() << "done playing first responses";

    // This variable is used for the reponse timestamps. Setting it to 0 will make the oplog
    // entries come out of order.
    numGetMoreOplogEntries = 0;
    setResponses({responses.end() - 4, responses.end()});
    playResponses();
    log() << "done playing second responses";
    verifySync(getNet(), ErrorCodes::OplogOutOfOrder);
}

TEST_F(InitialSyncTest, InitialSyncStateIsResetAfterFailure) {
    const Responses responses =
        {
            {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},
            // get latest oplog ts
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, a:1}}]}}")},
            // oplog fetcher find
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, a:1}}]}}")},
            // Clone Start
            // listDatabases
            {"listDatabases", fromjson("{ok:1, databases:[{name:'a'}]}")},
            // listCollections for "a"
            {"listCollections",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listCollections', firstBatch:["
                      "{name:'a', options:{}} "
                      "]}}")},
            // count:a
            {"count", BSON("n" << 1 << "ok" << 1)},
            // listIndexes:a
            {"listIndexes",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listIndexes.a', firstBatch:["
                         "{v:"
                      << OplogEntry::kOplogVersion
                      << ", key:{_id:1}, name:'_id_', ns:'a.a'}]}}")},
            // find:a
            {"find",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.a', firstBatch:["
                      "{_id:1, a:1} "
                      "]}}")},
            // Clone Done
            // get latest oplog ts
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(7,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:5, a:2}}]}}")},
            {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:2}")},
            // Applier starts ...
        };

    startSync(2);

    numGetMoreOplogEntriesMax = responses.size();
    setResponses(responses);
    playResponses();
    log() << "done playing first responses";

    // Play first response again to ensure data replicator has entered initial sync state.
    setResponses({responses.begin(), responses.begin() + 1});
    playResponses();
    log() << "done playing first response of second round of responses";

    auto dr = &getDR();
    ASSERT_TRUE(dr->getState() == DataReplicatorState::InitialSync) << ", state: "
                                                                    << dr->getDiagnosticString();
    ASSERT_EQUALS(dr->getLastFetched(), OpTimeWithHash());
    ASSERT_EQUALS(dr->getLastApplied(), OpTimeWithHash());

    setResponses({responses.begin() + 1, responses.end()});
    playResponses();
    log() << "done playing second round of responses";
    verifySync(getNet(), ErrorCodes::UnrecoverableRollbackError);
}

TEST_F(InitialSyncTest, GetInitialSyncProgressReturnsCorrectProgress) {
    const Responses failedResponses = {
        {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},
        // get latest oplog ts
        {"find",
         fromjson(
             str::stream() << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                              "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                           << OplogEntry::kOplogVersion
                           << ", op:'i', o:{_id:1, a:1}}]}}")},
        // oplog fetcher find
        {"find",
         fromjson(
             str::stream() << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs', firstBatch:["
                              "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                           << OplogEntry::kOplogVersion
                           << ", op:'i', o:{_id:1, a:1}}]}}")},
        // Clone Start
        // listDatabases
        {"listDatabases",
         fromjson("{ok:0, errmsg:'fail on clone -- listDBs injected failure', code:9}")},
    };

    const Responses successfulResponses =
        {
            {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},
            // get latest oplog ts
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, a:1}}]}}")},
            // oplog fetcher find
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, a:1}}]}}")},
            // Clone Start
            // listDatabases
            {"listDatabases", fromjson("{ok:1, databases:[{name:'a'}]}")},
            // listCollections for "a"
            {"listCollections",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listCollections', firstBatch:["
                      "{name:'a', options:{}} "
                      "]}}")},
            // count:a
            {"count", BSON("n" << 5 << "ok" << 1)},
            // listIndexes:a
            {"listIndexes",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listIndexes.a', firstBatch:["
                         "{v:"
                      << OplogEntry::kOplogVersion
                      << ", key:{_id:1}, name:'_id_', ns:'a.a'}]}}")},
            // find:a - first batch
            {"find",
             fromjson("{ok:1, cursor:{id:NumberLong(2), ns:'a.a', firstBatch:["
                      "{_id:1, a:1} "
                      "]}}")},
            // getMore:a - second batch
            {"getMore",
             fromjson("{ok:1, cursor:{id:NumberLong(2), ns:'a.a', nextBatch:["
                      "{_id:2, a:2} "
                      "]}}")},
            // getMore:a - third batch
            {"getMore",
             fromjson("{ok:1, cursor:{id:NumberLong(2), ns:'a.a', nextBatch:["
                      "{_id:3, a:3} "
                      "]}}")},
            // getMore:a - fourth batch
            {"getMore",
             fromjson("{ok:1, cursor:{id:NumberLong(2), ns:'a.a', nextBatch:["
                      "{_id:3, a:3} "
                      "]}}")},
            // getMore:a - last batch
            {"getMore",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.a', nextBatch:["
                      "{_id:4, a:4} "
                      "]}}")},
            // Clone Done
            // get latest oplog ts
            // This is a testing-only side effect of using playResponses. We may end up generating
            // getMore responses past this timestamp 7.
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(7,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:5, a:2}}]}}")},
            {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},
            // Applier starts ...
        };

    startSync(2);

    // Play first 2 responses to ensure data replicator has started the oplog fetcher.
    setResponses({failedResponses.begin(), failedResponses.begin() + 3});
    numGetMoreOplogEntriesMax = failedResponses.size() + successfulResponses.size();
    playResponses();
    log() << "Done playing first failed response";

    auto progress = getInitialSyncProgress();
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
    setResponses({failedResponses.begin() + 3, failedResponses.end()});
    playResponses();
    log() << "Done playing failed responses";

    // Play the first 3 responses of the successful round of responses to ensure that the
    // data replicator starts the oplog fetcher.
    setResponses({successfulResponses.begin(), successfulResponses.begin() + 3});
    numGetMoreOplogEntries = 0;
    playResponses();
    log() << "Done playing first successful response";

    progress = getInitialSyncProgress();
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
    ASSERT_EQUALS(attempt0.getStringField("status"),
                  std::string("FailedToParse: fail on clone -- listDBs injected failure"))
        << attempt0;
    ASSERT_EQUALS(attempt0["durationMillis"].type(), NumberInt) << attempt0;
    ASSERT_EQUALS(attempt0.getStringField("syncSource"), std::string("localhost:27017"))
        << attempt0;

    // Play all but last of the successful round of responses.
    setResponses({successfulResponses.begin() + 3, successfulResponses.end() - 1});
    // Reset getMore counter because the data replicator starts a new oplog tailing query.
    numGetMoreOplogEntries = 0;
    playResponses();
    log() << "Done playing all but last successful response";

    progress = getInitialSyncProgress();
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
    ASSERT_EQUALS(attempt0.getStringField("status"),
                  std::string("FailedToParse: fail on clone -- listDBs injected failure"))
        << attempt0;
    ASSERT_EQUALS(attempt0["durationMillis"].type(), NumberInt) << attempt0;
    ASSERT_EQUALS(attempt0.getStringField("syncSource"), std::string("localhost:27017"))
        << attempt0;

    // Play last successful response.
    setResponses({successfulResponses.end() - 1, successfulResponses.end()});
    playResponses();

    log() << "waiting for initial sync to verify it completed OK";
    verifySync(getNet());

    progress = getInitialSyncProgress();
    log() << "Progress at end: " << progress;
    ASSERT_EQUALS(progress.nFields(), 10) << progress;
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
    ASSERT_EQUALS(attempt0.getStringField("status"),
                  std::string("FailedToParse: fail on clone -- listDBs injected failure"))
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

TEST_F(InitialSyncTest, DataReplicatorCreatesNewApplierForNextBatchBeforeDestroyingCurrentApplier) {
    auto getRollbackIdResponse = BSON("ok" << 1 << "rbid" << 1);
    auto noopOp1 = BSON("ts" << Timestamp(Seconds(1), 1U) << "h" << 1LL << "v"
                             << OplogEntry::kOplogVersion
                             << "ns"
                             << ""
                             << "op"
                             << "n"
                             << "o"
                             << BSON("msg"
                                     << "noop"));
    auto createCollectionOp1 =
        BSON("ts" << Timestamp(Seconds(2), 1U) << "h" << 1LL << "v" << OplogEntry::kOplogVersion
                  << "ns"
                  << "test.$cmd"
                  << "op"
                  << "c"
                  << "o"
                  << BSON("create"
                          << "coll1"));
    auto createCollectionOp2 =
        BSON("ts" << Timestamp(Seconds(3), 1U) << "h" << 1LL << "v" << OplogEntry::kOplogVersion
                  << "ns"
                  << "test.$cmd"
                  << "op"
                  << "c"
                  << "o"
                  << BSON("create"
                          << "coll2"));
    const Responses responses = {
        // pre-initial sync rollback checker request
        {"replSetGetRBID", getRollbackIdResponse},
        // get latest oplog ts - this should match the first op returned by the oplog fetcher
        {"find",
         BSON("ok" << 1 << "cursor" << BSON("id" << 0LL << "ns"
                                                 << "local.oplog.rs"
                                                 << "firstBatch"
                                                 << BSON_ARRAY(noopOp1)))},
        // oplog fetcher find - single set of results containing two commands that have to be
        // applied in separate batches per batching logic
        {"find",
         BSON("ok" << 1 << "cursor" << BSON("id" << 0LL << "ns"
                                                 << "local.oplog.rs"
                                                 << "firstBatch"
                                                 << BSON_ARRAY(noopOp1 << createCollectionOp1
                                                                       << createCollectionOp2)))},
        // Clone Start
        // listDatabases - return empty list of databases since we're not testing the cloner.
        {"listDatabases", BSON("ok" << 1 << "databases" << BSONArray())},
        // get latest oplog ts - this should match the last op returned by the oplog fetcher
        {"find",
         BSON("ok" << 1 << "cursor" << BSON("id" << 0LL << "ns"
                                                 << "local.oplog.rs"
                                                 << "firstBatch"
                                                 << BSON_ARRAY(createCollectionOp2)))},
        // post-initial sync rollback checker request
        {"replSetGetRBID", getRollbackIdResponse},
    };

    startSync(1);

    setResponses(responses);
    playResponses();
    log() << "Done playing responses";
    verifySync(getNet());
    ASSERT_EQUALS(OplogEntry(createCollectionOp2).getOpTime(), _myLastOpTime);
}

}  // namespace
