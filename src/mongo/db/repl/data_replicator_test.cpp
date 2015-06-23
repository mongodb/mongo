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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/client/fetcher.h"
#include "mongo/db/json.h"
#include "mongo/db/repl/base_cloner_test_fixture.h"
#include "mongo/db/repl/data_replicator.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_executor_test_fixture.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/reporter.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"

namespace {
using namespace mongo;
using namespace mongo::repl;
using executor::NetworkInterfaceMock;
using LockGuard = stdx::lock_guard<stdx::mutex>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;
using mutex = stdx::mutex;

class SyncSourceSelectorMock : public SyncSourceSelector {
    MONGO_DISALLOW_COPYING(SyncSourceSelectorMock);

public:
    SyncSourceSelectorMock(const HostAndPort& syncSource) : _syncSource(syncSource) {}
    void clearSyncSourceBlacklist() override {}
    HostAndPort chooseNewSyncSource() override {
        HostAndPort result = _syncSource;
        _syncSource = HostAndPort();
        return result;
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {}
    bool shouldChangeSyncSource(const HostAndPort& currentSource) override {
        return false;
    }
    HostAndPort _syncSource;
};

class DataReplicatorTest : public ReplicationExecutorTest,
                           public ReplicationProgressManager,
                           public SyncSourceSelector {
public:
    DataReplicatorTest() {}

    void postExecutorThreadLaunch() override{};

    /**
     * clear/reset state
     */
    void reset() {
        _applierFn = [](OperationContext*, const BSONObj&) -> Status { return Status::OK(); };
        _setMyLastOptime = [this](const OpTime& opTime) { _myLastOpTime = opTime; };
        _myLastOpTime = OpTime();
        _memberState = MemberState::RS_UNKNOWN;
        _syncSourceSelector.reset(new SyncSourceSelectorMock(HostAndPort("localhost", -1)));
    }

    // ReplicationProgressManager
    bool prepareReplSetUpdatePositionCommand(BSONObjBuilder* cmdBuilder) override {
        cmdBuilder->append("replSetUpdatePosition", 1);
        return true;
    }

    // SyncSourceSelector
    void clearSyncSourceBlacklist() override {
        _syncSourceSelector->clearSyncSourceBlacklist();
    }
    HostAndPort chooseNewSyncSource() override {
        return _syncSourceSelector->chooseNewSyncSource();
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {
        _syncSourceSelector->blacklistSyncSource(host, until);
    }
    bool shouldChangeSyncSource(const HostAndPort& currentSource) override {
        return _syncSourceSelector->shouldChangeSyncSource(currentSource);
    }

    void scheduleNetworkResponse(const BSONObj& obj) {
        NetworkInterfaceMock* net = getNet();
        ASSERT_TRUE(net->hasReadyRequests());
        scheduleNetworkResponse(net->getNextReadyRequest(), obj);
    }

    void scheduleNetworkResponse(NetworkInterfaceMock::NetworkOperationIterator noi,
                                 const BSONObj& obj) {
        NetworkInterfaceMock* net = getNet();
        Milliseconds millis(0);
        RemoteCommandResponse response(obj, millis);
        ReplicationExecutor::ResponseStatus responseStatus(response);
        net->scheduleResponse(noi, net->now(), responseStatus);
    }

    void scheduleNetworkResponse(ErrorCodes::Error code, const std::string& reason) {
        NetworkInterfaceMock* net = getNet();
        ASSERT_TRUE(net->hasReadyRequests());
        ReplicationExecutor::ResponseStatus responseStatus(code, reason);
        net->scheduleResponse(net->getNextReadyRequest(), net->now(), responseStatus);
    }

    void processNetworkResponse(const BSONObj& obj) {
        scheduleNetworkResponse(obj);
        finishProcessingNetworkResponse();
    }

    void processNetworkResponse(ErrorCodes::Error code, const std::string& reason) {
        scheduleNetworkResponse(code, reason);
        finishProcessingNetworkResponse();
    }

    void finishProcessingNetworkResponse() {
        getNet()->runReadyNetworkOperations();
        ASSERT_FALSE(getNet()->hasReadyRequests());
    }

    DataReplicator& getDR() {
        return *_dr;
    }

protected:
    void setUp() override {
        ReplicationExecutorTest::setUp();
        reset();

        launchExecutorThread();
        DataReplicatorOptions options;
        options.initialSyncRetryWait = Milliseconds(0);
        options.applierFn = [this](OperationContext* txn, const BSONObj& operation) {
            return _applierFn(txn, operation);
        };
        options.replicationProgressManager = this;
        options.getMyLastOptime = [this]() { return _myLastOpTime; };
        options.setMyLastOptime = [this](const OpTime& opTime) { _setMyLastOptime(opTime); };
        options.setFollowerMode = [this](const MemberState& state) {
            _memberState = state;
            return true;
        };
        options.syncSourceSelector = this;
        try {
            _dr.reset(new DataReplicator(options, &(getExecutor())));
        } catch (...) {
            ASSERT_OK(exceptionToStatus());
        }
    }

    void tearDown() override {
        ReplicationExecutorTest::tearDown();
        _dr.reset();
        // Executor may still invoke callback before shutting down.
    }

    Applier::ApplyOperationFn _applierFn;
    DataReplicatorOptions::SetMyLastOptimeFn _setMyLastOptime;
    OpTime _myLastOpTime;
    MemberState _memberState;
    std::unique_ptr<SyncSourceSelector> _syncSourceSelector;

private:
    std::unique_ptr<DataReplicator> _dr;
};

TEST_F(DataReplicatorTest, CreateDestroy) {}

TEST_F(DataReplicatorTest, StartOk) {
    ASSERT_EQ(getDR().start().code(), ErrorCodes::OK);
}

TEST_F(DataReplicatorTest, CannotInitialSyncAfterStart) {
    ASSERT_EQ(getDR().start().code(), ErrorCodes::OK);
    ASSERT_EQ(getDR().initialSync(), ErrorCodes::AlreadyInitialized);
}

// Used to run a Initial Sync in a separate thread, to avoid blocking test execution.
class InitialSyncBackgroundRunner {
public:
    InitialSyncBackgroundRunner(DataReplicator* dr)
        : _dr(dr), _result(Status(ErrorCodes::BadValue, "failed to set status")) {}

    // Could block if _sgr has not finished
    TimestampStatus getResult() {
        _thread->join();
        return _result;
    }

    void run() {
        _thread.reset(new stdx::thread(stdx::bind(&InitialSyncBackgroundRunner::_run, this)));
        sleepmillis(2);  // sleep to let new thread run initialSync so it schedules work
    }

private:
    void _run() {
        setThreadName("InitialSyncRunner");
        log() << "starting initial sync";
        _result = _dr->initialSync();  // blocking
    }

    DataReplicator* _dr;
    TimestampStatus _result;
    std::unique_ptr<stdx::thread> _thread;
};

class InitialSyncTest : public DataReplicatorTest {
public:
    InitialSyncTest()
        : _insertCollectionFn([&](OperationContext* txn,
                                  const NamespaceString& theNss,
                                  const std::vector<BSONObj>& theDocuments) {
              log() << "insertDoc for " << theNss.toString();
              LockGuard lk(_collectionCountMutex);
              ++(_collectionCounts[theNss.toString()]);
              return Status::OK();
          }),
          _beginCollectionFn([&](OperationContext* txn,
                                 const NamespaceString& theNss,
                                 const CollectionOptions& theOptions,
                                 const std::vector<BSONObj>& theIndexSpecs) {
              log() << "beginCollection for " << theNss.toString();
              LockGuard lk(_collectionCountMutex);
              _collectionCounts[theNss.toString()] = 0;
              return Status::OK();
          }){};

protected:
    void setStorageFuncs(ClonerStorageInterfaceMock::InsertCollectionFn ins,
                         ClonerStorageInterfaceMock::BeginCollectionFn beg) {
        _insertCollectionFn = ins;
        _beginCollectionFn = beg;
    }

    void setResponses(std::vector<BSONObj> resps) {
        _responses = resps;
    }

    void startSync() {
        DataReplicator* dr = &(getDR());

        _storage.beginCollectionFn = _beginCollectionFn;
        _storage.insertDocumentsFn = _insertCollectionFn;
        _storage.insertMissingDocFn =
            [&](OperationContext* txn, const NamespaceString& nss, const BSONObj& doc) {
                return Status::OK();
            };

        dr->_setInitialSyncStorageInterface(&_storage);
        _isbr.reset(new InitialSyncBackgroundRunner(dr));
        _isbr->run();
    }


    void playResponses() {
        // TODO: Handle network responses
        NetworkInterfaceMock* net = getNet();
        int processedRequests(0);
        const int expectedResponses(_responses.size());

        // counter for oplog entries
        int c(0);
        while (true) {
            net->enterNetwork();
            if (!net->hasReadyRequests() && processedRequests < expectedResponses) {
                net->exitNetwork();
                continue;
            }
            NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();

            const BSONObj reqBSON = noi->getRequest().cmdObj;
            const BSONElement cmdElem = reqBSON.firstElement();
            const bool isGetMore = cmdElem.fieldNameStringData().equalCaseInsensitive("getmore");
            const long long cursorId = cmdElem.numberLong();
            if (isGetMore && cursorId == 1LL) {
                // process getmore requests from the oplog fetcher
                auto respBSON = fromjson(str::stream()
                                         << "{ok:1, cursor:{id:1, ns:'local.oplog.rs', nextBatch:["
                                            "{ts:Timestamp(" << ++c
                                         << ",1), h:1, ns:'test.a', v:2, op:'u', o2:{_id:" << c
                                         << "}, o:{$set:{a:1}}}"
                                            "]}}");
                net->scheduleResponse(
                    noi,
                    net->now(),
                    ResponseStatus(RemoteCommandResponse(respBSON, Milliseconds(10))));
                net->runReadyNetworkOperations();
                net->exitNetwork();
                continue;
            } else if (isGetMore) {
                // TODO: return more data
            }

            // process fixed set of responses
            log() << "processing network request: " << noi->getRequest().dbname << "."
                  << noi->getRequest().cmdObj.toString();
            net->scheduleResponse(noi,
                                  net->now(),
                                  ResponseStatus(RemoteCommandResponse(
                                      _responses[processedRequests], Milliseconds(10))));
            net->runReadyNetworkOperations();
            net->exitNetwork();
            if (++processedRequests >= expectedResponses) {
                log() << "done processing expected requests ";
                break;  // once we have processed all requests, continue;
            }
        }

        net->enterNetwork();
        if (net->hasReadyRequests()) {
            log() << "There are unexpected requests left";
            log() << "next cmd: " << net->getNextReadyRequest()->getRequest().cmdObj.toString();
            ASSERT_FALSE(net->hasReadyRequests());
        }
        net->exitNetwork();
    }

    void verifySync(Status s = Status::OK()) {
        verifySync(_isbr->getResult().getStatus().code());
    }

    void verifySync(ErrorCodes::Error code) {
        // Check result
        ASSERT_EQ(_isbr->getResult().getStatus().code(), code) << "status codes differ";
    }

    std::map<std::string, int> getLocalCollectionCounts() {
        return _collectionCounts;
    }

private:
    ClonerStorageInterfaceMock::InsertCollectionFn _insertCollectionFn;
    ClonerStorageInterfaceMock::BeginCollectionFn _beginCollectionFn;
    std::vector<BSONObj> _responses;
    std::unique_ptr<InitialSyncBackgroundRunner> _isbr;
    std::map<std::string, int> _collectionCounts;  // counts of inserts during cloning
    mutex _collectionCountMutex;                   // used to protect the collectionCount map
    ClonerStorageInterfaceMock _storage;
};

TEST_F(InitialSyncTest, Complete) {
    /**
     * Initial Sync will issue these query/commands
     *   - startTS = oplog.rs->find().sort({$natural:-1}).limit(-1).next()["ts"]
     *   - listDatabases (foreach db do below)
     *   -- cloneDatabase (see DatabaseCloner tests).
     *   - endTS = oplog.rs->find().sort({$natural:-1}).limit(-1).next()["ts"]
     *   - ops = oplog.rs->find({ts:{$gte: startTS}}) (foreach op)
     *   -- if local doc is missing, getCollection(op.ns).findOne(_id:op.o2._id)
     *   - if any retries were done in the previous loop, endTS query again for minvalid
     *
     */

    const std::vector<BSONObj> responses = {
        // get latest oplog ts
        fromjson(
            "{ok:1, cursor:{id:0, ns:'local.oplog.rs', firstBatch:["
            "{ts:Timestamp(1,1), h:1, ns:'a.a', v:2, op:'i', o:{_id:1, a:1}}"
            "]}}"),
        // oplog fetcher find
        fromjson(
            "{ok:1, cursor:{id:1, ns:'local.oplog.rs', firstBatch:["
            "{ts:Timestamp(1,1), h:1, ns:'a.a', v:2, op:'i', o:{_id:1, a:1}}"
            "]}}"),
        // Clone Start
        // listDatabases
        fromjson("{ok:1, databases:[{name:'a'}]}"),
        // listCollections for "a"
        fromjson(
            "{ok:1, cursor:{id:0, ns:'a.$cmd.listCollections', firstBatch:["
            "{name:'a', options:{}} "
            "]}}"),
        // listIndexes:a
        fromjson(
            "{ok:1, cursor:{id:0, ns:'a.$cmd.listIndexes.a', firstBatch:["
            "{v:1, key:{_id:1}, name:'_id_', ns:'a.a'}"
            "]}}"),
        // find:a
        fromjson(
            "{ok:1, cursor:{id:0, ns:'a.a', firstBatch:["
            "{_id:1, a:1} "
            "]}}"),
        // Clone Done
        // get latest oplog ts
        fromjson(
            "{ok:1, cursor:{id:0, ns:'local.oplog.rs', firstBatch:["
            "{ts:Timestamp(2,2), h:1, ns:'b.c', v:2, op:'i', o:{_id:1, c:1}}"
            "]}}"),
        // Applier starts ...
    };
    startSync();
    setResponses(responses);
    playResponses();
    verifySync();
}

TEST_F(InitialSyncTest, MissingDocOnApplyCompletes) {
    DataReplicatorOptions opts;
    int applyCounter{0};
    _applierFn = [&](OperationContext* txn, const BSONObj& op) {
        if (++applyCounter == 1) {
            return Status(ErrorCodes::NoMatchingDocument, "failed: missing doc.");
        }
        return Status::OK();
    };

    const std::vector<BSONObj> responses = {
        // get latest oplog ts
        fromjson(
            "{ok:1, cursor:{id:0, ns:'local.oplog.rs', firstBatch:["
            "{ts:Timestamp(1,1), h:1, ns:'a.a', v:2, op:'i', o:{_id:1, a:1}}"
            "]}}"),
        // oplog fetcher find
        fromjson(
            "{ok:1, cursor:{id:1, ns:'local.oplog.rs', firstBatch:["
            "{ts:Timestamp(1,1), h:1, ns:'a.a', v:2, op:'u', o2:{_id:1}, o:{$set:{a:1}}}"
            "]}}"),
        // Clone Start
        // listDatabases
        fromjson("{ok:1, databases:[{name:'a'}]}"),
        // listCollections for "a"
        fromjson(
            "{ok:1, cursor:{id:0, ns:'a.$cmd.listCollections', firstBatch:["
            "{name:'a', options:{}} "
            "]}}"),
        // listIndexes:a
        fromjson(
            "{ok:1, cursor:{id:0, ns:'a.$cmd.listIndexes.a', firstBatch:["
            "{v:1, key:{_id:1}, name:'_id_', ns:'a.a'}"
            "]}}"),
        // find:a -- empty
        fromjson("{ok:1, cursor:{id:0, ns:'a.a', firstBatch:[]}}"),
        // Clone Done
        // get latest oplog ts
        fromjson(
            "{ok:1, cursor:{id:0, ns:'local.oplog.rs', firstBatch:["
            "{ts:Timestamp(2,2), h:1, ns:'b.c', v:2, op:'i', o:{_id:1, c:1}}"
            "]}}"),
        // Applier starts ...
        // missing doc fetch -- find:a {_id:1}
        fromjson(
            "{ok:1, cursor:{id:0, ns:'a.a', firstBatch:["
            "{_id:1, a:1} "
            "]}}"),
    };
    startSync();
    setResponses(responses);
    playResponses();
    verifySync(ErrorCodes::OK);
}

TEST_F(InitialSyncTest, Failpoint) {
    mongo::getGlobalFailPointRegistry()
        ->getFailPoint("failInitialSyncWithBadHost")
        ->setMode(FailPoint::alwaysOn);

    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")));

    Timestamp time1(100, 1);
    OpTime opTime1(time1, OpTime::kDefaultTerm);
    _myLastOpTime = opTime1;
    _memberState = MemberState::RS_SECONDARY;

    DataReplicator* dr = &(getDR());
    InitialSyncBackgroundRunner isbr(dr);
    isbr.run();
    ASSERT_EQ(isbr.getResult().getStatus().code(), ErrorCodes::InitialSyncFailure);

    mongo::getGlobalFailPointRegistry()
        ->getFailPoint("failInitialSyncWithBadHost")
        ->setMode(FailPoint::off);
}

TEST_F(InitialSyncTest, FailsOnClone) {
    const std::vector<BSONObj> responses = {
        // get latest oplog ts
        fromjson(
            "{ok:1, cursor:{id:0, ns:'local.oplog.rs', firstBatch:["
            "{ts:Timestamp(1,1), h:1, ns:'a.a', v:2, op:'i', o:{_id:1, a:1}}"
            "]}}"),
        // oplog fetcher find
        fromjson(
            "{ok:1, cursor:{id:1, ns:'local.oplog.rs', firstBatch:["
            "{ts:Timestamp(1,1), h:1, ns:'a.a', v:2, op:'i', o:{_id:1, a:1}}"
            "]}}"),
        // Clone Start
        // listDatabases
        fromjson("{ok:0}")};
    startSync();
    setResponses(responses);
    playResponses();
    verifySync(ErrorCodes::InitialSyncFailure);
}

class SteadyStateTest : public DataReplicatorTest {
protected:
    void _testOplogStartMissing(const BSONObj& oplogFetcherResponse) {
        DataReplicator& dr = getDR();
        auto net = getNet();
        net->enterNetwork();
        ASSERT_OK(dr.start());

        ASSERT_TRUE(net->hasReadyRequests());
        auto noi = net->getNextReadyRequest();
        scheduleNetworkResponse(noi, oplogFetcherResponse);
        net->runReadyNetworkOperations();
        ASSERT_EQUALS(MemberState(MemberState::RS_RECOVERING).toString(), _memberState.toString());
    }
};

TEST_F(SteadyStateTest, StartWhenInSteadyState) {
    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    ASSERT_OK(dr.start());
    ASSERT_EQUALS(toString(DataReplicatorState::Steady), toString(dr.getState()));
    ASSERT_EQUALS(ErrorCodes::IllegalOperation, dr.start().code());
}

TEST_F(SteadyStateTest, ShutdownAfterStart) {
    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    auto net = getNet();
    net->enterNetwork();
    ASSERT_OK(dr.start());
    ASSERT_TRUE(net->hasReadyRequests());
    getExecutor().shutdown();
    ASSERT_EQUALS(toString(DataReplicatorState::Steady), toString(dr.getState()));
    ASSERT_EQUALS(ErrorCodes::IllegalOperation, dr.start().code());
}

TEST_F(SteadyStateTest, RequestShutdownAfterStart) {
    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    auto net = getNet();
    net->enterNetwork();
    ASSERT_OK(dr.start());
    ASSERT_TRUE(net->hasReadyRequests());
    ASSERT_EQUALS(toString(DataReplicatorState::Steady), toString(dr.getState()));
    // Simulating an invalid remote oplog query response. This will invalidate the existing
    // sync source but that's fine because we're not testing oplog processing.
    scheduleNetworkResponse(BSON("ok" << 0));
    net->runReadyNetworkOperations();
    ASSERT_OK(dr.scheduleShutdown());
    net->exitNetwork();  // runs work item scheduled in 'scheduleShutdown()).
    dr.waitForShutdown();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
}

TEST_F(SteadyStateTest, ScheduleNextActionFailsAfterChoosingEmptySyncSource) {
    class TestSyncSourceSelector : public SyncSourceSelector {
    public:
        TestSyncSourceSelector(ReplicationExecutor* exec) : _exec(exec) {}
        void clearSyncSourceBlacklist() override {}
        HostAndPort chooseNewSyncSource() override {
            _exec->shutdown();
            return HostAndPort();
        }
        void blacklistSyncSource(const HostAndPort& host, Date_t until) override {}
        bool shouldChangeSyncSource(const HostAndPort& currentSource) override {
            return false;
        }
        ReplicationExecutor* _exec;
    };
    TestSyncSourceSelector* testSyncSourceSelector = new TestSyncSourceSelector(&getExecutor());
    _syncSourceSelector.reset(testSyncSourceSelector);

    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    auto net = getNet();
    net->enterNetwork();
    ASSERT_OK(dr.start());
    ASSERT_EQUALS(HostAndPort(), dr.getSyncSource());
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
}

class TestSyncSourceSelector2 : public SyncSourceSelector {
public:
    void clearSyncSourceBlacklist() override {}
    HostAndPort chooseNewSyncSource() override {
        return HostAndPort(str::stream() << "host-" << _sourceNum++, -1);
    }
    void blacklistSyncSource(const HostAndPort& host, Date_t until) override {
        _blacklistedSource = host;
    }
    bool shouldChangeSyncSource(const HostAndPort& currentSource) override {
        return false;
    }
    int _sourceNum{0};
    HostAndPort _blacklistedSource;
};

TEST_F(SteadyStateTest, ChooseNewSyncSourceAfterFailedNetworkRequest) {
    TestSyncSourceSelector2* testSyncSourceSelector = new TestSyncSourceSelector2();
    _syncSourceSelector.reset(testSyncSourceSelector);

    DataReplicator& dr = getDR();
    ASSERT_EQUALS(toString(DataReplicatorState::Uninitialized), toString(dr.getState()));
    auto net = getNet();
    net->enterNetwork();
    ASSERT_OK(dr.start());
    ASSERT_TRUE(net->hasReadyRequests());
    ASSERT_EQUALS(toString(DataReplicatorState::Steady), toString(dr.getState()));
    // Simulating an invalid remote oplog query response to cause the data replicator to
    // blacklist the existing sync source and request a new one.
    scheduleNetworkResponse(BSON("ok" << 0));
    net->runReadyNetworkOperations();
    ASSERT_EQUALS(HostAndPort("host-0", -1), testSyncSourceSelector->_blacklistedSource);
    ASSERT_EQUALS(HostAndPort("host-1", -1), dr.getSyncSource());
}

TEST_F(SteadyStateTest, RemoteOplogEmpty) {
    _testOplogStartMissing(fromjson("{ok:1, cursor:{id:0, ns:'local.oplog.rs', firstBatch: []}}"));
}

TEST_F(SteadyStateTest, RemoteOplogFirstOperationMissingTimestamp) {
    _testOplogStartMissing(
        fromjson("{ok:1, cursor:{id:0, ns:'local.oplog.rs', firstBatch: [{}]}}"));
}

TEST_F(SteadyStateTest, RemoteOplogFirstOperationTimestampDoesNotMatch) {
    _testOplogStartMissing(fromjson(
        "{ok:1, cursor:{id:0, ns:'local.oplog.rs', "
        "firstBatch: [{ts:Timestamp(1,1)}]}}"));
}

TEST_F(SteadyStateTest, PauseDataReplicator) {
    auto operationToApply = BSON("op"
                                 << "a"
                                 << "ts" << Timestamp(Seconds(123), 0));
    stdx::mutex mutex;
    unittest::Barrier barrier(2U);
    Timestamp lastTimestampApplied;
    BSONObj operationApplied;
    _applierFn = [&](OperationContext* txn, const BSONObj& op) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        operationApplied = op;
        barrier.countDownAndWait();
        return Status::OK();
    };
    DataReplicatorOptions::SetMyLastOptimeFn oldSetMyLastOptime = _setMyLastOptime;
    _setMyLastOptime = [&](const OpTime& opTime) {
        oldSetMyLastOptime(opTime);
        stdx::lock_guard<stdx::mutex> lock(mutex);
        lastTimestampApplied = opTime.getTimestamp();
        barrier.countDownAndWait();
    };

    auto& dr = getDR();
    _myLastOpTime = OpTime(operationToApply["ts"].timestamp(), OpTime::kDefaultTerm);
    _memberState = MemberState::RS_SECONDARY;

    auto net = getNet();
    net->enterNetwork();

    ASSERT_OK(dr.start());

    ASSERT_TRUE(net->hasReadyRequests());
    {
        auto networkRequest = net->getNextReadyRequest();
        auto commandResponse = BSON(
            "ok" << 1 << "cursor" << BSON("id" << 0LL << "ns"
                                               << "local.oplog.rs"
                                               << "firstBatch" << BSON_ARRAY(operationToApply)));
        scheduleNetworkResponse(networkRequest, commandResponse);
    }

    dr.pause();

    ASSERT_EQUALS(0U, dr.getOplogBufferCount());

    // Data replication will process the fetcher response but will not schedule the applier.
    net->runReadyNetworkOperations();
    ASSERT_EQUALS(operationToApply["ts"].timestamp(), dr.getLastTimestampFetched());

    // Schedule a bogus work item to ensure that the operation applier function
    // is not scheduled.
    auto& exec = getExecutor();
    exec.scheduleWork(
        [&barrier](const executor::TaskExecutor::CallbackArgs&) { barrier.countDownAndWait(); });


    // Wake up executor thread and wait for bogus work callback to be invoked.
    net->exitNetwork();
    barrier.countDownAndWait();

    // Oplog buffer should contain fetched operations since applier is not scheduled.
    ASSERT_EQUALS(1U, dr.getOplogBufferCount());

    dr.resume();

    // Wait for applier function.
    barrier.countDownAndWait();
    // Run scheduleWork() work item scheduled in DataReplicator::_onApplyBatchFinish().
    net->exitNetwork();

    // Wait for batch completion callback.
    barrier.countDownAndWait();

    ASSERT_EQUALS(MemberState(MemberState::RS_SECONDARY).toString(), _memberState.toString());
    {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        ASSERT_EQUALS(operationToApply, operationApplied);
        ASSERT_EQUALS(operationToApply["ts"].timestamp(), lastTimestampApplied);
    }
}

TEST_F(SteadyStateTest, ApplyOneOperation) {
    auto operationToApply = BSON("op"
                                 << "a"
                                 << "ts" << Timestamp(Seconds(123), 0));
    stdx::mutex mutex;
    unittest::Barrier barrier(2U);
    Timestamp lastTimestampApplied;
    BSONObj operationApplied;
    _applierFn = [&](OperationContext* txn, const BSONObj& op) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        operationApplied = op;
        barrier.countDownAndWait();
        return Status::OK();
    };
    DataReplicatorOptions::SetMyLastOptimeFn oldSetMyLastOptime = _setMyLastOptime;
    _setMyLastOptime = [&](const OpTime& opTime) {
        oldSetMyLastOptime(opTime);
        stdx::lock_guard<stdx::mutex> lock(mutex);
        lastTimestampApplied = opTime.getTimestamp();
        barrier.countDownAndWait();
    };

