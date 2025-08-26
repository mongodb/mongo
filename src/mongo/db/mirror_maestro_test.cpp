/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/mirror_maestro.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/mirror_maestro_gen.h"
#include "mongo/db/repl/hello/topology_version_observer.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/platform/basic.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

class MirrorMaestroTest : service_context_test::WithSetupTransportLayer,
                          public ServiceContextMongoDTest {
public:
    constexpr static auto kInitialConfigVersion = 1;
    constexpr static auto kInitialTermVersion = 1;
    const BSONObj kEastTag = BSON("dc" << "east");
    const BSONObj kWestTag = BSON("dc" << "west");
    constexpr static auto kHost1 = "host1:27017";
    constexpr static auto kHost2 = "host2:27017";
    const BSONArray kFiveHosts = BSON_ARRAY(BSON("_id" << 0 << "host" << "hostname:10001")
                                            << BSON("_id" << 1 << "host" << "hostname:10002")
                                            << BSON("_id" << 2 << "host" << "hostname:10003")
                                            << BSON("_id" << 3 << "host" << "hostname:10004")
                                            << BSON("_id" << 4 << "host" << "hostname:10005"));
    const BSONArray kTwoHostsEW =
        BSON_ARRAY(BSON("_id" << 0 << "host" << kHost1 << "tags" << kEastTag)
                   << BSON("_id" << 1 << "host" << kHost2 << "tags" << kWestTag));
    const BSONArray kTwoHostsWE =
        BSON_ARRAY(BSON("_id" << 0 << "host" << kHost1 << "tags" << kWestTag)
                   << BSON("_id" << 1 << "host" << kHost2 << "tags" << kEastTag));
    const BSONArray kTwoHostsEE =
        BSON_ARRAY(BSON("_id" << 0 << "host" << kHost1 << "tags" << kEastTag)
                   << BSON("_id" << 1 << "host" << kHost2 << "tags" << kEastTag));
    const BSONArray kZeroHosts = BSONArrayBuilder().arr();

    virtual bool initMaestro() const {
        return true;
    };

    virtual bool initReplCoord() const {
        return true;
    }

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto service = getServiceContext();

        _net = std::make_shared<executor::NetworkInterfaceMock>();
        executor::ThreadPoolMock::Options opts{};
        _executor = executor::ThreadPoolTaskExecutor::create(
            std::make_unique<executor::ThreadPoolMock>(_net.get(), 1, std::move(opts)), _net);
        _executor->startup();

        if (initReplCoord()) {
            auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
            _replCoord = replCoord.get();
            auto config = makeConfig(kInitialConfigVersion, kInitialTermVersion, kTwoHostsEW);
            _replCoord->setGetConfigReturnValue(config);
            repl::ReplicationCoordinator::set(service, std::move(replCoord));
        }

        if (initMaestro()) {
            MirrorMaestro::init(service);
            setMirroringTaskExecutor_forTest(getServiceContext(), _executor);
        }
    }

    void tearDown() override {
        _executor->shutdown();
        // Drain all unfinished operations before resetting the executor.
        {
            executor::NetworkInterfaceMock::InNetworkGuard guard(_net.get());
            _net->drainUnfinishedNetworkOperations();
        };
        _executor->join();
        _executor.reset();
        _net.reset();

        MirrorMaestro::shutdown(getServiceContext());
        ServiceContextTest::tearDown();
    }

    void makeCommandAndMirror(OperationContext* opCtx,
                              BSONObj cmdObj,
                              size_t expNumMirrors,
                              bool supportsMirroring = true) {
        OpMsgRequest request = OpMsgRequestBuilder::create(
            auth::ValidatedTenancyScope::kNotRequired,
            DatabaseName::createDatabaseName_forTest(boost::none, "testDB"),
            cmdObj);
        Command* cmd = getCommandRegistry(opCtx)->findCommand(request.getCommandName());
        ASSERT(cmd);

        std::shared_ptr<CommandInvocation> invocation = cmd->parse(opCtx, request);
        ASSERT_EQ(invocation->isReadOperation(), supportsMirroring);
        ASSERT_EQ(invocation->supportsReadMirroring(), supportsMirroring);

        // Set the command we're "mirroring" on the opCtx.
        CommandInvocation::set(opCtx, invocation);

        // Mirror the command.
        MirrorMaestro::tryMirrorRequest(opCtx);
    }

    void assertNumReqs(size_t expNumReqs) {
        ClockSource::StopWatch stopwatch;
        auto timeout = Milliseconds(1000);

        // The MirrorMaestro schedules the command on a separate thread. To ensure that we don't
        // have a race condition between when the commands are being sent and when we are checking
        // for the number of ready requests, we have the get-call in a timed loop.
        while (stopwatch.elapsed() < timeout && getNumReadyRequests() != expNumReqs) {
            sleepmillis(10);
        }

        ASSERT_EQ(getNumReadyRequests(), expNumReqs);
    }

    size_t getNumReadyRequests() {
        executor::NetworkInterfaceMock::InNetworkGuard guard(_net.get());
        return _net->getNumReadyRequests();
    }

    executor::RemoteCommandRequest getNthRequest(int n) {
        executor::NetworkInterfaceMock::InNetworkGuard guard(_net.get());
        return _net->getNthReadyRequest(n)->getRequest();
    }

