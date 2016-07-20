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
    HostAndPort chooseNewSyncSource(const Timestamp& ts) override {
        HostAndPort result = _syncSource;
        _syncSource = HostAndPort();
        return result;
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {
        _blacklistedSource = host;
    }
    bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                const rpc::ReplSetMetadata& metadata) override {
        return false;
    }
    SyncSourceResolverResponse selectSyncSource(OperationContext* txn,
                                                const OpTime& lastOpTimeFetched) override {
        return SyncSourceResolverResponse();
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
        _rollbackFn = [](OperationContext*, const OpTime&, const HostAndPort&) -> Status {
            return Status::OK();
        };
        _setMyLastOptime = [this](const OpTime& opTime) { _myLastOpTime = opTime; };
        _myLastOpTime = OpTime();
        _memberState = MemberState::RS_UNKNOWN;
        _syncSourceSelector.reset(new SyncSourceSelectorMock(HostAndPort("localhost", -1)));
    }

    // SyncSourceSelector
    void clearSyncSourceBlacklist() override {
        _syncSourceSelector->clearSyncSourceBlacklist();
    }
    HostAndPort chooseNewSyncSource(const Timestamp& ts) override {
        return _syncSourceSelector->chooseNewSyncSource(ts);
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {
        _syncSourceSelector->blacklistSyncSource(host, until);
    }
    bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                const rpc::ReplSetMetadata& metadata) override {
        return _syncSourceSelector->shouldChangeSyncSource(currentSource, metadata);
    }
    SyncSourceResolverResponse selectSyncSource(OperationContext* txn,
                                                const OpTime& lastOpTimeFetched) override {
        return SyncSourceResolverResponse();
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

    StorageInterfaceResults _storageInterfaceWorkDone;

    void setUp() override {
        executor::ThreadPoolExecutorTest::setUp();
        _storageInterface = stdx::make_unique<StorageInterfaceMock>();
        _storageInterface->createOplogFn = [this](OperationContext* txn,
                                                  const NamespaceString& nss) {
            _storageInterfaceWorkDone.createOplogCalled = true;
            return Status::OK();
        };
        _storageInterface->insertDocumentFn =
            [this](OperationContext* txn, const NamespaceString& nss, const BSONObj& doc) {
                ++_storageInterfaceWorkDone.documentsInsertedCount;
                return Status::OK();
            };
        _storageInterface->insertDocumentsFn = [this](
            OperationContext* txn, const NamespaceString& nss, const std::vector<BSONObj>& ops) {
            _storageInterfaceWorkDone.insertedOplogEntries = true;
            ++_storageInterfaceWorkDone.oplogEntriesInserted;
            return Status::OK();
        };
        _storageInterface->dropCollFn = [this](OperationContext* txn, const NamespaceString& nss) {
            _storageInterfaceWorkDone.droppedCollections.push_back(nss.ns());
            return Status::OK();
        };
        _storageInterface->dropUserDBsFn = [this](OperationContext* txn) {
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
                    ->init(nullptr, nullptr, secondaryIndexSpecs);

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
        options.rollbackFn = [this](OperationContext* txn,
                                    const OpTime& lastOpTimeWritten,
                                    const HostAndPort& syncSource) -> Status {
            return _rollbackFn(txn, lastOpTimeWritten, syncSource);
        };

        options.prepareReplSetUpdatePositionCommandFn =
            [](ReplicationCoordinator::ReplSetUpdatePositionCommandStyle commandStyle)
            -> StatusWith<BSONObj> { return BSON(UpdatePositionArgs::kCommandFieldName << 1); };
        options.getMyLastOptime = [this]() { return _myLastOpTime; };
        options.setMyLastOptime = [this](const OpTime& opTime) { _setMyLastOptime(opTime); };
        options.setFollowerMode = [this](const MemberState& state) {
            _memberState = state;
            return true;
        };
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
        } catch (...) {
            ASSERT_OK(exceptionToStatus());
        }
    }

    void tearDown() override {
        executor::ThreadPoolExecutorTest::shutdownExecutorThread();
        executor::ThreadPoolExecutorTest::joinExecutorThread();

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


    DataReplicatorOptions::RollbackFn _rollbackFn;
    DataReplicatorOptions::SetMyLastOptimeFn _setMyLastOptime;
    OpTime _myLastOpTime;
    MemberState _memberState;
    std::unique_ptr<SyncSourceSelector> _syncSourceSelector;
    std::unique_ptr<StorageInterfaceMock> _storageInterface;
    std::unique_ptr<OldThreadPool> _dbWorkThreadPool;
    std::map<NamespaceString, CollectionMockStats> _collectionStats;
    std::map<NamespaceString, CollectionCloneInfo> _collections;

private:
    DataReplicatorExternalStateMock* _externalState;
    std::unique_ptr<DataReplicator> _dr;
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

TEST_F(DataReplicatorTest, StartOk) {
    ASSERT_OK(getDR().start(makeOpCtx().get()));
}

TEST_F(DataReplicatorTest, CannotInitialSyncAfterStart) {
    auto txn = makeOpCtx();
    ASSERT_EQ(getDR().start(txn.get()).code(), ErrorCodes::OK);
    ASSERT_EQ(getDR().doInitialSync(txn.get()), ErrorCodes::AlreadyInitialized);
}

// Used to run a Initial Sync in a separate thread, to avoid blocking test execution.
class InitialSyncBackgroundRunner {
public:
    InitialSyncBackgroundRunner(DataReplicator* dr) : _dr(dr) {}

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

private:
    void _run() {
        setThreadName("InitialSyncRunner");
        Client::initThreadIfNotAlready();
        auto txn = getGlobalServiceContext()->makeOperationContext(&cc());

        // Synchonize this thread starting with the call in run() above.
        UniqueLock lk(_mutex);
        _condVar.notify_all();
        lk.unlock();

        auto result = _dr->doInitialSync(txn.get());  // blocking

        lk.lock();
        _result = result;
    }

    stdx::mutex _mutex;  // protects _result.
    StatusWith<OpTimeWithHash> _result{ErrorCodes::NotYetInitialized, "InitialSync not started."};

    DataReplicator* _dr;
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

    void startSync() {
        DataReplicator* dr = &(getDR());
        _isbr.reset(new InitialSyncBackgroundRunner(dr));
        _isbr->run();
    }

    void playResponsesNTimees(int n) {
        for (int x = 0; x < n; ++x) {
            log() << "playing responses, pass " << x << " of " << n;
            playResponses(false);
        }
        playResponses(true);
    }

    void playResponses(bool isLastBatchOfResponses) {
        NetworkInterfaceMock* net = getNet();
        int processedRequests(0);
        const int expectedResponses(_responses.size());

        Date_t lastLog{Date_t::now()};
        // counter for oplog entries
        int c(1);
        while (true) {
            if (_isbr && _isbr->isDone()) {
                log() << "There are responses left which were unprocessed.";
                return;
            }

            NetworkGuard guard(net);
            if (!net->hasReadyRequests() && processedRequests < expectedResponses) {
                net->runReadyNetworkOperations();
                guard.dismiss();
                continue;
            }

            auto noi = net->getNextReadyRequest();
            if (isOplogGetMore(noi)) {
                // process getmore requests from the oplog fetcher
                auto respBSON =
                    fromjson(str::stream() << "{ok:1, cursor:{id:NumberLong(1), ns:'local.oplog.rs'"
                                              " , nextBatch:[{ts:Timestamp("
                                           << ++c
                                           << ",1), h:NumberLong(1), ns:'test.a', v:"
                                           << OplogEntry::kOplogVersion
                                           << ", op:'i', o:{_id:"
                                           << c
                                           << "}}]}}");
                net->scheduleResponse(
                    noi,
                    net->now(),
                    ResponseStatus(RemoteCommandResponse(respBSON, BSONObj(), Milliseconds(10))));

                if ((Date_t::now() - lastLog) > Seconds(1)) {
                    lastLog = Date_t::now();
                    log() << "processing oplog getmore, net:" << net->getDiagnosticString();
                    net->logQueues();
                }
                net->runReadyNetworkOperations();
                guard.dismiss();
                continue;
            } else if (isOplogKillCursor(noi)) {
                auto respBSON = BSON("ok" << 1.0);
                log() << "processing oplog killcursors req, net:" << net->getDiagnosticString();
                net->scheduleResponse(
                    noi,
                    net->now(),
                    ResponseStatus(RemoteCommandResponse(respBSON, BSONObj(), Milliseconds(10))));
                net->runReadyNetworkOperations();
                guard.dismiss();
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

        if (!isLastBatchOfResponses) {
            return;
        }

        NetworkGuard guard(net);
        if (net->hasReadyRequests()) {
            // Blackhole all oplog getmores for cursor 1.
            while (net->hasReadyRequests()) {
                auto noi = net->getNextReadyRequest();
                if (isOplogGetMore(noi)) {
                    net->blackHole(noi);
                    continue;
                }

                // Error.
                ASSERT_FALSE(net->hasReadyRequests());
            }
        }
    }

    void verifySync(NetworkInterfaceMock* net, Status s = Status::OK()) {
        verifySync(net, s.code());
    }

    void verifySync(NetworkInterfaceMock* net, ErrorCodes::Error code) {
        // Check result
        ASSERT_EQ(_isbr->getResult(net).getStatus().code(), code) << "status codes differ";
    }

private:
    Responses _responses;
    std::unique_ptr<InitialSyncBackgroundRunner> _isbr{nullptr};
};

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
            {"find",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:["
                         "{ts:Timestamp(1,1), h:NumberLong(1), ns:'a.a', v:"
                      << OplogEntry::kOplogVersion
                      << ", op:'i', o:{_id:1, a:1}}]}}")},
            // Applier starts ...
        };

    // Initial sync flag should not be set before starting.
    auto txn = makeOpCtx();
    ASSERT_FALSE(getStorage().getInitialSyncFlag(txn.get()));

    startSync();

    // Play first response to ensure data replicator has entered initial sync state.
    setResponses({responses.begin(), responses.begin() + 1});
    playResponses(false);

    // Initial sync flag should be set.
    ASSERT_TRUE(getStorage().getInitialSyncFlag(txn.get()));

    // Play rest of the responses after checking initial sync flag.
    setResponses({responses.begin() + 1, responses.end()});
    playResponses(true);
    log() << "done playing last responses";

    log() << "doing asserts";
    ASSERT_TRUE(_storageInterfaceWorkDone.droppedUserDBs);
    ASSERT_TRUE(_storageInterfaceWorkDone.createOplogCalled);
    ASSERT_EQ(1, _storageInterfaceWorkDone.oplogEntriesInserted);

    log() << "waiting for initial sync to verify it completed OK";
    verifySync(getNet());

    log() << "checking initial sync flag isn't set.";
    // Initial sync flag should not be set after completion.
    ASSERT_FALSE(getStorage().getInitialSyncFlag(txn.get()));
}


TEST_F(InitialSyncTest, Failpoint) {
    mongo::getGlobalFailPointRegistry()
        ->getFailPoint("failInitialSyncWithBadHost")
        ->setMode(FailPoint::alwaysOn);

    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")));

    Timestamp time1(100, 1);
    OpTime opTime1(time1, OpTime::kInitialTerm);
    _myLastOpTime = opTime1;
    _memberState = MemberState::RS_SECONDARY;

    DataReplicator* dr = &(getDR());
    InitialSyncBackgroundRunner isbr(dr);
    isbr.run();

    ASSERT_EQ(isbr.getResult(getNet()).getStatus().code(), ErrorCodes::InitialSyncFailure);

    mongo::getGlobalFailPointRegistry()
        ->getFailPoint("failInitialSyncWithBadHost")
        ->setMode(FailPoint::off);
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
         fromjson("{ok:0, errmsg:'fail on clone -- listDBs injected failure', code:9}")},
        // rollback checker.
        {"replSetGetRBID", fromjson(str::stream() << "{ok: 1, rbid:1}")},

    };
    startSync();
    setResponses(responses);
    playResponsesNTimees(repl::kInitialSyncMaxRetries);
    verifySync(getNet(), ErrorCodes::InitialSyncFailure);
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

    startSync();
    setResponses(responses);
    playResponsesNTimees(repl::kInitialSyncMaxRetries);
    verifySync(getNet(), ErrorCodes::InitialSyncFailure);
}


