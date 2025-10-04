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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <memory>
#include <string>

namespace mongo {

class BSONObj;
struct HostAndPort;

namespace repl {

class ReplSetConfig;

class ReplicationCoordinatorExternalStateMock;
class ReplicationCoordinatorImpl;

class StorageInterfaceMock;
class TopologyCoordinator;

using executor::NetworkInterfaceMock;

/**
 * Fixture for testing ReplicationCoordinatorImpl behaviors.
 */
class MONGO_MOD_OPEN ReplCoordTest : public ServiceContextMongoDTest {
public:
    /**
     * Makes a command response with the given "doc" response and optional elapsed time "millis".
     */
    static executor::RemoteCommandResponse makeResponseStatus(
        const BSONObj& doc, Milliseconds millis = Milliseconds(0));

    /**
     * Constructs a ReplSetConfig from the given BSON, or raises a test failure exception.
     */
    static ReplSetConfig assertMakeRSConfig(const BSONObj& configBSON);

    /**
     * Adds { protocolVersion: 0 or 1 } to the config.
     */
    static BSONObj addProtocolVersion(const BSONObj& configDoc, int protocolVersion);

    /**
     * Helpers to construct a config.
     */
    static BSONObj member(int id, std::string host, int votes = 1) {
        return BSON("_id" << id << "host" << host << "votes" << votes << "priority" << votes);
    }

    static BSONObj configWithMembers(int version, long long term, BSONArray members) {
        return BSON("_id" << "mySet"
                          << "protocolVersion" << 1 << "version" << version << "term" << term
                          << "members" << members);
    }

protected:
    explicit ReplCoordTest(Options options = Options{}.useMockClock(true));
    ~ReplCoordTest() override;

    /**
     * Asserts that calling start(configDoc, selfHost) successfully initiates the
     * ReplicationCoordinator under test.
     */
    virtual void assertStartSuccess(const BSONObj& configDoc, const HostAndPort& selfHost);

    /**
     * Asserts that calling start with data (such as a populated oplog or last vote document)
     * successfully initiates the ReplicationCoordinator under test.
     */
    virtual void assertStartSuccessWithData(const BSONObj& configDoc,
                                            const HostAndPort& selfHost,
                                            const LastVote& lastVote,
                                            const OpTime& topOfOplog);

    /**
     * Gets the network mock.
     */
    executor::NetworkInterfaceMock* getNet() {
        return _net;
    }

    /**
     * Gets the replication executor under test.
     */
    executor::TaskExecutor* getReplExec();

    /**
     * Gets the replication coordinator under test.
     */
    ReplicationCoordinatorImpl* getReplCoord() {
        return _repl.get();
    }

    /**
     * These are the setters to advance lastWritten/lastApplied/lastDurable timestamp in replication
     * coordinator. Note that the call will be ignored if the input timestamp is not greater than
     * the current one in ReplicationCoordinatorImpl(that's what "Forward" means), while the call
     * won't be ignored in ReplicationCoordinatorMock.
     */
    void replCoordSetMyLastWrittenOpTime(const OpTime& opTime, Date_t wallTime = Date_t()) {
        if (wallTime == Date_t()) {
            wallTime = Date_t() + Seconds(opTime.getSecs());
        }
        getReplCoord()->setMyLastWrittenOpTimeAndWallTimeForward({opTime, wallTime});
    }

    void replCoordSetMyLastAppliedOpTime(const OpTime& opTime, Date_t wallTime = Date_t()) {
        if (wallTime == Date_t()) {
            wallTime = Date_t() + Seconds(opTime.getSecs());
        }
        getReplCoord()->setMyLastAppliedOpTimeAndWallTimeForward({opTime, wallTime});
    }

    void replCoordSetMyLastDurableOpTime(const OpTime& opTime, Date_t wallTime = Date_t()) {
        if (wallTime == Date_t()) {
            wallTime = Date_t() + Seconds(opTime.getSecs());
        }
        getReplCoord()->setMyLastDurableOpTimeAndWallTimeForward({opTime, wallTime});
    }

    void replCoordSetMyLastDurableAndLastWrittenOpTime(const OpTime& opTime,
                                                       Date_t wallTime = Date_t()) {
        if (wallTime == Date_t()) {
            wallTime = Date_t() + Seconds(opTime.getSecs());
        }
        getReplCoord()->setMyLastDurableAndLastWrittenOpTimeAndWallTimeForward({opTime, wallTime});
    }

    void replCoordSetMyLastAppliedAndLastWrittenOpTime(const OpTime& opTime,
                                                       Date_t wallTime = Date_t()) {
        if (wallTime == Date_t()) {
            wallTime = Date_t() + Seconds(opTime.getSecs());
        }
        getReplCoord()->setMyLastAppliedAndLastWrittenOpTimeAndWallTimeForward({opTime, wallTime});
    }