protected:
    ServerParameterControllerForTest _serverParameterController{"mirrorReads", BSONObj()};
    repl::ReplicationCoordinatorMock* _replCoord = nullptr;
    std::shared_ptr<executor::TaskExecutor> _executor;

    virtual repl::ReplSetConfig makeConfig(int version, int term, BSONArray members) {
        BSONObjBuilder configBuilder;
        configBuilder << "_id" << "_rs";
        configBuilder << "version" << version;
        configBuilder << "term" << term;
        configBuilder << "members" << members;
        configBuilder << "protocolVersion" << 1;
        return repl::ReplSetConfig::parse(configBuilder.obj());
    }

    virtual repl::ReplSetConfig setAndVerifyConfig(int version,
                                                   int term,
                                                   BSONArray members,
                                                   size_t expNumMembers) {
        auto config = makeConfig(version, term, members);
        ASSERT_EQ(config.getNumMembers(), expNumMembers);
        _replCoord->setGetConfigReturnValue(config);
        ASSERT_EQ(_replCoord->getConfig().getNumMembers(), expNumMembers);

        if (!initMaestro() || !initReplCoord()) {
            return config;
        }

        auto cache = _getCachedHelloResponse();
        ClockSource::StopWatch stopwatch;
        auto timeout = Milliseconds(1000);

        while (stopwatch.elapsed() < timeout) {
            if (cache && cache->getHosts().size() == expNumMembers) {
                break;
            }
            // The TopologyVersionObserver may have an uninitialized HelloResponse, defaulted to a
            // null pointer. Additionally, the MirrorMaestro's view of the server topology may not
            // be updated immediately. If either of these situations happen, we wait to get a new
            // cached HelloResponse in a timed-loop.
            sleepmillis(10);
            cache = _getCachedHelloResponse();
        }

        return config;
    }

private:
    std::shared_ptr<const repl::HelloResponse> _getCachedHelloResponse() const {
        auto swCache = getCachedHelloResponse_forTest(getServiceContext());
        ASSERT(swCache.isOK());
        return swCache.getValue();
    }

    std::shared_ptr<executor::NetworkInterfaceMock> _net;
};

class GeneralMirrorMaestroTest : public MirrorMaestroTest {
protected:
    void setServerParams(double generalSamplingRate) {
        _serverParameterController = ServerParameterControllerForTest(
            "mirrorReads",
            BSON("samplingRate" << generalSamplingRate << "maxTimeMS" << 500 << "targetedMirroring"
                                << BSON("samplingRate" << 0.0 << "maxTimeMS" << 500)));
    }
};

// When the general sample rate is 0%, we want no hosts to be targeted.
TEST_F(GeneralMirrorMaestroTest, GeneralSampleRateZero) {
    // Set the server parameters to have sampling rate = 0%.
    setServerParams(0.0);

    // Set the config and verify the number of members in the ReplSet.
    setAndVerifyConfig(2, 1, kFiveHosts, 5);

    // Try to mirror a command.
    auto opCtx = makeOperationContext();
    BSONObj cmdObj = BSON("find" << "test");
    makeCommandAndMirror(opCtx.get(), cmdObj, 0);

    // Check that we have no requests ready to be processed.
    assertNumReqs(0);
}

