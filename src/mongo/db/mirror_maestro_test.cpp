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
#include "mongo/idl/server_parameter_test_util.h"
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
    const BSONObj kDefaultServerParam = BSON(
        "samplingRate" << 0.0 << "targetedMirroring"
                       << BSON("samplingRate" << 0.1 << "maxTimeMS" << 500 << "tag" << kEastTag));

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto service = getServiceContext();

        _net = std::make_shared<executor::NetworkInterfaceMock>();
        executor::ThreadPoolMock::Options opts{};
        _executor = executor::ThreadPoolTaskExecutor::create(
            std::make_unique<executor::ThreadPoolMock>(_net.get(), 1, std::move(opts)), _net);
        _executor->startup();
        _networkTestEnv = std::make_shared<executor::NetworkTestEnv>(_executor.get(), _net.get());

        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
        _replCoord = replCoord.get();
        auto config = makeConfig(kInitialConfigVersion, kInitialTermVersion, kEastTag, kWestTag);
        _replCoord->setGetConfigReturnValue(config);
        repl::ReplicationCoordinator::set(service, std::move(replCoord));

        if (initMaestro()) {
            MirrorMaestro::init(service);
            setExecutor_forTest(getServiceContext(), _executor);
        }
    }

    void tearDown() override {
        _networkTestEnv.reset();
        _executor->shutdown();
        _executor->join();
        _executor.reset();
        _net.reset();

        MirrorMaestro::shutdown(getServiceContext());
        ServiceContextTest::tearDown();
    }

    virtual bool initMaestro() const {
        return true;
    };

    void onCommand(executor::NetworkTestEnv::OnCommandFunction func) {
        _networkTestEnv->onCommand(func);
    }

    template <typename Lambda>
    executor::NetworkTestEnv::FutureHandle<typename std::invoke_result<Lambda>::type> launchAsync(
        Lambda&& func) const {
        return _networkTestEnv->launchAsync(std::forward<Lambda>(func));
    }

protected:
    ServerParameterControllerForTest _serverParameterController{"mirrorReads", BSONObj()};
    repl::ReplicationCoordinatorMock* _replCoord = nullptr;
    std::shared_ptr<executor::TaskExecutor> _executor;

    repl::ReplSetConfig makeConfig(int version, int term, BSONObj host1Tag, BSONObj host2Tag) {
        BSONObjBuilder configBuilder;
        configBuilder << "_id" << "_rs";
        configBuilder << "version" << version;
        configBuilder << "term" << term;
        configBuilder << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host" << kHost1 << "tags" << host1Tag)
                                    << BSON("_id" << 1 << "host" << kHost2 << "tags" << host2Tag));
        configBuilder << "protocolVersion" << 1;
        return repl::ReplSetConfig::parse(configBuilder.obj());
    }

private:
    FailPointEnableBlock _skipRegisteringMirroredReadsTopologyObserverCallback{
        "skipRegisteringMirroredReadsTopologyObserverCallback"};
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagTargetedMirrorReads",
                                                                true};

    std::shared_ptr<executor::NetworkTestEnv> _networkTestEnv;
    std::shared_ptr<executor::NetworkInterfaceMock> _net;
};

TEST_F(MirrorMaestroTest, BasicInitializationEmptyHostsCache) {
    // Verify hosts cache is initially empty
    auto hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT(hosts.empty());
}

TEST_F(MirrorMaestroTest, UpdateCachedHostsOnUpdatedTag) {
    // Turn the failpoint on so we can directly test the update function when the tag has changed,
    // without testing the server parameter update path calls into this path correctly
    FailPointEnableBlock skipTriggeringTargetedHostsListRefreshOnServerParamChange{
        "skipTriggeringTargetedHostsListRefreshOnServerParamChange"};

    // First, set the server parameter and update the cached hosts list
    _serverParameterController =
        ServerParameterControllerForTest("mirrorReads", kDefaultServerParam);

    auto version = kInitialConfigVersion + 1;
    auto term = kInitialTermVersion;
    auto config = makeConfig(version, term, kEastTag, kWestTag);
    _replCoord->setGetConfigReturnValue(config);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
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
    hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost2);
}

TEST_F(MirrorMaestroTest, AssertCachedHostsUpdatedOnServerParameterChange) {
    // First, set the server parameter and update the cached hosts list
    _serverParameterController =
        ServerParameterControllerForTest("mirrorReads", kDefaultServerParam);

    auto version = kInitialConfigVersion + 1;
    auto term = kInitialTermVersion;
    auto config = makeConfig(version, term, kEastTag, kWestTag);
    _replCoord->setGetConfigReturnValue(config);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
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
    hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost2);
}

TEST_F(MirrorMaestroTest, UpdateCachedHostsOnTopologyVersionChange) {
    ServerParameterControllerForTest controller("mirrorReads", kDefaultServerParam);

    int version = 2;
    int term = 1;
    auto config = makeConfig(version, term, kEastTag, kWestTag);
    _replCoord->setGetConfigReturnValue(config);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);

    // Now change the tags for host2 and bump the configVersion
    version++;
    config = makeConfig(version, term, kEastTag, kEastTag);
    _replCoord->setGetConfigReturnValue(config);

    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were updated
    hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 2);
    ASSERT_EQ((hosts)[0].toString(), kHost1);
    ASSERT_EQ((hosts)[1].toString(), kHost2);
}

