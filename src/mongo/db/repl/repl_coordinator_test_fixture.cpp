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

#include "mongo/db/repl/repl_coordinator_test_fixture.h"

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/repl_coordinator_external_state_mock.h"
#include "mongo/db/repl/repl_coordinator_impl.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {

namespace {
    bool stringContains(const std::string &haystack, const std::string& needle) {
        return haystack.find(needle) != std::string::npos;
    }
}  // namespace

    ReplCoordTest::ReplCoordTest() : _callShutdown(false) {}
    ReplCoordTest::~ReplCoordTest() {}

    void ReplCoordTest::setUp() {
        _settings.replSet = "mySet/node1:12345,node2:54321";
    }

    void ReplCoordTest::tearDown() {
        if (_callShutdown) {
            shutdown();
        }
    }

    NetworkInterfaceMockWithMap* ReplCoordTest::getNetWithMap() {
        return dynamic_cast<NetworkInterfaceMockWithMap*>(_net);
    }

    void ReplCoordTest::init() {
        invariant(!_repl);
        invariant(!_callShutdown);

        // PRNG seed for tests.
        const int64_t seed = 0;

        _topo = new TopologyCoordinatorImpl(Seconds(0));
        _net = new NetworkInterfaceMockWithMap;
        _externalState = new ReplicationCoordinatorExternalStateMock;
        _repl.reset(new ReplicationCoordinatorImpl(_settings,
                                                   _externalState,
                                                   _net,
                                                   _topo,
                                                   seed));
    }

    void ReplCoordTest::init(const ReplSettings& settings) {
        _settings = settings;
        init();
    }

    void ReplCoordTest::init(const std::string& replSet) {
        _settings.replSet = replSet;
        init();
    }

    void ReplCoordTest::start() {
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

    void ReplCoordTest::start(const BSONObj& configDoc, const HostAndPort& selfHost) {
        if (!_repl) {
            init();
        }
        _externalState->setLocalConfigDocument(StatusWith<BSONObj>(configDoc));
        _externalState->addSelf(selfHost);
        start();
    }

    void ReplCoordTest::start(const HostAndPort& selfHost) {
        if (!_repl) {
            init();
        }
        _externalState->addSelf(selfHost);
        start();
    }

    void ReplCoordTest::assertStart(
            ReplicationCoordinator::Mode expectedMode,
            const BSONObj& configDoc,
            const HostAndPort& selfHost) {
        start(configDoc, selfHost);
        ASSERT_EQUALS(expectedMode, getReplCoord()->getReplicationMode());
    }

    void ReplCoordTest::assertStartSuccess(
            const BSONObj& configDoc,
            const HostAndPort& selfHost) {
        assertStart(ReplicationCoordinator::modeReplSet, configDoc, selfHost);
    }

    void ReplCoordTest::shutdown() {
        invariant(_callShutdown);
        _repl->shutdown();
        _callShutdown = false;
    }

    int64_t ReplCoordTest::countLogLinesContaining(const std::string& needle) {
        return std::count_if(getCapturedLogMessages().begin(),
                             getCapturedLogMessages().end(),
                             stdx::bind(stringContains,
                                        stdx::placeholders::_1,
                                        needle));
    }

}  // namespace repl
}  // namespace mongo