    void replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(const OpTime& opTime,
                                                             Date_t wallTime = Date_t()) {
        replCoordSetMyLastWrittenOpTime(opTime, wallTime);
        replCoordSetMyLastAppliedOpTime(opTime, wallTime);
        replCoordSetMyLastDurableOpTime(opTime, wallTime);
    }

    void replCoordAdvanceCommitPoint(const OpTime& opTime,
                                     Date_t wallTime = Date_t(),
                                     bool fromSyncSource = false) {
        if (wallTime == Date_t()) {
            wallTime = Date_t() + Seconds(opTime.getSecs());
        }
        getReplCoord()->advanceCommitPoint({opTime, wallTime}, fromSyncSource);
    }

    void replCoordAdvanceCommitPoint(const OpTimeAndWallTime& opTime, bool fromSyncSource = false) {
        getReplCoord()->advanceCommitPoint(opTime, fromSyncSource);
    }

    /**
     * Gets the storage interface.
     */
    StorageInterfaceMock* getStorageInterface() {
        return _storageInterface;
    }

    /**
     * Gets the topology coordinator used by the replication coordinator under test.
     */
    TopologyCoordinator& getTopoCoord() {
        return *_repl->getTopologyCoordinator_forTest();
    }

    /**
     * Gets the external state used by the replication coordinator under test.
     */
    ReplicationCoordinatorExternalStateMock* getExternalState() {
        return _externalState;
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
        std::function<void(const executor::RemoteCommandRequest& request)> onDryRunRequest);
    void simulateSuccessfulDryRun();

    /**
     * Brings the repl coord from SECONDARY to PRIMARY by simulating the messages required to
     * elect it, after progressing the mocked-out notion of time past the election timeout.
     *
     * Behavior is unspecified if node does not have a clean config, is not in SECONDARY, etc.
     */
    void simulateSuccessfulV1Election();

    /**
     * Same as simulateSuccessfulV1Election, but rather than getting the election timeout and
     * progressing time past that point, takes in what time to expect an election to occur at.
     * Useful for simulating elections triggered via priority takeover.
     */
    void simulateSuccessfulV1ElectionAt(OperationContext* opCtx, Date_t electionTime);

    /**
     * When the test has been configured with a replica set config with a single member, use this
     * to put that single member into state PRIMARY.
     */
    void runSingleNodeElection(OperationContext* opCtx);

    /**
     * Same as simulateSuccessfulV1ElectionAt, but stops short of signaling drain completion,
     * so the node stays in drain mode.
     */
    void simulateSuccessfulV1ElectionWithoutExitingDrainMode(Date_t electionTime,
                                                             OperationContext* opCtx);

    /**
     * Transition the ReplicationCoordinator from writer draining mode to applier draining mode.
     */
    void signalWriterDrainComplete(OperationContext* opCtx) noexcept;

    /**
     * Transition the ReplicationCoordinator from drain mode to being fully primary/master.
     */
    void signalApplierDrainComplete(OperationContext* opCtx) noexcept;

    /**
     * Shuts down the objects under test.
     */
    void shutdown(OperationContext* opCtx);

    /**
     * Receive the heartbeat request from replication coordinator and reply with a response.
     */
    void replyToReceivedHeartbeatV1();
    /**
     * Consumes the network operation and responds if it's a heartbeat request.
     * Returns whether the operation is a heartbeat request.
     */
    bool consumeHeartbeatV1(const NetworkInterfaceMock::NetworkOperationIterator& noi);

    void simulateEnoughHeartbeatsForAllNodesUp();

    /**
     * Disables read concern majority support.
     */
    void disableReadConcernMajoritySupport();

    /**
     * Disables snapshot support.
     */
    void disableSnapshots();

    /**
     * Timeout all heartbeat requests for primary catch-up.
     */
    void simulateCatchUpAbort();

    ReadWriteConcernDefaultsLookupMock lookupMock;

private:
    std::unique_ptr<ReplicationCoordinatorImpl> _repl;
    // Owned by executor
    executor::NetworkInterfaceMock* _net = nullptr;
    // Owned by ReplicationCoordinatorImpl
    ReplicationCoordinatorExternalStateMock* _externalState = nullptr;
    // Owned by ReplicationCoordinatorImpl
    executor::TaskExecutor* _replExec = nullptr;
    // Owned by the ServiceContext
    StorageInterfaceMock* _storageInterface = nullptr;

    ReplSettings _settings;
    bool _callShutdown = false;

    // Disable the QueryAnalysisCoordinator because this fixture doesn't construct the
    // ServiceEntryPoint and this causes a segmentation fault when the QueryAnalysisCoordinator
    // uses DBDirectClient to call into the ServiceEntryPoint.
    FailPointEnableBlock _disableQueryAnalysisCoordinator{"disableQueryAnalysisCoordinator"};
    // Disable the QueryAnalysisWriter because the fixture doesn't construct the ServiceEntryPoint
    // or the PeriodicRunner.
    FailPointEnableBlock _disableQueryAnalysisWriter{"disableQueryAnalysisWriter"};
};

}  // namespace repl
}  // namespace mongo