TEST_F(MirrorMaestroTest, NoUpdateToCachedHostsIfTopologyVersionUnchanged) {
    ServerParameterControllerForTest controller("mirrorReads", kDefaultServerParam);

    int version = 1;
    int term = 1;
    auto config = makeConfig(version, term, kEastTag, kWestTag);
    _replCoord->setGetConfigReturnValue(config);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);

    // Now call update with an unchanged config version, but changed tags. This is just a sanity
    // check that we don't try to update if the config version hasn't changed - it should never
    // happen in production that tags are changed without a change in version.
    config = makeConfig(version, term, kEastTag, kEastTag);
    _replCoord->setGetConfigReturnValue(config);

    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were not updated
    hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);
}

DEATH_TEST_F(MirrorMaestroTest, InvariantOnDecreasedConfigVersionForSameTerm, "invariant") {
    ServerParameterControllerForTest controller("mirrorReads", kDefaultServerParam);

    auto version = 2;
    auto term = 1;
    auto config = makeConfig(version, term, kEastTag, kWestTag);
    _replCoord->setGetConfigReturnValue(config);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ(((hosts)[0]).toString(), kHost1);

    // Now call update again but with a lower config version and same term, which should crash
    version--;
    config = makeConfig(version, term, kWestTag, kEastTag);
    _replCoord->setGetConfigReturnValue(config);

    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);
}

TEST_F(MirrorMaestroTest, UpdateHostsOnNewTermEvenIfLowerConfigVersion) {
    ServerParameterControllerForTest controller("mirrorReads", kDefaultServerParam);

    auto version = 2;
    auto term = 1;
    auto config = makeConfig(version, term, kEastTag, kWestTag);
    _replCoord->setGetConfigReturnValue(config);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);

    // Now call update again but with a lower config version and higher term. The higher term
    // indicates the more up to date config, so we should update.
    version--;
    term++;
    config = makeConfig(version, term, kEastTag, kEastTag);
    _replCoord->setGetConfigReturnValue(config);

    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were updated
    hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 2);
    ASSERT_EQ((hosts)[0].toString(), kHost1);
    ASSERT_EQ((hosts)[1].toString(), kHost2);
}

TEST_F(MirrorMaestroTest, AssertExpectedHostsTargeted) {
    // Set the sampling rate for targeted mirroring to always mirror
    auto param = BSON("samplingRate"
                      << 0.0 << "targetedMirroring"
                      << BSON("samplingRate" << 1.0 << "maxTimeMS" << 500 << "tag" << kEastTag));
    ServerParameterControllerForTest controller("mirrorReads", param);

    int version = 2;
    int term = 1;
    auto config = makeConfig(version, term, kEastTag, kWestTag);
    _replCoord->setGetConfigReturnValue(config);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);

    // We don't really care about the response in this case, but otherwise we send the request as a
    // fireAndForget request, which the mock network env will cancel at the end of the test and will
    // try to wait for a response to the cancelation. To avoid having to mock a cancellation
    // response, we can instead have the mirroring thread just exeucte the commands as
    // non-fireAndForget.
    auto opCtx = makeOperationContext();
    FailPointEnableBlock mirrorMaestroExpectsResponseFp{"mirrorMaestroExpectsResponse"};

    // Now send mirror requests
    auto future = launchAsync([&] {
        // Set the client on this thread.
        Client::setCurrent(getServiceContext()->getService()->makeClient("MirrorMaestroTest"));

        // Set the command we're "mirroring" on the opCtx
        auto cmdObj = BSON("find" << "test");
        auto request = OpMsgRequestBuilder::create(
            auth::ValidatedTenancyScope::kNotRequired,
            DatabaseName::createDatabaseName_forTest(boost::none, "testDB"),
            cmdObj);
        auto cmd = getCommandRegistry(opCtx.get())->findCommand(request.getCommandName());
        ASSERT(cmd);
        std::shared_ptr<CommandInvocation> invocation = cmd->parse(opCtx.get(), request);
        CommandInvocation::set(opCtx.get(), invocation);

        // Now, actually mirror
        MirrorMaestro::tryMirrorRequest(opCtx.get());
        auto client = Client::releaseCurrent();
        client.reset(nullptr);
    });

    // Check the correct hosts were targeted - it should only be the one host with the kEast tag.
    std::vector<HostAndPort> targetedHosts;
    onCommand([&](const executor::RemoteCommandRequest& request) {
        targetedHosts.push_back(request.target);
        return BSONObj();
    });

    future.default_timed_get();

    ASSERT_EQ(hosts, targetedHosts);
}

TEST_F(MirrorMaestroTest, UninitializedConfigDefersHostCompute) {
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
    ASSERT_EQ(0, getCachedHostsForTargetedMirroring_forTest(service).size());

    // Update the config to be initialized.
    auto newConfig = makeConfig(2 /* version */, 1 /* term */, kEastTag, kWestTag);
    _replCoord->setGetConfigReturnValue(newConfig);

    // Host list should be computed upon getting.
    auto hosts = getCachedHostsForTargetedMirroring_forTest(service);
    ASSERT_EQ(1, hosts.size());
    ASSERT_EQ((hosts)[0].toString(), kHost1);
}

class NoInitMirrorTest : public MirrorMaestroTest {
public:
    bool initMaestro() const override {
        return false;
    }
};

TEST_F(NoInitMirrorTest, UninitMirrorMaestorDoesNotTargetHosts) {
    auto config = makeConfig(2 /* version */, 1 /* term */, kEastTag, kWestTag);
    _replCoord->setGetConfigReturnValue(config);

    auto param = BSON("samplingRate"
                      << 0.0 << "targetedMirroring"
                      << BSON("samplingRate" << 1.0 << "maxTimeMS" << 500 << "tag" << kEastTag));
    ServerParameterControllerForTest controller("mirrorReads", param);

    // Uninitialized MirrorMaestro should have 0 hosts that are targeted.
    ASSERT_EQ(0, getCachedHostsForTargetedMirroring_forTest(getServiceContext()).size());
}

}  // namespace
}  // namespace mongo