class TestSyncSourceSelector2 : public SyncSourceSelector {
public:
    void clearSyncSourceBlacklist() override {}
    HostAndPort chooseNewSyncSource(const Timestamp& ts) override {
        LockGuard lk(_mutex);
        auto result = HostAndPort(str::stream() << "host-" << _nextSourceNum++, -1);
        _condition.notify_all();
        return result;
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {
        LockGuard lk(_mutex);
        _blacklistedSource = host;
    }
    bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                const rpc::ReplSetMetadata& metadata) override {
        return false;
    }
    SyncSourceResolverResponse selectSyncSource(OperationContext* txn,
                                                const OpTime& lastOpTimeFetched) override {
        return SyncSourceResolverResponse();
    }
    mutable stdx::mutex _mutex;
    stdx::condition_variable _condition;
    int _nextSourceNum{0};
    HostAndPort _blacklistedSource;
};

class SteadyStateTest : public DataReplicatorTest {
protected:
    void _setUpOplogFetcherFailed() {
        DataReplicator& dr = getDR();
        _syncSourceSelector.reset(new TestSyncSourceSelector2());
        _memberState = MemberState::RS_UNKNOWN;
        auto net = getNet();
        net->enterNetwork();
        ASSERT_OK(dr.start(makeOpCtx().get()));
    }

