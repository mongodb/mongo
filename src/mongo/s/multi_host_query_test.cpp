/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/s/multi_host_query.h"

#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>

#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/synchronization.h"

namespace {

    using namespace mongo;
    using boost::scoped_ptr;
    using boost::shared_ptr;
    using std::make_pair;
    using std::map;
    using std::string;
    using std::vector;

    class CallbackCheck {
    public:

        enum LinkMode {
            None, Notify_Other, Wait_For_Other
        };

        CallbackCheck() :
            _status(ErrorCodes::OperationIncomplete, ""), _linkMode(None) {
        }

        void blockUntil(CallbackCheck* other) {

            _otherNotification.reset(new Notification);
            _linkMode = Wait_For_Other;

            other->_otherNotification = _otherNotification;
            other->_linkMode = Notify_Other;
        }

        HostThreadPool::Callback getCallback() {
            return stdx::bind(&CallbackCheck::noteCallback, this);
        }

        HostThreadPool::Callback getHostCallback(const ConnectionString& host) {
            return stdx::bind(&CallbackCheck::noteHostCallback, this, host);
        }

        void noteHostCallback(const ConnectionString& host) {
            _host = host;
            noteCallback();
        }

        void noteCallback() {

            _status = Status::OK();
            _notification.notifyOne();

            if (_linkMode == Wait_For_Other)
                _otherNotification->waitToBeNotified();
            else if (_linkMode == Notify_Other) {
                _otherNotification->notifyOne();
            }
        }

        void waitForCallback() {
            _notification.waitToBeNotified();
        }

        Status getStatus() {
            return _status;
        }

        const ConnectionString& getHost() {
            return _host;
        }

    private:

        Status _status;
        Notification _notification;

        ConnectionString _host;

        shared_ptr<Notification> _otherNotification;
        LinkMode _linkMode;
    };

    TEST(HostThreadPool, Schedule) {

        CallbackCheck cbCheck;

        // NOTE: pool must be initialized *after* the cbCheck that it executes - this avoids a
        // subtle race where the cbCheck structure is disposed before the callback is complete.
        HostThreadPool threadPool(1, true);

        threadPool.schedule(cbCheck.getCallback());

        cbCheck.waitForCallback();
        ASSERT_OK(cbCheck.getStatus());
    }

    TEST(HostThreadPool, ScheduleTwoSerial) {

        CallbackCheck cbCheckA;
        CallbackCheck cbCheckB;

        // NOTE: pool must be initialized *after* the cbCheck that it executes
        HostThreadPool threadPool(1, true);

        threadPool.schedule(cbCheckA.getCallback());
        threadPool.schedule(cbCheckB.getCallback());

        cbCheckB.waitForCallback();
        cbCheckA.waitForCallback();

        ASSERT_OK(cbCheckA.getStatus());
        ASSERT_OK(cbCheckB.getStatus());
    }

    TEST(HostThreadPool, ScheduleTwoParallel) {

        CallbackCheck cbCheckA;
        CallbackCheck cbCheckB;

        // NOTE: pool must be initialized *after* the cbCheck that it executes
        HostThreadPool threadPool(2, true);

        // Don't allow cbCheckA's callback to finish until cbCheckB's callback is processed
        cbCheckA.blockUntil(&cbCheckB);

        threadPool.schedule(cbCheckA.getCallback());
        cbCheckA.waitForCallback();
        ASSERT_OK(cbCheckA.getStatus());
        // We're still blocking the thread processing cbCheckA's callback

        threadPool.schedule(cbCheckB.getCallback());
        cbCheckB.waitForCallback();
        ASSERT_OK(cbCheckB.getStatus());
    }

    TEST(HostThreadPool, ScheduleTwoHosts) {

        CallbackCheck cbCheckA;
        CallbackCheck cbCheckB;

        // NOTE: pool must be initialized *after* the cbCheck that it executes
        HostThreadPools threadPool(1, true);

        // Don't allow cbCheckA's callback to finish until cbCheckB's callback is processed.
        // This means a single thread pool with a single thread would hang.
        cbCheckA.blockUntil(&cbCheckB);

        ConnectionString hostA = ConnectionString::mock(HostAndPort("$hostA:1000"));
        ConnectionString hostB = ConnectionString::mock(HostAndPort("$hostB:1000"));

        threadPool.schedule(hostA, cbCheckA.getHostCallback(hostA));
        cbCheckA.waitForCallback();
        ASSERT_OK(cbCheckA.getStatus());
        ASSERT_EQUALS(cbCheckA.getHost().toString(), hostA.toString());
        // We're still blocking the thread processing cbCheckA's callback

        threadPool.schedule(hostB, cbCheckB.getHostCallback(hostB));
        cbCheckB.waitForCallback();
        ASSERT_OK(cbCheckB.getStatus());
        ASSERT_EQUALS(cbCheckB.getHost().toString(), hostB.toString());
    }

