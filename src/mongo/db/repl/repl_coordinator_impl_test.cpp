/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include <boost/scoped_ptr.hpp>
#include <boost/thread.hpp>
#include <memory>
#include <set>
#include <vector>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/repl_coordinator_external_state_mock.h"
#include "mongo/db/repl/repl_coordinator_impl.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

    // So that you can ASSERT_EQUALS two OpTimes
    std::ostream& operator<<( std::ostream &s, const OpTime &ot ) {
        s << ot.toString();
        return s;
    }
    // So that you can ASSERT_EQUALS two Date_ts
    std::ostream& operator<<( std::ostream &s, const Date_t &t ) {
        s << t.toString();
        return s;
    }

namespace repl {
namespace {

    bool stringContains(const std::string &haystack, const std::string& needle) {
        return haystack.find(needle) != std::string::npos;
    }

    const Seconds zeroSecs(0);

    class ReplCoordTest : public mongo::unittest::Test {
    public:
        ReplCoordTest() : _callShutdown(false) {}

        virtual void setUp() {
            _settings.replSet = "mySet/node1:12345,node2:54321";
        }

        virtual void tearDown() {
            if (_callShutdown) {
                shutdown();
            }
        }

    protected:
        NetworkInterfaceMockWithMap* getNet() { return _net; }
        ReplicationCoordinatorImpl* getReplCoord() {return _repl.get();}
        TopologyCoordinatorImpl& getTopoCoord() {return *_topo;}

        void init() {
            invariant(!_repl);
            invariant(!_callShutdown);
            _topo = new TopologyCoordinatorImpl(zeroSecs);
            _net = new NetworkInterfaceMockWithMap;
            _externalState = new ReplicationCoordinatorExternalStateMock;
            _repl.reset(new ReplicationCoordinatorImpl(_settings,
                                                       _externalState,
                                                       _net,
                                                       _topo));
        }

        void init(ReplSettings settings) {
            _settings = settings;
            init();
        }

        void init(const std::string& replSet) {
            _settings.replSet = replSet;
            init();
        }

        void start() {
            invariant(!_callShutdown);
            // if we haven't initialized yet, do that first.
            if (!_repl) {
                init();
            }

            OperationContextNoop txn;
            _repl->startReplication(&txn);
            _repl->waitForStartUpComplete();
            _callShutdown = true;
        }

        void start(const BSONObj& configDoc, const HostAndPort& selfHost) {
            if (!_repl) {
                init();
            }
            _externalState->setLocalConfigDocument(StatusWith<BSONObj>(configDoc));
            _externalState->addSelf(selfHost);
            start();
        }

        void start(const HostAndPort& selfHost) {
            if (!_repl) {
                init();
            }
            _externalState->addSelf(selfHost);
            start();
        }

        void assertStart(ReplicationCoordinator::Mode expectedMode,
                         const BSONObj& configDoc,
                         const HostAndPort& selfHost) {
            start(configDoc, selfHost);
            ASSERT_EQUALS(expectedMode, getReplCoord()->getReplicationMode());
        }

        void assertStartSuccess(const BSONObj& configDoc, const HostAndPort& selfHost) {
            assertStart(ReplicationCoordinator::modeReplSet, configDoc, selfHost);
        }

        void shutdown() {
            invariant(_callShutdown);
            _repl->shutdown();
            _callShutdown = false;
        }

        int64_t countLogLinesContaining(const std::string& needle) {
            return std::count_if(getCapturedLogMessages().begin(),
                                 getCapturedLogMessages().end(),
                                 stdx::bind(stringContains,
                                            stdx::placeholders::_1,
                                            needle));
        }

    private:
        boost::scoped_ptr<ReplicationCoordinatorImpl> _repl;
        // Owned by ReplicationCoordinatorImpl
        TopologyCoordinatorImpl* _topo;
        // Owned by ReplicationCoordinatorImpl
        NetworkInterfaceMockWithMap* _net;
        // Owned by ReplicationCoordinatorImpl
        ReplicationCoordinatorExternalStateMock* _externalState;
        ReplSettings _settings;
        bool _callShutdown;
    };

    TEST_F(ReplCoordTest, StartupWithValidLocalConfig) {
        assertStart(
                ReplicationCoordinator::modeReplSet,
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345"))),
                HostAndPort("node1", 12345));
    }