    void _testOplogFetcherFailed(const BSONObj& oplogFetcherResponse,
                                 const Status& rollbackStatus,
                                 const HostAndPort& expectedRollbackSource,
                                 const HostAndPort& expectedBlacklistedSource,
                                 const HostAndPort& expectedFinalSource,
                                 const MemberState& expectedFinalState,
                                 const DataReplicatorState& expectedDataReplicatorState,
                                 int expectedNextSourceNum) {
        OperationContext* rollbackTxn = nullptr;
        HostAndPort rollbackSource;
        DataReplicatorState stateDuringRollback = DataReplicatorState::Uninitialized;
        // Rollback happens on network thread now instead of DB worker thread previously.
        _rollbackFn = [&](OperationContext* txn,
                          const OpTime& lastOpTimeWritten,
                          const HostAndPort& syncSource) -> Status {
            rollbackTxn = txn;
            rollbackSource = syncSource;
            stateDuringRollback = getDR().getState();
            return rollbackStatus;
        };

        auto net = getNet();
        ASSERT_TRUE(net->hasReadyRequests());
        auto noi = net->getNextReadyRequest();
        ASSERT_EQUALS("find", std::string(noi->getRequest().cmdObj.firstElementFieldName()));
        scheduleNetworkResponse(noi, oplogFetcherResponse);
        net->runReadyNetworkOperations();

        // Replicator state should be ROLLBACK before rollback function returns.
        ASSERT_EQUALS(toString(DataReplicatorState::Rollback), toString(stateDuringRollback));
        ASSERT_TRUE(rollbackTxn);
        ASSERT_EQUALS(expectedRollbackSource, rollbackSource);

        auto&& dr = getDR();
        dr.waitForState(expectedDataReplicatorState);

        // Wait for data replicator to request a new sync source if rollback is expected to fail.
        if (!rollbackStatus.isOK()) {
            TestSyncSourceSelector2* syncSourceSelector =
                static_cast<TestSyncSourceSelector2*>(_syncSourceSelector.get());
            UniqueLock lk(syncSourceSelector->_mutex);
            while (syncSourceSelector->_nextSourceNum < expectedNextSourceNum) {
                syncSourceSelector->_condition.wait(lk);
            }
            ASSERT_EQUALS(expectedBlacklistedSource, syncSourceSelector->_blacklistedSource);
        }

        ASSERT_EQUALS(expectedFinalSource, dr.getSyncSource());
        ASSERT_EQUALS(expectedFinalState.toString(), _memberState.toString());
    }
};