    class MockSystemEnv : public MultiHostQueryOp::SystemEnv {
    private:

        struct MockHostInfo;
        typedef map<ConnectionString, MockHostInfo*> HostInfoMap;

    public:

        MockSystemEnv(HostThreadPools* threadPool) :
            _threadPool(threadPool), _mockTimeMillis(0) {
        }

        virtual ~MockSystemEnv() {
            for (HostInfoMap::iterator it = _mockHostInfo.begin(); it != _mockHostInfo.end();
                ++it) {
                if (_threadPool)
                    _threadPool->waitUntilIdle(it->first);
                delete it->second;
            }
        }

        void setHostThreadPools(HostThreadPools* threadPool) {
            _threadPool = threadPool;
        }

        void addMockHostResultAt(const ConnectionString& host, int timeMillis) {
            newMockHostResultAt(host, timeMillis, Status::OK(), NULL);
        }

        void addMockHostErrorAt(const ConnectionString& host, int timeMillis, Status error) {
            newMockHostResultAt(host, timeMillis, error, NULL);
        }

        void addMockHungHostAt(const ConnectionString& host,
                               int hangTimeMillis,
                               Notification* hangUntilNotify) {
            newMockHostResultAt(host, hangTimeMillis, Status::OK(), hangUntilNotify);
        }

        void addMockTimestepAt(int timeMillis) {

            // Add a mock query to a host we aren't using at the provided time
            ConnectionString host = ConnectionString::mock(HostAndPort("$timestepHost:1000"));
            newMockHostResultAt(host, timeMillis, Status::OK(), NULL);

            // The query won't be scheduled by the multi op, so we need to do so ourselves
            _threadPool->schedule(host,
                                  stdx::bind(&MockSystemEnv::doBlockingQuerySwallowResult,
                                              this,
                                              host,
                                              QuerySpec()));
        }

        Date_t currentTimeMillis() {
            return _mockTimeMillis;
        }

        void doBlockingQuerySwallowResult(const ConnectionString& host,
                                          const QuerySpec& query) {
            StatusWith<DBClientCursor*> result = doBlockingQuery(host, query);
            if (result.isOK())
                delete result.getValue();
        }

        StatusWith<DBClientCursor*> doBlockingQuery(const ConnectionString& host,
                                                    const QuerySpec& query) {

            ASSERT(_mockHostInfo.find(host) != _mockHostInfo.end());

            MockHostInfo& info = *(_mockHostInfo.find(host)->second);

            if (info.prevHostActiveNotify) {
                info.prevHostActiveNotify->waitToBeNotified();
                if (info.waitForPrevHostIdle) {
                    _threadPool->waitUntilIdle(info.prevHost);
                }
            }

            _mockTimeMillis = info.queryTimeMillis;

            if (info.nextHostActiveNotify) {
                info.nextHostActiveNotify->notifyOne();
            }

            if (info.hangUntilNotify) {
                info.hangUntilNotify->waitToBeNotified();
                return StatusWith<DBClientCursor*>(ErrorCodes::InternalError, "");
            }

            if (!info.error.isOK()) {
                return StatusWith<DBClientCursor*>(info.error);
            }

            //
            // Successful mock query
            //

            if (!info.conn) {
                info.conn.reset(new DBClientConnection(false));
                // Need to do a connect failure so that we get an empty MessagingPort on the conn and
                // the host name is set.
                string errMsg;
                ASSERT(!info.conn->connect(HostAndPort(host.toString()), errMsg));
            }
            
            return StatusWith<DBClientCursor*>(new DBClientCursor(info.conn.get(),
                                                                  query.ns(),
                                                                  query.query(),
                                                                  query.ntoreturn(),
                                                                  query.ntoskip(),
                                                                  query.fieldsPtr(),
                                                                  query.options(),
                                                                  0 /* batchSize */));
        }

    private:

