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

#include "mongo/platform/basic.h"

#include <boost/scoped_ptr.hpp>
#include <boost/thread.hpp>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/repl_coordinator_external_state_mock.h"
#include "mongo/db/repl/repl_coordinator_impl.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

namespace {

    TEST(ReplicationCoordinator, StartupShutdown) {
        ReplSettings settings;
        // Make sure we think we're a replSet
        settings.replSet = "mySet/node1:12345,node2:54321";
        ReplicationCoordinatorImpl coordinator(settings,
                                               new ReplicationCoordinatorExternalStateMock);
        coordinator.startReplication(new TopologyCoordinatorImpl(0), new NetworkInterfaceMock);
        coordinator.shutdown();
    }

    TEST(ReplicationCoordinator, AwaitReplicationNumberBaseCases) {
        ReplSettings settings;
        ReplicationCoordinatorImpl coordinator(settings,
                                               new ReplicationCoordinatorExternalStateMock);
        OperationContextNoop txn;
        OpTime time(1, 1);

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
        writeConcern.wNumNodes = 2;

        // Because we didn't set ReplSettings.replSet, it will think we're a standalone so
        // awaitReplication will always work.
        ReplicationCoordinator::StatusAndDuration statusAndDur = coordinator.awaitReplication(
                &txn, time, writeConcern);
        ASSERT_OK(statusAndDur.status);

        // Now make us a master in master/slave
        coordinator.getSettings().master = true;

        writeConcern.wNumNodes = 0;
        writeConcern.wMode = "majority";
        // w:majority always works on master/slave
        statusAndDur = coordinator.awaitReplication(&txn, time, writeConcern);
        ASSERT_OK(statusAndDur.status);

        // Now make us a replica set
        coordinator.getSettings().replSet = "mySet/node1:12345,node2:54321";

        // Waiting for 1 nodes always works
        writeConcern.wNumNodes = 1;
        writeConcern.wMode = "";
        statusAndDur = coordinator.awaitReplication(&txn, time, writeConcern);
        ASSERT_OK(statusAndDur.status);
    }

    TEST(ReplicationCoordinator, AwaitReplicationNumberOfNodesNonBlocking) {
        ReplSettings settings;
        // Make sure we think we're a replSet
        settings.replSet = "mySet/node1:12345,node2:54321";
        ReplicationCoordinatorImpl coordinator(settings,
                                               new ReplicationCoordinatorExternalStateMock);
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
        ReplicationCoordinator::StatusAndDuration statusAndDur = coordinator.awaitReplication(
                &txn, time1, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(coordinator.setLastOptime(&txn, client1, time1));
        statusAndDur = coordinator.awaitReplication(&txn, time1, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(coordinator.setLastOptime(&txn, client2, time1));
        statusAndDur = coordinator.awaitReplication(&txn, time1, writeConcern);
        ASSERT_OK(statusAndDur.status);

        // 2 nodes waiting for time2
        statusAndDur = coordinator.awaitReplication(&txn, time2, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(coordinator.setLastOptime(&txn, client2, time2));
        statusAndDur = coordinator.awaitReplication(&txn, time2, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(coordinator.setLastOptime(&txn, client3, time2));
        statusAndDur = coordinator.awaitReplication(&txn, time2, writeConcern);
        ASSERT_OK(statusAndDur.status);

        // 3 nodes waiting for time2
        writeConcern.wNumNodes = 3;
        statusAndDur = coordinator.awaitReplication(&txn, time2, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(coordinator.setLastOptime(&txn, client1, time2));
        statusAndDur = coordinator.awaitReplication(&txn, time2, writeConcern);
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

        void start() {
            ASSERT(!_finished);
            _thread.reset(new boost::thread(stdx::bind(&ReplicationAwaiter::_awaitReplication,
                                                       this)));
        }

        void reset() {
            ASSERT(_finished);
            _finished = false;
            _result = ReplicationCoordinator::StatusAndDuration(
                    Status::OK(), ReplicationCoordinator::Milliseconds(0));
        }

    private:

        void _awaitReplication() {
            OperationContextNoop txn;
            _result = _replCoord->awaitReplication(&txn, _optime, _writeConcern);
            _finished = true;
        }

        ReplicationCoordinatorImpl* _replCoord;
        bool _finished;
        OpTime _optime;
        WriteConcernOptions _writeConcern;
        ReplicationCoordinator::StatusAndDuration _result;
        boost::scoped_ptr<boost::thread> _thread;
    };

    TEST(ReplicationCoordinator, AwaitReplicationNumberOfNodesBlocking) {
        ReplSettings settings;
        // Make sure we think we're a replSet
        settings.replSet = "mySet/node1:12345,node2:54321";
        ReplicationCoordinatorImpl coordinator(settings,
                                               new ReplicationCoordinatorExternalStateMock);
        OperationContextNoop txn;
        ReplicationAwaiter awaiter(&coordinator, &txn);

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
        awaiter.start();
        ASSERT_OK(coordinator.setLastOptime(&txn, client1, time1));
        ASSERT_OK(coordinator.setLastOptime(&txn, client2, time1));
        ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
        ASSERT_OK(statusAndDur.status);
        awaiter.reset();

        // 2 nodes waiting for time2
        awaiter.setOpTime(time2);
        awaiter.start();
        ASSERT_OK(coordinator.setLastOptime(&txn, client2, time2));
        ASSERT_OK(coordinator.setLastOptime(&txn, client3, time2));
        statusAndDur = awaiter.getResult();
        ASSERT_OK(statusAndDur.status);
        awaiter.reset();

        // 3 nodes waiting for time2
        writeConcern.wNumNodes = 3;
        awaiter.setWriteConcern(writeConcern);
        awaiter.start();
        ASSERT_OK(coordinator.setLastOptime(&txn, client1, time2));
        statusAndDur = awaiter.getResult();
        ASSERT_OK(statusAndDur.status);
        awaiter.reset();
    }

    TEST(ReplicationCoordinator, AwaitReplicationTimeout) {
        ReplSettings settings;
        // Make sure we think we're a replSet
        settings.replSet = "mySet/node1:12345,node2:54321";
        ReplicationCoordinatorImpl coordinator(settings,
                                               new ReplicationCoordinatorExternalStateMock);
        OperationContextNoop txn;
        ReplicationAwaiter awaiter(&coordinator, &txn);

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
        awaiter.start();
        ASSERT_OK(coordinator.setLastOptime(&txn, client1, time1));
        ASSERT_OK(coordinator.setLastOptime(&txn, client2, time1));
        ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        awaiter.reset();
    }

    TEST(ReplicationCoordinator, AwaitReplicationShutdown) {
        ReplSettings settings;
        // Make sure we think we're a replSet
        settings.replSet = "mySet/node1:12345,node2:54321";
        ReplicationCoordinatorImpl coordinator(settings,
                                               new ReplicationCoordinatorExternalStateMock);
        coordinator.startReplication(new TopologyCoordinatorImpl(0), new NetworkInterfaceMock);
        OperationContextNoop txn;
        ReplicationAwaiter awaiter(&coordinator, &txn);

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
        awaiter.start();
        ASSERT_OK(coordinator.setLastOptime(&txn, client1, time1));
        ASSERT_OK(coordinator.setLastOptime(&txn, client2, time1));
        coordinator.shutdown();
        ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
        ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, statusAndDur.status);
        awaiter.reset();
    }

    TEST(ReplicationCoordinator, AwaitReplicationNamedModes) {
        // TODO(spencer): Test awaitReplication with w:majority and tag groups
    }

}  // namespace
}  // namespace repl
}  // namespace mongo