TEST_F(SteadyStateTest, StartWhenInSteadyState) {
    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    auto txn = makeOpCtx();
    ASSERT_OK(dr.start(txn.get()));
    ASSERT_EQUALS(toString(DataReplicatorState::Steady), toString(dr.getState()));
    ASSERT_EQUALS(ErrorCodes::IllegalOperation, dr.start(txn.get()));
}

TEST_F(SteadyStateTest, ShutdownAfterStart) {
    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    auto net = getNet();
    NetworkGuard guard(net);
    auto txn = makeOpCtx();
    ASSERT_OK(dr.start(txn.get()));
    ASSERT_TRUE(net->hasReadyRequests());
    getExecutor().shutdown();
    ASSERT_EQUALS(toString(DataReplicatorState::Steady), toString(dr.getState()));
    ASSERT_EQUALS(ErrorCodes::IllegalOperation, dr.start(txn.get()));
}

TEST_F(SteadyStateTest, RequestShutdownAfterStart) {
    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    {
        auto net = getNet();
        NetworkGuard guard(net);
        auto txn = makeOpCtx();
        ASSERT_OK(dr.start(txn.get()));
        ASSERT_TRUE(net->hasReadyRequests());
        ASSERT_EQUALS(toString(DataReplicatorState::Steady), toString(dr.getState()));
        // Simulating an invalid remote oplog query response. This will invalidate the existing
        // sync source but that's fine because we're not testing oplog processing.
        scheduleNetworkResponse("find", BSON("ok" << 0));
        net->runReadyNetworkOperations();
        ASSERT_OK(dr.scheduleShutdown(txn.get()));
    }
    // runs work item scheduled in 'scheduleShutdown()).
    dr.waitForShutdown();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
}