// Testing properties of mirrored requests when the sampling rate is 100%.
TEST_F(GeneralMirrorMaestroTest, GeneralProps) {
    // Set the server parameters to have sampling rate = 100%.
    setServerParams(1.0);

    // Set the config and verify the number of members in the ReplSet.
    setAndVerifyConfig(2, 1, kFiveHosts, 5);

    // Try to mirror a command.
    auto opCtx = makeOperationContext();
    BSONObj cmdObj = BSON("find" << "test");
    size_t expNumReqs = 4;
    makeCommandAndMirror(opCtx.get(), cmdObj, expNumReqs);

    // Check that we have 4 requests ready to process.
    assertNumReqs(expNumReqs);

    // Verify properties of the requests.
    for (size_t i = 0; i < expNumReqs; i += 1) {
        auto req = getNthRequest(i);

        ASSERT(req.fireAndForget);
        ASSERT(req.cmdObj.getBoolField("mirrored"));

        ASSERT_BSONOBJ_EQ_UNORDERED(BSON("mode" << "secondaryPreferred"),
                                    req.cmdObj.getField("$readPreference").embeddedObject());
        ASSERT_BSONOBJ_EQ_UNORDERED(BSON("level" << "local"),
                                    req.cmdObj.getField("readConcern").embeddedObject());
    }
}

// When the operation is not mirror-supported, we want no hosts to be targeted.
TEST_F(GeneralMirrorMaestroTest, GeneralUnsupportedOperation) {
    // Set the server parameter to have sampling rate = 100%.
    setServerParams(1.0);

    // Set the config and verify the number of members in the ReplSet.
    setAndVerifyConfig(2, 1, kFiveHosts, 5);

    // Try to mirror a command.
    auto opCtx = makeOperationContext();
    BSONObj cmdObj = BSON("insert" << "test" << "documents" << BSON_ARRAY(BSON("_id" << 0)));
    makeCommandAndMirror(opCtx.get(), cmdObj, 0, false);

    // Check that we have mirrored the command to no other node.
    assertNumReqs(0);
}

// No hosts should be targeted when the command is already mirrored.
TEST_F(GeneralMirrorMaestroTest, GeneralMirroredOperation) {
    // Set sampling rate to 100% to always catch that we don't send requests when the command is
    // already mirrored.
    setServerParams(1.0);

    // Set the config and verify the number of members in the ReplSet.
    setAndVerifyConfig(2, 1, kFiveHosts, 5);

    // Try to mirror a command.
    auto opCtx = makeOperationContext();
    BSONObj cmdObj = BSON("find" << "test" << "mirrored" << true);
    makeCommandAndMirror(opCtx.get(), cmdObj, 0);

    // Check that we have not mirrored the command.
    assertNumReqs(0);
}

// No requests sent when the host list is empty.
TEST_F(GeneralMirrorMaestroTest, GeneralEmptyHostsList) {
    // Set the server parameters to have some default sampling rate.
    setServerParams(1.0);

    // Set the config to have no members.
    setAndVerifyConfig(2, 1, kZeroHosts, 0);

    // Try to mirror a command.
    auto opCtx = makeOperationContext();
    BSONObj cmdObj = BSON("find" << "test");
    makeCommandAndMirror(opCtx.get(), cmdObj, 0);

    // Check that we have not mirrored the command.
    assertNumReqs(0);
}

// Invariant should be triggered when there is no invocation.
DEATH_TEST_F(GeneralMirrorMaestroTest, GeneralNoInvocation, "invariant") {
    // Set the server parameter to have sampling rate = 0%.
    setServerParams(0.0);

    // Set the config and verify the number of members in the ReplSet.
    setAndVerifyConfig(2, 1, kFiveHosts, 5);

    auto opCtx = makeOperationContext();

    // Try to mirror without a command invocation.
    MirrorMaestro::tryMirrorRequest(opCtx.get());
}
class NoInitGeneralMirrorTest : public GeneralMirrorMaestroTest {
public:
    bool initMaestro() const override {
        return false;
    }
};

// When the Impl is not initialised.
TEST_F(NoInitGeneralMirrorTest, GeneralUninitImpl) {
    // Set sampling rate to 100% to always check that we don't send requests when Impl is
    // not initialised.
    setServerParams(1.0);

    // Set the config and verify the number of members in the ReplSet.
    setAndVerifyConfig(2, 1, kFiveHosts, 5);

    // Try to mirror a command.
    auto opCtx = makeOperationContext();
    BSONObj cmdObj = BSON("find" << "test");
    makeCommandAndMirror(opCtx.get(), cmdObj, 0);

    // Check that we have no requests ready to be processed.
    assertNumReqs(0);
}

class NoReplCoordGeneralMirrorTest : public GeneralMirrorMaestroTest {
public:
    bool initReplCoord() const override {
        return false;
    }

    // We override this because we don't want the ServiceContextMongoDTest to initialise the Maestro
    // with a default ReplicationCoordinator.
    bool initMaestro() const override {
        return false;
    };
};

// Invariant should be triggered on absent replication coordinator.
DEATH_TEST_F(NoReplCoordGeneralMirrorTest, GeneralNoReplCoord, "invariant") {
    // Set up the Mirror Maestro to not have a replication coordinator.
    repl::ReplicationCoordinator::set(getServiceContext(), nullptr);
    MirrorMaestro::init(getServiceContext());
}