        MockHostInfo* newMockHostResultAt(const ConnectionString& host,
                                          int timeMillis,
                                          const Status& error,
                                          Notification* hangUntilNotify) {

            ASSERT(_mockHostInfo.find(host) == _mockHostInfo.end());

            MockHostInfo* info = new MockHostInfo(timeMillis);
            _mockHostInfo.insert(make_pair(host, info));
            info->error = error;
            info->hangUntilNotify = hangUntilNotify;

            linkMockTimes(host, info);
            return info;
        }

        void linkMockTimes(const ConnectionString& host, MockHostInfo* info) {

            //
            // This just basically sets up notifications between the processing of results such that
            // the results are returned in the order defined by the _mockQueryTimes map.
            //
            // Idea is (second host result) waits for (first host result) thread to start and end,
            //         (third host result) waits for (second host result) thread to start and end,
            //         (fourth host result) waits for (third host result) thread to start and end,
            //         ... and so on ...
            //

            ASSERT(_mockQueryTimes.find(info->queryTimeMillis) == _mockQueryTimes.end());

            HostQueryTimes::iterator prev = _mockQueryTimes.insert(make_pair(info->queryTimeMillis,
                                                                             host)).first;

            if (prev != _mockQueryTimes.begin())
                --prev;
            else
                prev = _mockQueryTimes.end();

            HostQueryTimes::iterator next = _mockQueryTimes.upper_bound(info->queryTimeMillis);

            if (prev != _mockQueryTimes.end()) {

                const ConnectionString& prevHost = prev->second;
                MockHostInfo* prevInfo = _mockHostInfo.find(prevHost)->second;

                linkToNext(prevHost, prevInfo, info);
            }

            if (next != _mockQueryTimes.end()) {

                const ConnectionString& nextHost = next->second;
                MockHostInfo* nextInfo = _mockHostInfo.find(nextHost)->second;

                linkToNext(host, info, nextInfo);
            }
        }

        void linkToNext(const ConnectionString& host, MockHostInfo* info, MockHostInfo* nextInfo) {

            nextInfo->prevHost = host;

            nextInfo->prevHostActiveNotify.reset(new Notification());
            info->nextHostActiveNotify = nextInfo->prevHostActiveNotify.get();

            nextInfo->waitForPrevHostIdle = info->hangUntilNotify == NULL;
        }

        // Not owned here, needed to allow ordering of mock queries
        HostThreadPools* _threadPool;

        int _mockTimeMillis;

        typedef map<int, ConnectionString> HostQueryTimes;
        HostQueryTimes _mockQueryTimes;

        struct MockHostInfo {

            MockHostInfo(int queryTimeMillis) :
                nextHostActiveNotify( NULL),
                waitForPrevHostIdle(false),
                queryTimeMillis(queryTimeMillis),
                hangUntilNotify( NULL),
                error(Status::OK()) {
            }

            Notification* nextHostActiveNotify;

            ConnectionString prevHost;
            scoped_ptr<Notification> prevHostActiveNotify;
            bool waitForPrevHostIdle;

            int queryTimeMillis;

            scoped_ptr<DBClientConnection> conn;
            Notification* hangUntilNotify;
            Status error;
        };

        HostInfoMap _mockHostInfo;

    };

    QuerySpec buildSpec(const StringData& ns, const BSONObj& query) {
        return QuerySpec(ns.toString(), query, BSONObj(), 0, 0, 0);
    }

    //
    // Tests for the MultiHostQueryOp
    //

    TEST(MultiHostQueryOp, SingleHost) {

        HostThreadPools threadPool(1, true);
        MockSystemEnv mockSystem(&threadPool);

        ConnectionString host = ConnectionString::mock(HostAndPort("$host:1000"));
        vector<ConnectionString> hosts;
        hosts.push_back(host);

        mockSystem.addMockHostResultAt(host, 1000);

        MultiHostQueryOp queryOp(&mockSystem, &threadPool);

        QuerySpec query;
        StatusWith<DBClientCursor*> result = queryOp.queryAny(hosts, query, 2000);

        ASSERT_OK(result.getStatus());
        ASSERT(NULL != result.getValue());
        ASSERT_EQUALS(result.getValue()->originalHost(), host.toString());
        delete result.getValue();
    }