class ShutdownExecutorSyncSourceSelector : public SyncSourceSelector {
public:
    ShutdownExecutorSyncSourceSelector(executor::TaskExecutor* exec) : _exec(exec) {}
    void clearSyncSourceBlacklist() override {}
    HostAndPort chooseNewSyncSource(const Timestamp& ts) override {
        _exec->shutdown();
        return HostAndPort();
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {}
    bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                const rpc::ReplSetMetadata& metadata) override {
        return false;
    }
    SyncSourceResolverResponse selectSyncSource(OperationContext* txn,
                                                const OpTime& lastOpTimeFetched) override {
        return SyncSourceResolverResponse();
    }
    executor::TaskExecutor* _exec;
};

TEST_F(SteadyStateTest, ScheduleNextActionFailsAfterChoosingEmptySyncSource) {
    _syncSourceSelector.reset(new ShutdownExecutorSyncSourceSelector(&getExecutor()));

    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    auto net = getNet();
    net->enterNetwork();
    ASSERT_OK(dr.start(makeOpCtx().get()));
    ASSERT_EQUALS(HostAndPort(), dr.getSyncSource());
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
}

TEST_F(SteadyStateTest, ChooseNewSyncSourceAfterFailedNetworkRequest) {
    TestSyncSourceSelector2* testSyncSourceSelector = new TestSyncSourceSelector2();
    _syncSourceSelector.reset(testSyncSourceSelector);

    _memberState = MemberState::RS_UNKNOWN;
    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    auto net = getNet();
    NetworkGuard guard(net);
    ASSERT_OK(dr.start(makeOpCtx().get()));
    ASSERT_TRUE(net->hasReadyRequests());
    ASSERT_EQUALS(toString(DataReplicatorState::Steady), toString(dr.getState()));
    // Simulating an invalid remote oplog query response to cause the data replicator to
    // blacklist the existing sync source and request a new one.
    scheduleNetworkResponse("find", BSON("ok" << 0));
    net->runReadyNetworkOperations();

    // Wait for data replicator to request a new sync source.
    {
        UniqueLock lk(testSyncSourceSelector->_mutex);
        while (testSyncSourceSelector->_nextSourceNum < 2) {
            testSyncSourceSelector->_condition.wait(lk);
        }
        ASSERT_EQUALS(HostAndPort("host-0", -1), testSyncSourceSelector->_blacklistedSource);
    }
    ASSERT_EQUALS(HostAndPort("host-1", -1), dr.getSyncSource());
    ASSERT_EQUALS(MemberState(MemberState::RS_UNKNOWN).toString(), _memberState.toString());
    ASSERT_EQUALS(toString(DataReplicatorState::Steady), toString(dr.getState()));
}

