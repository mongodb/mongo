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

#pragma once

#include <string>

#include "mongo/db/client.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class BSONObj;
struct HostAndPort;

namespace executor {
class NetworkInterfaceMock;
}  // namespace executor

namespace repl {

class ReplicaSetConfig;
class ReplicationCoordinatorExternalStateMock;
class ReplicationCoordinatorImpl;
class StorageInterfaceMock;
class TopologyCoordinatorImpl;

/**
 * Fixture for testing ReplicationCoordinatorImpl behaviors.
 */
class ReplCoordTest : public mongo::unittest::Test {
public:
    /**
     * Makes a ResponseStatus with the given "doc" response and optional elapsed time "millis".
     */
    static ResponseStatus makeResponseStatus(const BSONObj& doc,
                                             Milliseconds millis = Milliseconds(0));

    /**
     * Makes a ResponseStatus with the given "doc" response, metadata and optional elapsed time
     * "millis".
     */
    static ResponseStatus makeResponseStatus(const BSONObj& doc,
                                             const BSONObj& metadata,
                                             Milliseconds millis = Milliseconds(0));

    /**
     * Constructs a ReplicaSetConfig from the given BSON, or raises a test failure exception.
     */
    static ReplicaSetConfig assertMakeRSConfig(const BSONObj& configBSON);
    static ReplicaSetConfig assertMakeRSConfigV0(const BSONObj& configBson);

    /**
     * Adds { protocolVersion: 0 or 1 } to the config.
     */
    static BSONObj addProtocolVersion(const BSONObj& configDoc, int protocolVersion);

protected:
    virtual void setUp();
    virtual void tearDown();

    /**
     * Asserts that calling start(configDoc, selfHost) successfully initiates the
     * ReplicationCoordinator under test.
     */
    virtual void assertStartSuccess(const BSONObj& configDoc, const HostAndPort& selfHost);

    /**
     * Gets the network mock.
     */
    executor::NetworkInterfaceMock* getNet() {
        return _net;
    }

    /**
     * Gets the replication executor under test.
     */
    ReplicationExecutor* getReplExec() {
        return _replExec.get();
    }

    /**
     * Gets the replication coordinator under test.
     */
    ReplicationCoordinatorImpl* getReplCoord() {
        return _repl.get();
    }

    /**
     * Gets the topology coordinator used by the replication coordinator under test.
     */
    TopologyCoordinatorImpl& getTopoCoord() {
        return *_topo;
    }

    /**
     * Gets the external state used by the replication coordinator under test.
     */
    ReplicationCoordinatorExternalStateMock* getExternalState() {
        return _externalState;
    }

    /**
     * Makes a new OperationContext on the default Client for this test.
     */
    ServiceContext::UniqueOperationContext makeOperationContext() {
        return _client->makeOperationContext();
    }

    /**
     * Returns the ServiceContext for this test.
     */
    ServiceContext* getServiceContext() {
        return getGlobalServiceContext();
    }

    /**
     * Returns the default Client for this test.
     */
    Client* getClient() {
        return _client.get();
    }

    /**
     * Adds "selfHost" to the list of hosts that identify as "this" host.
     */
    void addSelf(const HostAndPort& selfHost);

    /**
     * Moves time forward in the network until the new time, and asserts if now!=newTime after
     */
    void assertRunUntil(Date_t newTime);

    /**
     * Shorthand for getNet()->enterNetwork()
     */
    void enterNetwork();

    /**
     * Shorthand for getNet()->exitNetwork()
     */
    void exitNetwork();

    /**
     * Initializes the objects under test; this behavior is optional, in case you need to call
     * any methods on the network or coordinator objects before calling start.
     */
    void init();

    /**
     * Initializes the objects under test, using the given "settings".
     */
    void init(const ReplSettings& settings);

    /**
     * Initializes the objects under test, using "replSet" as the name of the replica set under
     * test.
     */
    void init(const std::string& replSet);

    /**
     * Starts the replication coordinator under test, with no local config document and
     * no notion of what host or hosts are represented by the network interface.
     */
    void start();

    /**
     * Starts the replication coordinator under test, with the given configuration in
     * local storage and the given host name.
     */
    void start(const BSONObj& configDoc, const HostAndPort& selfHost);

    /**
     * Starts the replication coordinator under test with the given host name.
     */
    void start(const HostAndPort& selfHost);

    /**
     * Brings the TopologyCoordinator from follower to candidate by simulating a period of time in
     * which the election timer expires and starts a dry run election.
     * Returns after dry run is completed but before actual election starts.
     * If 'onDryRunRequest' is provided, this function is invoked with the
     * replSetRequestVotes network request before simulateSuccessfulDryRun() simulates
     * a successful dry run vote response.
     * Applicable to protocol version 1 only.
     */
    void simulateSuccessfulDryRun(
        stdx::function<void(const executor::RemoteCommandRequest& request)> onDryRunRequest);
    void simulateSuccessfulDryRun();

    /**
     * Brings the repl coord from SECONDARY to PRIMARY by simulating the messages required to
     * elect it.
     *
     * Behavior is unspecified if node does not have a clean config, is not in SECONDARY, etc.
     */
    void simulateSuccessfulElection();
    void simulateSuccessfulV1Election();

    /**
     * Shuts down the objects under test.
     */
    void shutdown(OperationContext* txn);

    /**
     * Receive the heartbeat request from replication coordinator and reply with a response.
     */
    void replyToReceivedHeartbeat();
    void replyToReceivedHeartbeatV1();

    /**
     * Sets how the test fixture reports the storage engine's durability feature.
     */
    void setStorageEngineDurable(bool val = true) {
        _isStorageEngineDurable = val;
    }
    bool isStorageEngineDurable() const {
        return _isStorageEngineDurable;
    }

    void simulateEnoughHeartbeatsForAllNodesUp();

    /**
     * Disables read concern majority support.
     */
    void disableReadConcernMajoritySupport();

    /**
     * Disables snapshot support.
     */
    void disableSnapshots();

private:
    std::unique_ptr<ReplicationCoordinatorImpl> _repl;
    // Owned by ReplicationCoordinatorImpl
    TopologyCoordinatorImpl* _topo = nullptr;
    // Owned by ReplicationExecutor
    executor::NetworkInterfaceMock* _net = nullptr;
    std::unique_ptr<ReplicationExecutor> _replExec;
    // Owned by ReplicationCoordinatorImpl
    ReplicationCoordinatorExternalStateMock* _externalState = nullptr;
    ReplSettings _settings;
    bool _callShutdown = false;
    bool _isStorageEngineDurable = true;
    ServiceContext::UniqueClient _client = getGlobalServiceContext()->makeClient("testClient");
};

}  // namespace repl
}  // namespace mongo