class TargetedMirrorMaestroTest : public MirrorMaestroTest {
public:
    const BSONObj kDefaultServerParam = BSON(
        "samplingRate" << 0.0 << "targetedMirroring"
                       << BSON("samplingRate" << 0.1 << "maxTimeMS" << 500 << "tag" << kEastTag));

    std::vector<HostAndPort> getCachedHosts() const {
        auto swHosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
        ASSERT(swHosts.isOK());
        return swHosts.getValue();
    }

private:
    FailPointEnableBlock _skipRegisteringMirroredReadsTopologyObserverCallback{
        "skipRegisteringMirroredReadsTopologyObserverCallback"};
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagTargetedMirrorReads",
                                                                true};
};

TEST_F(TargetedMirrorMaestroTest, BasicInitializationEmptyHostsCache) {
    // Verify hosts cache is initially empty
    auto hosts = getCachedHosts();

    ASSERT(hosts.empty());
}

TEST_F(TargetedMirrorMaestroTest, UpdateCachedHostsOnUpdatedTag) {
    // Turn the failpoint on so we can directly test the update function when the tag has changed,
    // without testing the server parameter update path calls into this path correctly
    FailPointEnableBlock skipTriggeringTargetedHostsListRefreshOnServerParamChange{
        "skipTriggeringTargetedHostsListRefreshOnServerParamChange"};

    // First, set the server parameter and update the cached hosts list
    _serverParameterController =
        ServerParameterControllerForTest("mirrorReads", kDefaultServerParam);

    auto version = kInitialConfigVersion + 1;
    auto term = kInitialTermVersion;
    auto config = setAndVerifyConfig(version, term, kTwoHostsEW, 2);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHosts();
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);

    // Now update the server parameter to change the tag and call update again
    auto updatedServerParam =
        BSON("targetedMirroring" << BSON("samplingRate" << 0.1 << "maxTimeMS" << 500 << "tag"
                                                        << kWestTag));
    _serverParameterController =
        ServerParameterControllerForTest("mirrorReads", updatedServerParam);

    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, true /* tagChanged */);

    // Verify hosts were updated
    hosts = getCachedHosts();
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost2);
}

TEST_F(TargetedMirrorMaestroTest, AssertCachedHostsUpdatedOnServerParameterChange) {
    // First, set the server parameter and update the cached hosts list
    _serverParameterController =
        ServerParameterControllerForTest("mirrorReads", kDefaultServerParam);

    auto version = kInitialConfigVersion + 1;
    auto term = kInitialTermVersion;
    auto config = setAndVerifyConfig(version, term, kTwoHostsEW, 2);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHosts();
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);

    // Now update the server parameter to change the tag. This test case does not set the failpoint
    // to skip updating the hosts list on a server parameter update, so this param change should
    // trigger the hosts to update
    auto updatedServerParam =
        BSON("targetedMirroring" << BSON("samplingRate" << 0.1 << "maxTimeMS" << 500 << "tag"
                                                        << kWestTag));
    _serverParameterController =
        ServerParameterControllerForTest("mirrorReads", updatedServerParam);

    // Verify hosts were updated
    hosts = getCachedHosts();
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost2);
}

TEST_F(TargetedMirrorMaestroTest, UpdateCachedHostsOnTopologyVersionChange) {
    ServerParameterControllerForTest controller("mirrorReads", kDefaultServerParam);

    int version = 2;
    int term = 1;
    auto config = setAndVerifyConfig(version, term, kTwoHostsEW, 2);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHosts();
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);

    // Now change the tags for host2 and bump the configVersion
    version++;
    config = setAndVerifyConfig(version, term, kTwoHostsEE, 2);

    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were updated
    hosts = getCachedHosts();
    ASSERT_EQ(hosts.size(), 2);
    ASSERT_EQ((hosts)[0].toString(), kHost1);
    ASSERT_EQ((hosts)[1].toString(), kHost2);
}

TEST_F(TargetedMirrorMaestroTest, NoUpdateToCachedHostsIfTopologyVersionUnchanged) {
    ServerParameterControllerForTest controller("mirrorReads", kDefaultServerParam);

    int version = 1;
    int term = 1;
    auto config = setAndVerifyConfig(version, term, kTwoHostsEW, 2);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHosts();
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);

    // Now call update with an unchanged config version, but changed tags. This is just a sanity
    // check that we don't try to update if the config version hasn't changed - it should never
    // happen in production that tags are changed without a change in version.
    config = setAndVerifyConfig(version, term, kTwoHostsEE, 2);

    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were not updated
    hosts = getCachedHosts();
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);
}