TEST_F(SteadyStateTest, RemoteOplogEmptyRollbackSucceeded) {
    _setUpOplogFetcherFailed();
    auto oplogFetcherResponse =
        fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch: []}}");
    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status::OK(),
                            HostAndPort("host-0", -1),  // rollback source
                            HostAndPort(),              // sync source should not be blacklisted.
                            HostAndPort("host-0", -1),
                            MemberState::RS_SECONDARY,
                            DataReplicatorState::Steady,
                            2);
}

TEST_F(SteadyStateTest, RemoteOplogEmptyRollbackFailed) {
    _setUpOplogFetcherFailed();
    auto oplogFetcherResponse =
        fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch: []}}");
    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status(ErrorCodes::OperationFailed, "rollback failed"),
                            HostAndPort("host-0", -1),  // rollback source
                            HostAndPort("host-0", -1),
                            HostAndPort("host-1", -1),
                            MemberState::RS_UNKNOWN,
                            DataReplicatorState::Rollback,
                            2);
}

TEST_F(SteadyStateTest, RemoteOplogFirstOperationMissingTimestampRollbackFailed) {
    _setUpOplogFetcherFailed();
    auto oplogFetcherResponse =
        fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch: [{}]}}");
    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status(ErrorCodes::OperationFailed, "rollback failed"),
                            HostAndPort("host-0", -1),  // rollback source
                            HostAndPort("host-0", -1),
                            HostAndPort("host-1", -1),
                            MemberState::RS_UNKNOWN,
                            DataReplicatorState::Rollback,
                            2);
}

TEST_F(SteadyStateTest, RemoteOplogFirstOperationTimestampDoesNotMatchRollbackFailed) {
    _setUpOplogFetcherFailed();
    auto oplogFetcherResponse = fromjson(
        "{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch:[{ts:Timestamp(1,1)}]}}");
    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status(ErrorCodes::OperationFailed, "rollback failed"),
                            HostAndPort("host-0", -1),  // rollback source
                            HostAndPort("host-0", -1),
                            HostAndPort("host-1", -1),
                            MemberState::RS_UNKNOWN,
                            DataReplicatorState::Rollback,
                            2);
}

TEST_F(SteadyStateTest, RollbackTwoSyncSourcesBothFailed) {
    _setUpOplogFetcherFailed();
    auto oplogFetcherResponse =
        fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch: []}}");

    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status(ErrorCodes::OperationFailed, "rollback failed"),
                            HostAndPort("host-0", -1),  // rollback source
                            HostAndPort("host-0", -1),
                            HostAndPort("host-1", -1),
                            MemberState::RS_UNKNOWN,
                            DataReplicatorState::Rollback,
                            2);

    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status(ErrorCodes::OperationFailed, "rollback failed"),
                            HostAndPort("host-1", -1),  // rollback source
                            HostAndPort("host-1", -1),
                            HostAndPort("host-2", -1),
                            MemberState::RS_UNKNOWN,
                            DataReplicatorState::Rollback,
                            3);
}

TEST_F(SteadyStateTest, RollbackTwoSyncSourcesSecondRollbackSucceeds) {
    _setUpOplogFetcherFailed();
    auto oplogFetcherResponse =
        fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'local.oplog.rs', firstBatch: []}}");

    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status(ErrorCodes::OperationFailed, "rollback failed"),
                            HostAndPort("host-0", -1),  // rollback source
                            HostAndPort("host-0", -1),
                            HostAndPort("host-1", -1),
                            MemberState::RS_UNKNOWN,
                            DataReplicatorState::Rollback,
                            2);

    _testOplogFetcherFailed(oplogFetcherResponse,
                            Status::OK(),
                            HostAndPort("host-1", -1),  // rollback source
                            HostAndPort("host-0", -1),  // blacklisted source unchanged
                            HostAndPort("host-1", -1),
                            MemberState::RS_SECONDARY,
                            DataReplicatorState::Steady,
                            2);  // not used when rollback is expected to succeed
}