    TEST_F(ReplCoordTest, StartupWithInvalidLocalConfig) {
        startCapturingLogMessages();
        assertStart(ReplicationCoordinator::modeNone,
                    BSON("_id" << "mySet"), HostAndPort("node1", 12345));
        stopCapturingLogMessages();
        ASSERT_EQUALS(1, countLogLinesContaining("configuration does not parse"));
    }

    TEST_F(ReplCoordTest, StartupWithConfigMissingSelf) {
        startCapturingLogMessages();
        assertStart(
                ReplicationCoordinator::modeNone,
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345") <<
                                             BSON("_id" << 2 << "host" << "node2:54321"))),
                HostAndPort("node3", 12345));
        stopCapturingLogMessages();
        ASSERT_EQUALS(1, countLogLinesContaining("NodeNotFound"));
    }

    TEST_F(ReplCoordTest, StartupWithLocalConfigSetNameMismatch) {
        init("mySet");
        startCapturingLogMessages();
        assertStart(ReplicationCoordinator::modeNone,
                    BSON("_id" << "notMySet" <<
                         "version" << 2 <<
                         "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345"))),
                    HostAndPort("node1", 12345));
        stopCapturingLogMessages();
        ASSERT_EQUALS(1, countLogLinesContaining("reports set name of notMySet,"));
    }

    TEST_F(ReplCoordTest, StartupWithNoLocalConfig) {
        startCapturingLogMessages();
        start();
        stopCapturingLogMessages();
        ASSERT_EQUALS(1, countLogLinesContaining("Did not find local "));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, InitiateFailsWithEmptyConfig) {
        OperationContextNoop txn;
        init("mySet");
        start(HostAndPort("node1", 12345));
        BSONObjBuilder result;
        ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                      getReplCoord()->processReplSetInitiate(&txn, BSONObj(), &result));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, InitiateSucceedsWithOneNodeConfig) {
        OperationContextNoop txn;
        init("mySet");
        start(HostAndPort("node1", 12345));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());

        // Starting uninitialized, show that we can perform the initiate behavior.
        BSONObjBuilder result1;
        ASSERT_OK(getReplCoord()->processReplSetInitiate(
                          &txn,
                          BSON("_id" << "mySet" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(
                                       BSON("_id" << 0 << "host" << "node1:12345"))),
                          &result1));
        ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());

        // Show that initiate fails after it has already succeeded.
        BSONObjBuilder result2;
        ASSERT_EQUALS(ErrorCodes::AlreadyInitialized,
                      getReplCoord()->processReplSetInitiate(
                              &txn,
                              BSON("_id" << "mySet" <<
                                   "version" << 1 <<
                                   "members" << BSON_ARRAY(
                                           BSON("_id" << 0 << "host" << "node1:12345"))),
                              &result2));

        // Still in repl set mode, even after failed reinitiate.
        ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, InitiateSucceedsAfterFailing) {
        OperationContextNoop txn;
        init("mySet");
        start(HostAndPort("node1", 12345));
        BSONObjBuilder result;
        ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                      getReplCoord()->processReplSetInitiate(&txn, BSONObj(), &result));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());

        // Having failed to initiate once, show that we can now initiate.
        BSONObjBuilder result1;
        ASSERT_OK(getReplCoord()->processReplSetInitiate(
                          &txn,
                          BSON("_id" << "mySet" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(
                                       BSON("_id" << 0 << "host" << "node1:12345"))),
                          &result1));
        ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, InitiateFailsIfAlreadyInitialized) {
        OperationContextNoop txn;
        assertStart(
                ReplicationCoordinator::modeReplSet,
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345"))),
                HostAndPort("node1", 12345));
        BSONObjBuilder result;
        ASSERT_EQUALS(ErrorCodes::AlreadyInitialized,
                      getReplCoord()->processReplSetInitiate(
                              &txn,
                              BSON("_id" << "mySet" <<
                                   "version" << 2 <<
                                   "members" << BSON_ARRAY(BSON("_id" << 1 <<
                                                                "host" << "node1:12345"))),
                              &result));
    }

    TEST_F(ReplCoordTest, InitiateFailsIfSelfMissing) {
        OperationContextNoop txn;
        BSONObjBuilder result;
        init("mySet");
        start(HostAndPort("node1", 12345));
        ASSERT_EQUALS(ErrorCodes::NodeNotFound,
                      getReplCoord()->processReplSetInitiate(
                              &txn,
                              BSON("_id" << "mySet" <<
                                   "version" << 1 <<
                                   "members" << BSON_ARRAY(
                                           BSON("_id" << 0 << "host" << "node4"))),
                              &result));
    }

    TEST_F(ReplCoordTest, InitiateFailsIfQuorumNotMet) {
        OperationContextNoop txn;
        init("mySet");
        start(HostAndPort("node1", 12345));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());

        BSONObjBuilder result1;
        ASSERT_EQUALS(
                ErrorCodes::NodeNotFound,
                getReplCoord()->processReplSetInitiate(
                        &txn,
                        BSON("_id" << "mySet" <<
                             "version" << 1 <<
                             "members" << BSON_ARRAY(
                                     BSON("_id" << 0 << "host" << "node1:12345") <<
                                     BSON("_id" << 1 << "host" << "node2:54321"))),
                        &result1));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());
        getNet()->addResponse(
                ReplicationExecutor::RemoteCommandRequest(
                        HostAndPort("node2", 54321),
                        "admin",
                        BSON("replSetHeartbeat" << "mySet" <<
                             "v" << 1 <<
                             "pv" << 1 <<
                             "checkEmpty" << true <<
                             "from" << "node1:12345" <<
                             "fromId" << 0)),
                StatusWith<BSONObj>(BSON("ok" << 1)));

        ASSERT_OK(
                getReplCoord()->processReplSetInitiate(
                        &txn,
                        BSON("_id" << "mySet" <<
                             "version" << 1 <<
                             "members" << BSON_ARRAY(
                                     BSON("_id" << 0 << "host" << "node1:12345") <<
                                     BSON("_id" << 1 << "host" << "node2:54321"))),
                        &result1));
        ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, InitiateFailsWithSetNameMismatch) {
        OperationContextNoop txn;
        init("mySet");
        start(HostAndPort("node1", 12345));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());

        BSONObjBuilder result1;
        ASSERT_EQUALS(
                ErrorCodes::BadValue,
                getReplCoord()->processReplSetInitiate(
                        &txn,
                        BSON("_id" << "wrongSet" <<
                             "version" << 1 <<
                             "members" << BSON_ARRAY(
                                     BSON("_id" << 0 << "host" << "node1:12345"))),
                        &result1));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, AwaitReplicationNumberBaseCases) {
        init("");
        OperationContextNoop txn;
        OpTime time(1, 1);

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
        writeConcern.wNumNodes = 2;

        // Because we didn't set ReplSettings.replSet, it will think we're a standalone so
        // awaitReplication will always work.
        ReplicationCoordinator::StatusAndDuration statusAndDur =
                                        getReplCoord()->awaitReplication(&txn, time, writeConcern);
        ASSERT_OK(statusAndDur.status);

        // Now make us a master in master/slave
        getReplCoord()->getSettings().master = true;

        writeConcern.wNumNodes = 0;
        writeConcern.wMode = "majority";
        // w:majority always works on master/slave
        statusAndDur = getReplCoord()->awaitReplication(&txn, time, writeConcern);
        ASSERT_OK(statusAndDur.status);

        // Now make us a replica set
        getReplCoord()->getSettings().replSet = "mySet/node1:12345,node2:54321";

        // Waiting for 1 nodes always works
        writeConcern.wNumNodes = 1;
        writeConcern.wMode = "";
        statusAndDur = getReplCoord()->awaitReplication(&txn, time, writeConcern);
        ASSERT_OK(statusAndDur.status);
    }

    TEST_F(ReplCoordTest, CheckReplEnabledForCommandNotRepl) {
        // pass in settings to avoid having a replSet
        ReplSettings settings;
        init(settings);
        start();

        // check status NoReplicationEnabled and empty result
        BSONObjBuilder result;
        Status status = getReplCoord()->checkReplEnabledForCommand(&result);
        ASSERT_EQUALS(status, ErrorCodes::NoReplicationEnabled);
        ASSERT_TRUE(result.obj().isEmpty());
    }

    TEST_F(ReplCoordTest, checkReplEnabledForCommandConfigSvr) {
        ReplSettings settings;
        serverGlobalParams.configsvr = true;
        init(settings);
        start();

        // check status NoReplicationEnabled and result mentions configsrv
        BSONObjBuilder result;
        Status status = getReplCoord()->checkReplEnabledForCommand(&result);
        ASSERT_EQUALS(status, ErrorCodes::NoReplicationEnabled);
        ASSERT_EQUALS(result.obj()["info"].String(), "configsvr");
        serverGlobalParams.configsvr = false;
    }

    TEST_F(ReplCoordTest, checkReplEnabledForCommandNoConfig) {
        start();

        // check status NotYetInitialized and result mentions rs.initiate
        BSONObjBuilder result;
        Status status = getReplCoord()->checkReplEnabledForCommand(&result);
        ASSERT_EQUALS(status, ErrorCodes::NotYetInitialized);
        ASSERT_TRUE(result.obj()["info"].String().find("rs.initiate") != std::string::npos);
    }

    TEST_F(ReplCoordTest, checkReplEnabledForCommandWorking) {
        assertStartSuccess(BSON("_id" << "mySet" <<
                                "version" << 2 <<
                                "members" << BSON_ARRAY(BSON("host" << "node1:12345" <<
                                                             "_id" << 0 ))),
                    HostAndPort("node1", 12345));

        // check status OK and result is empty
        BSONObjBuilder result;
        Status status = getReplCoord()->checkReplEnabledForCommand(&result);
        ASSERT_EQUALS(status, Status::OK());
        ASSERT_TRUE(result.obj().isEmpty());
    }

    TEST_F(ReplCoordTest, BasicRBIDUsage) {
        start();
        BSONObjBuilder result;
        getReplCoord()->processReplSetGetRBID(&result);
        long long initialValue = result.obj()["rbid"].Int();
        getReplCoord()->incrementRollbackID();

        BSONObjBuilder result2;
        getReplCoord()->processReplSetGetRBID(&result2);
        long long incrementedValue = result2.obj()["rbid"].Int();
        ASSERT_EQUALS(incrementedValue, initialValue + 1);
    }

    TEST_F(ReplCoordTest, AwaitReplicationNumberOfNodesNonBlocking) {
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("host" << "node1:12345" << "_id" << 0 ))),
                HostAndPort("node1", 12345));
        OperationContextNoop txn;

        OID client1 = OID::gen();
        OID client2 = OID::gen();
        OID client3 = OID::gen();
        OpTime time1(1, 1);
        OpTime time2(1, 2);

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
        writeConcern.wNumNodes = 2;

        // 2 nodes waiting for time1
        ReplicationCoordinator::StatusAndDuration statusAndDur =
                                        getReplCoord()->awaitReplication(&txn, time1, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client1, time1));
        statusAndDur = getReplCoord()->awaitReplication(&txn, time1, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client2, time1));
        statusAndDur = getReplCoord()->awaitReplication(&txn, time1, writeConcern);
        ASSERT_OK(statusAndDur.status);

        // 2 nodes waiting for time2
        statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client2, time2));
        statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client3, time2));
        statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
        ASSERT_OK(statusAndDur.status);

        // 3 nodes waiting for time2
        writeConcern.wNumNodes = 3;
        statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client1, time2));
        statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
        ASSERT_OK(statusAndDur.status);
    }

    /**
     * Used to wait for replication in a separate thread without blocking execution of the test.
     * To use, set the optime and write concern to be passed to awaitReplication and then call
     * start(), which will spawn a thread that calls awaitReplication.  No calls may be made
     * on the ReplicationAwaiter instance between calling start and getResult().  After returning
     * from getResult(), you can call reset() to allow the awaiter to be reused for another
     * awaitReplication call.
     */
    class ReplicationAwaiter {
    public:

        ReplicationAwaiter(ReplicationCoordinatorImpl* replCoord, OperationContext* txn) :
            _replCoord(replCoord), _finished(false),
            _result(ReplicationCoordinator::StatusAndDuration(
                    Status::OK(), ReplicationCoordinator::Milliseconds(0))) {}

        void setOpTime(const OpTime& ot) {
            _optime = ot;
        }

        void setWriteConcern(const WriteConcernOptions& wc) {
            _writeConcern = wc;
        }

        // may block
        ReplicationCoordinator::StatusAndDuration getResult() {
            _thread->join();
            ASSERT(_finished);
            return _result;
        }

        void start(OperationContext* txn) {
            ASSERT(!_finished);
            _thread.reset(new boost::thread(stdx::bind(&ReplicationAwaiter::_awaitReplication,
                                                       this,
                                                       txn)));
        }

        void reset() {
            ASSERT(_finished);
            _finished = false;
            _result = ReplicationCoordinator::StatusAndDuration(
                    Status::OK(), ReplicationCoordinator::Milliseconds(0));
        }

    private:

        void _awaitReplication(OperationContext* txn) {
            _result = _replCoord->awaitReplication(txn, _optime, _writeConcern);
            _finished = true;
        }

        ReplicationCoordinatorImpl* _replCoord;
        bool _finished;
        OpTime _optime;
        WriteConcernOptions _writeConcern;
        ReplicationCoordinator::StatusAndDuration _result;
        boost::scoped_ptr<boost::thread> _thread;
    };

    TEST_F(ReplCoordTest, AwaitReplicationNumberOfNodesBlocking) {
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("host" << "node1:12345" << "_id" << 0 ))),
                HostAndPort("node1", 12345));

        OperationContextNoop txn;
        ReplicationAwaiter awaiter(getReplCoord(), &txn);

        OID client1 = OID::gen();
        OID client2 = OID::gen();
        OID client3 = OID::gen();
        OpTime time1(1, 1);
        OpTime time2(1, 2);

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
        writeConcern.wNumNodes = 2;

        // 2 nodes waiting for time1
        awaiter.setOpTime(time1);
        awaiter.setWriteConcern(writeConcern);
        awaiter.start(&txn);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client1, time1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client2, time1));
        ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
        ASSERT_OK(statusAndDur.status);
        awaiter.reset();

        // 2 nodes waiting for time2
        awaiter.setOpTime(time2);
        awaiter.start(&txn);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client2, time2));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client3, time2));
        statusAndDur = awaiter.getResult();
        ASSERT_OK(statusAndDur.status);
        awaiter.reset();

        // 3 nodes waiting for time2
        writeConcern.wNumNodes = 3;
        awaiter.setWriteConcern(writeConcern);
        awaiter.start(&txn);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client1, time2));
        statusAndDur = awaiter.getResult();
        ASSERT_OK(statusAndDur.status);
        awaiter.reset();
    }

    TEST_F(ReplCoordTest, AwaitReplicationTimeout) {
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("host" << "node1:12345" << "_id" << 0 ))),
                HostAndPort("node1", 12345));
        OperationContextNoop txn;
        ReplicationAwaiter awaiter(getReplCoord(), &txn);

        OID client1 = OID::gen();
        OID client2 = OID::gen();
        OpTime time1(1, 1);
        OpTime time2(1, 2);

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = 50;
        writeConcern.wNumNodes = 2;

        // 2 nodes waiting for time2
        awaiter.setOpTime(time2);
        awaiter.setWriteConcern(writeConcern);
        awaiter.start(&txn);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client1, time1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client2, time1));
        ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        awaiter.reset();
    }

    TEST_F(ReplCoordTest, AwaitReplicationShutdown) {
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("host" << "node1:12345" << "_id" << 0 ))),
                HostAndPort("node1", 12345));
        OperationContextNoop txn;
        ReplicationAwaiter awaiter(getReplCoord(), &txn);

        OID client1 = OID::gen();
        OID client2 = OID::gen();
        OpTime time1(1, 1);
        OpTime time2(1, 2);

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
        writeConcern.wNumNodes = 2;

        // 2 nodes waiting for time2
        awaiter.setOpTime(time2);
        awaiter.setWriteConcern(writeConcern);
        awaiter.start(&txn);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client1, time1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client2, time1));
        shutdown();
        ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
        ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, statusAndDur.status);
        awaiter.reset();
    }

    class OperationContextNoopWithInterrupt : public OperationContextNoop {
    public:

        OperationContextNoopWithInterrupt() : _opID(0), _interruptOp(false) {}

        virtual unsigned int getOpID() const {
            return _opID;
        }

        /**
         * Can only be called before any multi-threaded access to this object has begun.
         */
        void setOpID(unsigned int opID) {
            _opID = opID;
        }

        virtual void checkForInterrupt(bool heedMutex = true) const {
            if (_interruptOp) {
                uasserted(ErrorCodes::Interrupted, "operation was interrupted");
            }
        }

        /**
         * Can only be called before any multi-threaded access to this object has begun.
         */
        void setInterruptOp(bool interrupt) {
            _interruptOp = interrupt;
        }

    private:
        unsigned int _opID;
        bool _interruptOp;
    };

    TEST_F(ReplCoordTest, AwaitReplicationInterrupt) {
        // Tests that a thread blocked in awaitReplication can be killed by a killOp operation
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("_id" << 0 << "host" << "node1") <<
                                             BSON("_id" << 1 << "host" << "node2") <<
                                             BSON("_id" << 2 << "host" << "node3"))),
                HostAndPort("node1"));
        OperationContextNoopWithInterrupt txn;
        ReplicationAwaiter awaiter(getReplCoord(), &txn);

        OID client1 = OID::gen();
        OID client2 = OID::gen();
        OpTime time1(1, 1);
        OpTime time2(1, 2);

        HandshakeArgs handshake1;
        ASSERT_OK(handshake1.initialize(BSON("handshake" << client1 << "member" << 1)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake1));
        HandshakeArgs handshake2;
        ASSERT_OK(handshake2.initialize(BSON("handshake" << client2 << "member" << 2)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake2));

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
        writeConcern.wNumNodes = 2;

        unsigned int opID = 100;
        txn.setOpID(opID);

        // 2 nodes waiting for time2
        awaiter.setOpTime(time2);
        awaiter.setWriteConcern(writeConcern);
        awaiter.start(&txn);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client1, time1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client2, time1));

        txn.setInterruptOp(true);
        getReplCoord()->interrupt(opID);
        ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
        ASSERT_EQUALS(ErrorCodes::Interrupted, statusAndDur.status);
        awaiter.reset();
    }

    TEST_F(ReplCoordTest, AwaitReplicationNamedModes) {
        // TODO(spencer): Test awaitReplication with w:majority and tag groups
        warning() << "Test ReplCoordTest.AwaitReplicationNamedModes needs to be written.";
    }

    TEST_F(ReplCoordTest, GetReplicationModeNone) {
        init();
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, GetReplicationModeMaster) {
        // modeMasterSlave if master set
        ReplSettings settings;
        settings.master = true;
        init(settings);
        ASSERT_EQUALS(ReplicationCoordinator::modeMasterSlave,
                      getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, GetReplicationModeSlave) {
        // modeMasterSlave if the slave flag was set
        ReplSettings settings;
        settings.slave = SimpleSlave;
        init(settings);
        ASSERT_EQUALS(ReplicationCoordinator::modeMasterSlave,
                      getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, GetReplicationModeRepl) {
        // modeReplSet only once config isInitialized
        ReplSettings settings;
        settings.replSet = "mySet/node1:12345";
        init(settings);
        ASSERT_EQUALS(ReplicationCoordinator::modeNone,
                      getReplCoord()->getReplicationMode());
        assertStart(
                ReplicationCoordinator::modeReplSet,
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("host" << "node1:12345" << "_id" << 0 ))),
                HostAndPort("node1", 12345));
    }

    TEST_F(ReplCoordTest, TestPrepareReplSetUpdatePositionCommand) {
        OperationContextNoop txn;
        init("mySet/test1:1234,test2:1234,test3:1234");
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(BSON("_id" << 0 << "host" << "test1:1234") <<
                                             BSON("_id" << 1 << "host" << "test2:1234") <<
                                             BSON("_id" << 2 << "host" << "test3:1234"))),
                HostAndPort("test1", 1234));
        OID rid1 = getReplCoord()->getMyRID(&txn);
        OID rid2 = OID::gen();
        OID rid3 = OID::gen();
        HandshakeArgs handshake2;
        handshake2.initialize(BSON("handshake" << rid2 <<
                                   "member" << 1 <<
                                   "config" << BSON("_id" << 1 << "host" << "test2:1234")));
        HandshakeArgs handshake3;
        handshake3.initialize(BSON("handshake" << rid3 <<
                                   "member" << 2 <<
                                   "config" << BSON("_id" << 2 << "host" << "test3:1234")));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake2));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake3));
        OpTime optime1(1, 1);
        OpTime optime2(1, 2);
        OpTime optime3(2, 1);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid1, optime1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid2, optime2));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid3, optime3));

        // Check that the proper BSON is generated for the replSetUpdatePositionCommand
        BSONObjBuilder cmdBuilder;
        getReplCoord()->prepareReplSetUpdatePositionCommand(&txn, &cmdBuilder);
        BSONObj cmd = cmdBuilder.done();

        ASSERT_EQUALS(2, cmd.nFields());
        ASSERT_EQUALS("replSetUpdatePosition", cmd.firstElement().fieldNameStringData());

        std::set<OID> rids;
        BSONForEach(entryElement, cmd["optimes"].Obj()) {
            BSONObj entry = entryElement.Obj();
            OID rid = entry["_id"].OID();
            rids.insert(rid);
            if (rid == rid1) {
                ASSERT_EQUALS(optime1, entry["optime"]._opTime());
            } else if (rid == rid2) {
                ASSERT_EQUALS(optime2, entry["optime"]._opTime());
            } else {
                ASSERT_EQUALS(rid3, rid);
                ASSERT_EQUALS(optime3, entry["optime"]._opTime());
            }
        }
        ASSERT_EQUALS(3U, rids.size()); // Make sure we saw all 3 nodes
    }

    TEST_F(ReplCoordTest, TestHandshakes) {
        init("mySet/test1:1234,test2:1234,test3:1234");
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(BSON("_id" << 0 << "host" << "test1:1234") <<
                                             BSON("_id" << 1 << "host" << "test2:1234") <<
                                             BSON("_id" << 2 << "host" << "test3:1234"))),
                HostAndPort("test2", 1234));
        // Test generating basic handshake with no chaining
        std::vector<BSONObj> handshakes;
        OperationContextNoop txn;
        getReplCoord()->prepareReplSetUpdatePositionCommandHandshakes(&txn, &handshakes);
        ASSERT_EQUALS(1U, handshakes.size());
        BSONObj handshakeCmd = handshakes[0];
        ASSERT_EQUALS(2, handshakeCmd.nFields());
        ASSERT_EQUALS("replSetUpdatePosition", handshakeCmd.firstElement().fieldNameStringData());
        BSONObj handshake = handshakeCmd["handshake"].Obj();
        ASSERT_EQUALS(getReplCoord()->getMyRID(&txn), handshake["handshake"].OID());
        ASSERT_EQUALS(1, handshake["member"].Int());
        handshakes.clear();

        // Have other nodes handshake us and make sure we process it right.
        OID slave1RID = OID::gen();
        OID slave2RID = OID::gen();
        HandshakeArgs slave1Handshake;
        slave1Handshake.initialize(BSON("handshake" << slave1RID <<
                                        "member" << 0 <<
                                        "config" << BSON("_id" << 0 << "host" << "test1:1234")));
        HandshakeArgs slave2Handshake;
        slave2Handshake.initialize(BSON("handshake" << slave2RID <<
                                        "member" << 2 <<
                                        "config" << BSON("_id" << 2 << "host" << "test2:1234")));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, slave1Handshake));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, slave2Handshake));

        getReplCoord()->prepareReplSetUpdatePositionCommandHandshakes(&txn, &handshakes);
        ASSERT_EQUALS(3U, handshakes.size());
        std::set<OID> rids;
        for (std::vector<BSONObj>::iterator it = handshakes.begin(); it != handshakes.end(); ++it) {
            BSONObj handshakeCmd = *it;
            ASSERT_EQUALS(2, handshakeCmd.nFields());
            ASSERT_EQUALS("replSetUpdatePosition",
                          handshakeCmd.firstElement().fieldNameStringData());

            BSONObj handshake = handshakeCmd["handshake"].Obj();
            OID rid = handshake["handshake"].OID();
            rids.insert(rid);
            if (rid == getReplCoord()->getMyRID(&txn)) {
                ASSERT_EQUALS(1, handshake["member"].Int());
            } else if (rid == slave1RID) {
                ASSERT_EQUALS(0, handshake["member"].Int());
            } else {
                ASSERT_EQUALS(slave2RID, rid);
                ASSERT_EQUALS(2, handshake["member"].Int());
            }
        }
        ASSERT_EQUALS(3U, rids.size()); // Make sure we saw all 3 nodes
    }

    TEST_F(ReplCoordTest, TestGetElectionId) {
        init("mySet/test1:1234,test2:1234,test3:1234");
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "test1:1234"))),
                HostAndPort("test1", 1234));
        OID electionID1 = getReplCoord()->getElectionId();
        getTopoCoord()._changeMemberState(MemberState::RS_PRIMARY);
        OID electionID2 = getReplCoord()->getElectionId();
        ASSERT_NOT_EQUALS(electionID1, electionID2);
    }

    // TODO(spencer): Unit test replSetFreeze

}  // namespace
}  // namespace repl
}  // namespace mongo