DEATH_TEST_F(TargetedMirrorMaestroTest, InvariantOnDecreasedConfigVersionForSameTerm, "invariant") {
    ServerParameterControllerForTest controller("mirrorReads", kDefaultServerParam);

    auto version = 2;
    auto term = 1;
    auto config = setAndVerifyConfig(version, term, kTwoHostsEW, 2);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHosts();
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ(((hosts)[0]).toString(), kHost1);

    // Now call update again but with a lower config version and same term, which should crash
    version--;
    config = setAndVerifyConfig(version, term, kTwoHostsWE, 2);

    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);
}

TEST_F(TargetedMirrorMaestroTest, UpdateHostsOnNewTermEvenIfLowerConfigVersion) {
    ServerParameterControllerForTest controller("mirrorReads", kDefaultServerParam);

    auto version = 2;
    auto term = 1;
    auto config = setAndVerifyConfig(version, term, kTwoHostsEW, 2);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHosts();
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);

    // Now call update again but with a lower config version and higher term. The higher term
    // indicates the more up to date config, so we should update.
    version--;
    term++;
    config = setAndVerifyConfig(version, term, kTwoHostsEE, 2);

    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were updated
    hosts = getCachedHosts();
    ASSERT_EQ(hosts.size(), 2);
    ASSERT_EQ((hosts)[0].toString(), kHost1);
    ASSERT_EQ((hosts)[1].toString(), kHost2);
}

TEST_F(TargetedMirrorMaestroTest, AssertExpectedHostsTargeted) {
    // Set the sampling rate for targeted mirroring to always mirror
    auto param = BSON("samplingRate"
                      << 0.0 << "targetedMirroring"
                      << BSON("samplingRate" << 1.0 << "maxTimeMS" << 500 << "tag" << kEastTag));
    ServerParameterControllerForTest controller("mirrorReads", param);

    int version = 2;
    int term = 1;
    auto config = setAndVerifyConfig(version, term, kTwoHostsEW, 2);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHosts();
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);

    auto opCtx = makeOperationContext();

    // Try to mirror a command.
    BSONObj cmdObj = BSON("find" << "test");
    makeCommandAndMirror(opCtx.get(), cmdObj, hosts.size());

    // Verify that we have reached the right host.
    assertNumReqs(1);
    auto req = getNthRequest(0);
    ASSERT_EQ(req.target, hosts.at(0));
}

TEST_F(TargetedMirrorMaestroTest, UninitializedConfigDefersHostCompute) {
    auto service = getServiceContext();

    // Set the sampling rate for targeted mirroring to always mirror
    auto param = BSON("samplingRate"
                      << 0.0 << "targetedMirroring"
                      << BSON("samplingRate" << 1.0 << "maxTimeMS" << 500 << "tag" << kEastTag));
    ServerParameterControllerForTest controller("mirrorReads", param);

    // Create an uninitialized config.
    auto config = repl::ReplSetConfig();
    _replCoord->setGetConfigReturnValue(config);

    // Attempt to update cached hosts and assert host size.
    updateCachedHostsForTargetedMirroring_forTest(service, config, false /* tagChanged */);
    ASSERT_EQ(0, getCachedHosts().size());

    // Update the config to be initialized.
    setAndVerifyConfig(2 /* version */, 1 /* term */, kTwoHostsEW, 2);

    // Host list should be computed upon getting.
    auto hosts = getCachedHosts();
    ASSERT_EQ(1, hosts.size());
    ASSERT_EQ((hosts)[0].toString(), kHost1);
}

class NoInitMirrorTest : public TargetedMirrorMaestroTest {
public:
    bool initMaestro() const override {
        return false;
    }
};

// Test that setting the mirrorReads parameter before initialization will update the cached hosts
// after initialization.
TEST_F(NoInitMirrorTest, SetParamBeforeInit) {
    setAndVerifyConfig(2 /* version */, 1 /* term */, kTwoHostsEW, 2);

    auto param = BSON("samplingRate"
                      << 0.0 << "targetedMirroring"
                      << BSON("samplingRate" << 1.0 << "maxTimeMS" << 500 << "tag" << kEastTag));
    ServerParameterControllerForTest controller("mirrorReads", param);

    // Initializing MirrorMaestro will update the cached hosts according to the param we set above.
    MirrorMaestro::init(getServiceContext());
    ASSERT_EQ(1, getCachedHosts().size());
}

}  // namespace
}  // namespace mongo