// TODO: re-enable
// Disabled until start reads the last fetch oplog entry so the hash is correct and doesn't detect
// a rollback when the OplogFetcher starts.
// TEST_F(SteadyStateTest, PauseDataReplicator) {
//    auto lastOperationApplied = BSON("op"
//                                     << "a"
//                                     << "v"
//                                     << OplogEntry::kOplogVersion
//                                     << "ts"
//                                     << Timestamp(Seconds(123), 0)
//                                     << "h"
//                                     << 12LL);
//
//    auto operationToApply = BSON("op"
//                                 << "a"
//                                 << "v"
//                                 << OplogEntry::kOplogVersion
//                                 << "ts"
//                                 << Timestamp(Seconds(456), 0)
//                                 << "h"
//                                 << 13LL);
//
//    stdx::mutex mutex;
//    unittest::Barrier barrier(2U);
//    Timestamp lastTimestampApplied;
//    BSONObj operationApplied;
//    getExternalState()->multiApplyFn = [&](OperationContext*,
//                                           const MultiApplier::Operations& ops,
//                                           MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
//        LockGuard lock(mutex);
//        operationApplied = ops.back().raw;
//        barrier.countDownAndWait();
//        return ops.back().getOpTime();
//    };
//    DataReplicatorOptions::SetMyLastOptimeFn oldSetMyLastOptime = _setMyLastOptime;
//    _setMyLastOptime = [&](const OpTime& opTime) {
//        oldSetMyLastOptime(opTime);
//        LockGuard lock(mutex);
//        lastTimestampApplied = opTime.getTimestamp();
//        barrier.countDownAndWait();
//    };
//
//    auto& dr = getDR();
//    _myLastOpTime = OpTime(lastOperationApplied["ts"].timestamp(), OpTime::kInitialTerm);
//    _memberState = MemberState::RS_SECONDARY;
//
//    auto net = getNet();
//    net->enterNetwork();
//
//    ASSERT_OK(dr.start(makeOpCtx().get()));
//
//    ASSERT_TRUE(net->hasReadyRequests());
//    {
//        auto networkRequest = net->getNextReadyRequest();
//        auto commandResponse =
//            BSON("ok" << 1 << "cursor"
//                      << BSON("id" << 1LL << "ns"
//                                   << "local.oplog.rs"
//                                   << "firstBatch"
//                                   << BSON_ARRAY(lastOperationApplied << operationToApply)));
//        scheduleNetworkResponse(networkRequest, commandResponse);
//    }
//
//    dr.pause();
//
//    ASSERT_EQUALS(0U, dr.getOplogBufferCount());
//
//    // Data replication will process the fetcher response but will not schedule the applier.
//    net->runReadyNetworkOperations();
//    ASSERT_EQUALS(operationToApply["ts"].timestamp(), dr.getLastTimestampFetched());
//
//    // Schedule a bogus work item to ensure that the operation applier function
//    // is not scheduled.
//    auto& exec = getExecutor();
//    exec.scheduleWork(
//        [&barrier](const executor::TaskExecutor::CallbackArgs&) { barrier.countDownAndWait(); });
//
//
//    // Wake up executor thread and wait for bogus work callback to be invoked.
//    net->exitNetwork();
//    barrier.countDownAndWait();
//
//    // Oplog buffer should contain fetched operations since applier is not scheduled.
//    ASSERT_EQUALS(1U, dr.getOplogBufferCount());
//
//    dr.resume();
//
//    // Wait for applier function.
//    barrier.countDownAndWait();
//    // Run scheduleWork() work item scheduled in DataReplicator::_onApplyBatchFinish().
//    net->exitNetwork();
//
//    // Wait for batch completion callback.
//    barrier.countDownAndWait();
//
//    ASSERT_EQUALS(MemberState(MemberState::RS_SECONDARY).toString(), _memberState.toString());
//    {
//        LockGuard lock(mutex);
//        ASSERT_EQUALS(operationToApply, operationApplied);
//        ASSERT_EQUALS(operationToApply["ts"].timestamp(), lastTimestampApplied);
//    }
//}