    TEST(MultiHostQueryOp, SingleHostError) {

        HostThreadPools threadPool(1, true);
        MockSystemEnv mockSystem(&threadPool);

        ConnectionString host = ConnectionString::mock(HostAndPort("$host:1000"));
        vector<ConnectionString> hosts;
        hosts.push_back(host);

        Status hostError = Status(ErrorCodes::InternalError, "");
        mockSystem.addMockHostErrorAt(host, 1000, hostError);

        MultiHostQueryOp queryOp(&mockSystem, &threadPool);

        QuerySpec query;
        StatusWith<DBClientCursor*> result = queryOp.queryAny(hosts, query, 2000);

        ASSERT_EQUALS(result.getStatus().code(), hostError.code());
    }

    TEST(MultiHostQueryOp, SingleHostHang) {

        // Initialize notifier before the thread pool, otherwise we may dispose while threads are
        // active
        Notification unhangNotify;

        HostThreadPools threadPool(1, true);
        MockSystemEnv mockSystem(&threadPool);

        ConnectionString host = ConnectionString::mock(HostAndPort("$host:1000"));
        vector<ConnectionString> hosts;
        hosts.push_back(host);

        mockSystem.addMockHungHostAt(host, 4000, &unhangNotify);

        MultiHostQueryOp queryOp(&mockSystem, &threadPool);

        QuerySpec query;
        StatusWith<DBClientCursor*> result = queryOp.queryAny(hosts, query, 2000);
        // Unhang before checking status, in case it throws
        unhangNotify.notifyOne();

        ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::NetworkTimeout);
    }

    TEST(MultiHostQueryOp, TwoHostResponses) {

        HostThreadPools threadPool(1, true);
        MockSystemEnv mockSystem(&threadPool);

        ConnectionString hostA = ConnectionString::mock(HostAndPort("$hostA:1000"));
        ConnectionString hostB = ConnectionString::mock(HostAndPort("$hostB:1000"));
        vector<ConnectionString> hosts;
        hosts.push_back(hostA);
        hosts.push_back(hostB);

        // Make sure we return the first response, from hostB at time 1000
        mockSystem.addMockHostResultAt(hostA, 2000);
        mockSystem.addMockHostResultAt(hostB, 1000);

        MultiHostQueryOp queryOp(&mockSystem, &threadPool);

        QuerySpec query;
        StatusWith<DBClientCursor*> result = queryOp.queryAny(hosts, query, 3000);

        ASSERT_OK(result.getStatus());
        ASSERT(NULL != result.getValue());
        ASSERT_EQUALS(result.getValue()->originalHost(), hostB.toString());
        delete result.getValue();
    }

    TEST(MultiHostQueryOp, TwoHostsOneErrorResponse) {

        HostThreadPools threadPool(1, true);
        MockSystemEnv mockSystem(&threadPool);

        ConnectionString hostA = ConnectionString::mock(HostAndPort("$hostA:1000"));
        ConnectionString hostB = ConnectionString::mock(HostAndPort("$hostB:1000"));
        vector<ConnectionString> hosts;
        hosts.push_back(hostA);
        hosts.push_back(hostB);

        // The first response is a host error, the second is a successful result
        Status hostError = Status(ErrorCodes::InternalError, "");
        mockSystem.addMockHostErrorAt(hostA, 1000, hostError);
        mockSystem.addMockHostResultAt(hostB, 2000);

        MultiHostQueryOp queryOp(&mockSystem, &threadPool);

        QuerySpec query;
        StatusWith<DBClientCursor*> result = queryOp.queryAny(hosts, query, 3000);

        ASSERT_OK(result.getStatus());
        ASSERT(NULL != result.getValue());
        ASSERT_EQUALS(result.getValue()->originalHost(), hostB.toString());
        delete result.getValue();
    }

    TEST(MultiHostQueryOp, TwoHostsBothErrors) {

        HostThreadPools threadPool(1, true);
        MockSystemEnv mockSystem(&threadPool);

        ConnectionString hostA = ConnectionString::mock(HostAndPort("$hostA:1000"));
        ConnectionString hostB = ConnectionString::mock(HostAndPort("$hostB:1000"));
        vector<ConnectionString> hosts;
        hosts.push_back(hostA);
        hosts.push_back(hostB);

        // Both responses are errors
        Status hostError = Status(ErrorCodes::InternalError, "");
        mockSystem.addMockHostErrorAt(hostA, 1000, hostError);
        mockSystem.addMockHostErrorAt(hostB, 2000, hostError);

        MultiHostQueryOp queryOp(&mockSystem, &threadPool);

        QuerySpec query;
        StatusWith<DBClientCursor*> result = queryOp.queryAny(hosts, query, 3000);

        ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::MultipleErrorsOccurred);
    }

    TEST(MultiHostQueryOp, TwoHostsOneHang) {

        // Initialize notifier before the thread pool
        Notification unhangNotify;

        HostThreadPools threadPool(1, true);
        MockSystemEnv mockSystem(&threadPool);

        ConnectionString hostA = ConnectionString::mock(HostAndPort("$hostA:1000"));
        ConnectionString hostB = ConnectionString::mock(HostAndPort("$hostB:1000"));
        vector<ConnectionString> hosts;
        hosts.push_back(hostA);
        hosts.push_back(hostB);

        // One host hangs
        mockSystem.addMockHungHostAt(hostA, 1000, &unhangNotify);
        mockSystem.addMockHostResultAt(hostB, 2000);

        MultiHostQueryOp queryOp(&mockSystem, &threadPool);

        QuerySpec query;
        StatusWith<DBClientCursor*> result = queryOp.queryAny(hosts, query, 3000);
        // Unhang before checking status, in case it throws
        unhangNotify.notifyOne();

        ASSERT_OK(result.getStatus());
        ASSERT(NULL != result.getValue());
        ASSERT_EQUALS(result.getValue()->originalHost(), hostB.toString());
        delete result.getValue();
    }

    TEST(MultiHostQueryOp, TwoHostsOneHangOneError) {

        // Initialize notifier before the thread pool
        Notification unhangNotify;

        HostThreadPools threadPool(1, true);
        MockSystemEnv mockSystem(&threadPool);

        ConnectionString hostA = ConnectionString::mock(HostAndPort("$hostA:1000"));
        ConnectionString hostB = ConnectionString::mock(HostAndPort("$hostB:1000"));
        vector<ConnectionString> hosts;
        hosts.push_back(hostA);
        hosts.push_back(hostB);

        // One host hangs, one host has an error (at the mock timeout point so the query finishes)
        Status hostError = Status(ErrorCodes::InternalError, "");
        mockSystem.addMockHungHostAt(hostA, 1000, &unhangNotify);
        mockSystem.addMockHostErrorAt(hostB, 3000, hostError);
        mockSystem.addMockTimestepAt(4000);

        MultiHostQueryOp queryOp(&mockSystem, &threadPool);

        QuerySpec query;
        StatusWith<DBClientCursor*> result = queryOp.queryAny(hosts, query, 4000);
        // Unhang before checking status, in case it throws
        unhangNotify.notifyOne();

        ASSERT_EQUALS(result.getStatus().code(), hostError.code());
    }

    TEST(MultiHostQueryOp, ThreeHostsOneHang) {

        // Initialize notifier before the thread pool
        Notification unhangNotify;

        HostThreadPools threadPool(1, true);
        MockSystemEnv mockSystem(&threadPool);

        ConnectionString hostA = ConnectionString::mock(HostAndPort("$hostA:1000"));
        ConnectionString hostB = ConnectionString::mock(HostAndPort("$hostB:1000"));
        ConnectionString hostC = ConnectionString::mock(HostAndPort("$hostC:1000"));
        vector<ConnectionString> hosts;
        hosts.push_back(hostA);
        hosts.push_back(hostB);
        hosts.push_back(hostC);

        // One host hangs, last host is fastest with result
        mockSystem.addMockHungHostAt(hostA, 1000, &unhangNotify);
        mockSystem.addMockHostResultAt(hostB, 3000);
        mockSystem.addMockHostResultAt(hostC, 2000);

        MultiHostQueryOp queryOp(&mockSystem, &threadPool);

        QuerySpec query;
        StatusWith<DBClientCursor*> result = queryOp.queryAny(hosts, query, 4000);
        // Unhang before checking status, in case it throws
        unhangNotify.notifyOne();

        ASSERT_OK(result.getStatus());
        ASSERT(NULL != result.getValue());
        ASSERT_EQUALS(result.getValue()->originalHost(), hostC.toString());
        delete result.getValue();
    }

    TEST(MultiHostQueryOp, ThreeHostsTwoErrors) {

        // Initialize notifier before the thread pool
        Notification unhangNotify;

        HostThreadPools threadPool(1, true);
        MockSystemEnv mockSystem(&threadPool);

        ConnectionString hostA = ConnectionString::mock(HostAndPort("$hostA:1000"));
        ConnectionString hostB = ConnectionString::mock(HostAndPort("$hostB:1000"));
        ConnectionString hostC = ConnectionString::mock(HostAndPort("$hostC:1000"));
        vector<ConnectionString> hosts;
        hosts.push_back(hostA);
        hosts.push_back(hostB);
        hosts.push_back(hostC);

        // One host hangs, two hosts have errors (finish at mock timeout point so query ends)
        Status hostError = Status(ErrorCodes::InternalError, "");
        mockSystem.addMockHungHostAt(hostA, 1000, &unhangNotify);
        mockSystem.addMockHostErrorAt(hostB, 4000, hostError);
        mockSystem.addMockHostErrorAt(hostC, 2000, hostError);
        mockSystem.addMockTimestepAt(5000);

        MultiHostQueryOp queryOp(&mockSystem, &threadPool);

        QuerySpec query;
        StatusWith<DBClientCursor*> result = queryOp.queryAny(hosts, query, 5000);
        // Unhang before checking status, in case it throws
        unhangNotify.notifyOne();

        ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::MultipleErrorsOccurred);
    }

    TEST(MultiHostQueryOp, ThreeHostsOneHangOneError) {

        // Initialize notifier before the thread pool
        Notification unhangNotify;

        HostThreadPools threadPool(1, true);
        MockSystemEnv mockSystem(&threadPool);

        ConnectionString hostA = ConnectionString::mock(HostAndPort("$hostA:1000"));
        ConnectionString hostB = ConnectionString::mock(HostAndPort("$hostB:1000"));
        ConnectionString hostC = ConnectionString::mock(HostAndPort("$hostC:1000"));
        vector<ConnectionString> hosts;
        hosts.push_back(hostA);
        hosts.push_back(hostB);
        hosts.push_back(hostC);

        // One host hangs, two hosts have errors (finish at mock timeout point so query ends)
        Status hostError = Status(ErrorCodes::InternalError, "");
        mockSystem.addMockHungHostAt(hostA, 1000, &unhangNotify);
        mockSystem.addMockHostErrorAt(hostB, 2000, hostError);
        mockSystem.addMockHostResultAt(hostC, 3000);

        MultiHostQueryOp queryOp(&mockSystem, &threadPool);

        QuerySpec query;
        StatusWith<DBClientCursor*> result = queryOp.queryAny(hosts, query, 4000);
        // Unhang before checking status, in case it throws
        unhangNotify.notifyOne();

        ASSERT_OK(result.getStatus());
        ASSERT(NULL != result.getValue());
        ASSERT_EQUALS(result.getValue()->originalHost(), hostC.toString());
        delete result.getValue();
    }

    TEST(MultiHostQueryOp, TwoHostsOneHangUnscoped) {

        // Initialize notifier before the thread pool
        Notification unhangNotify;

        // Create a thread pool which detaches itself from outstanding work on cleanup
        scoped_ptr<HostThreadPools> threadPool(new HostThreadPools(1, false));
        MockSystemEnv mockSystem(threadPool.get());

        ConnectionString hostA = ConnectionString::mock(HostAndPort("$hostA:1000"));
        ConnectionString hostB = ConnectionString::mock(HostAndPort("$hostB:1000"));
        vector<ConnectionString> hosts;
        hosts.push_back(hostA);
        hosts.push_back(hostB);

        // One host hangs
        mockSystem.addMockHungHostAt(hostA, 1000, &unhangNotify);
        mockSystem.addMockHostResultAt(hostB, 2000);

        MultiHostQueryOp queryOp(&mockSystem, threadPool.get());

        QuerySpec query;
        StatusWith<DBClientCursor*> result = queryOp.queryAny(hosts, query, 3000);

        // Clean up the thread pool
        mockSystem.setHostThreadPools( NULL);
        threadPool.reset();

        // Unhang before checking status, in case it throws
        unhangNotify.notifyOne();

        ASSERT_OK(result.getStatus());
        ASSERT(NULL != result.getValue());
        ASSERT_EQUALS(result.getValue()->originalHost(), hostB.toString());
        delete result.getValue();

        // Make sure we get the next result
        result = queryOp.waitForNextResult(4000);

        ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::InternalError);
    }

} // unnamed namespace