    _myLastOpTime = OpTime(operationToApply["ts"].timestamp(), OpTime::kDefaultTerm);
    _memberState = MemberState::RS_SECONDARY;

    auto net = getNet();
    net->enterNetwork();

    auto& dr = getDR();
    ASSERT_OK(dr.start());

    ASSERT_TRUE(net->hasReadyRequests());
    {
        auto networkRequest = net->getNextReadyRequest();
        auto commandResponse = BSON(
            "ok" << 1 << "cursor" << BSON("id" << 0LL << "ns"
                                               << "local.oplog.rs"
                                               << "firstBatch" << BSON_ARRAY(operationToApply)));
        scheduleNetworkResponse(networkRequest, commandResponse);
    }
    ASSERT_EQUALS(0U, dr.getOplogBufferCount());

    // Oplog buffer should be empty because contents are transferred to applier.
    net->runReadyNetworkOperations();
    ASSERT_EQUALS(0U, dr.getOplogBufferCount());

    // Wait for applier function.
    barrier.countDownAndWait();
    ASSERT_EQUALS(operationToApply["ts"].timestamp(), dr.getLastTimestampFetched());
    // Run scheduleWork() work item scheduled in DataReplicator::_onApplyBatchFinish().
    net->exitNetwork();

    // Wait for batch completion callback.
    barrier.countDownAndWait();

    ASSERT_EQUALS(MemberState(MemberState::RS_SECONDARY).toString(), _memberState.toString());
    {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        ASSERT_EQUALS(operationToApply, operationApplied);
        ASSERT_EQUALS(operationToApply["ts"].timestamp(), lastTimestampApplied);
    }

    // Ensure that we send position information upstream after completing batch.
    net->enterNetwork();
    ASSERT_TRUE(net->hasReadyRequests());
    {
        auto networkRequest = net->getNextReadyRequest();
        auto commandRequest = networkRequest->getRequest();
        ASSERT_EQUALS("admin", commandRequest.dbname);
        const auto& cmdObj = commandRequest.cmdObj;
        ASSERT_EQUALS(std::string("replSetUpdatePosition"), cmdObj.firstElementFieldName());
    }
}

}  // namespace