// TEST_F(SteadyStateTest, ApplyOneOperation) {
//    auto lastOperationApplied = BSON("op"
//                                     << "a"
//                                     << "v"
//                                     << OplogEntry::kOplogVersion
//                                     << "ts"
//                                     << Timestamp(Seconds(123), 0)
//                                     << "h"
//                                     << 12LL);
//
//    auto operationToApply = BSON("op"
//                                 << "a"
//                                 << "v"
//                                 << OplogEntry::kOplogVersion
//                                 << "ts"
//                                 << Timestamp(Seconds(456), 0)
//                                 << "h"
//                                 << 13LL);
//
//    stdx::mutex mutex;
//    unittest::Barrier barrier(2U);
//    Timestamp lastTimestampApplied;
//    BSONObj operationApplied;
//    getExternalState()->multiApplyFn = [&](OperationContext*,
//                                           const MultiApplier::Operations& ops,
//                                           MultiApplier::ApplyOperationFn) -> StatusWith<OpTime> {
//        LockGuard lock(mutex);
//        operationApplied = ops.back().raw;
//        barrier.countDownAndWait();
//        return ops.back().getOpTime();
//    };
//    DataReplicatorOptions::SetMyLastOptimeFn oldSetMyLastOptime = _setMyLastOptime;
//    _setMyLastOptime = [&](const OpTime& opTime) {
//        oldSetMyLastOptime(opTime);
//        LockGuard lock(mutex);
//        lastTimestampApplied = opTime.getTimestamp();
//        barrier.countDownAndWait();
//    };
//
//    _myLastOpTime = OpTime(lastOperationApplied["ts"].timestamp(), OpTime::kInitialTerm);
//    _memberState = MemberState::RS_SECONDARY;
//
//    auto net = getNet();
//    {
//        NetworkGuard guard(net);
//
//        auto& dr = getDR();
//        ASSERT_OK(dr.start(makeOpCtx().get()));
//
//        ASSERT_TRUE(net->hasReadyRequests());
//        {
//            auto networkRequest = net->getNextReadyRequest();
//            auto commandResponse =
//                BSON("ok" << 1 << "cursor"
//                          << BSON("id" << 1LL << "ns"
//                                       << "local.oplog.rs"
//                                       << "firstBatch"
//                                       << BSON_ARRAY(lastOperationApplied << operationToApply)));
//            scheduleNetworkResponse(networkRequest, commandResponse);
//        }
//        ASSERT_EQUALS(0U, dr.getOplogBufferCount());
//
//        // Oplog buffer should be empty because contents are transferred to applier.
//        net->runReadyNetworkOperations();
//        ASSERT_EQUALS(0U, dr.getOplogBufferCount());
//
//        // Wait for applier function.
//        barrier.countDownAndWait();
//        ASSERT_EQUALS(operationToApply["ts"].timestamp(), dr.getLastTimestampFetched());
//        // Run scheduleWork() work item scheduled in DataReplicator::_onApplyBatchFinish().
//    }
//    // Wait for batch completion callback.
//    barrier.countDownAndWait();
//
//    ASSERT_EQUALS(MemberState(MemberState::RS_SECONDARY).toString(), _memberState.toString());
//    {
//        LockGuard lock(mutex);
//        ASSERT_EQUALS(operationToApply, operationApplied);
//        ASSERT_EQUALS(operationToApply["ts"].timestamp(), lastTimestampApplied);
//    }
//
//    // Ensure that we send position information upstream after completing batch.
//    NetworkGuard guard(net);
//    bool found = false;
//    while (net->hasReadyRequests()) {
//        auto networkRequest = net->getNextReadyRequest();
//        auto commandRequest = networkRequest->getRequest();
//        const auto& cmdObj = commandRequest.cmdObj;
//        if (str::equals(cmdObj.firstElementFieldName(), UpdatePositionArgs::kCommandFieldName) &&
//            commandRequest.dbname == "admin") {
//            found = true;
//            break;
//        } else {
//            net->blackHole(networkRequest);
//        }
//    }
//    ASSERT_TRUE(found);
//}

}  // namespace
