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

#include "mongo/platform/basic.h"

#include <set>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_internal.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::map;
using std::set;
using std::string;
using std::unique_ptr;
using std::vector;
using unittest::assertGet;

MONGO_INITIALIZER(DisableReplicaSetMonitorRefreshRetries)(InitializerContext*) {
    ReplicaSetMonitor::disableRefreshRetries_forTest();
    return Status::OK();
}

// TODO: Port these existing tests here: replmonitor_bad_seed.js, repl_monitor_refresh.js

/**
 * Warning: Tests running this fixture cannot be run in parallel with other tests
 * that uses ConnectionString::setConnectionHook
 */
class ReplicaSetMonitorTest : public mongo::unittest::Test {
protected:
    void setUp() {
        _replSet.reset(new MockReplicaSet("test", 3));
        _originalConnectionHook = ConnectionString::getConnectionHook();
        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());
    }

    void tearDown() {
        ConnectionString::setConnectionHook(_originalConnectionHook);
        ReplicaSetMonitor::cleanup();
        _replSet.reset();
        mongo::ScopedDbConnection::clearPool();
    }

    MockReplicaSet* getReplSet() {
        return _replSet.get();
    }

private:
    ConnectionString::ConnectionHook* _originalConnectionHook;
    std::unique_ptr<MockReplicaSet> _replSet;
};

TEST_F(ReplicaSetMonitorTest, SeedWithPriOnlySecDown) {
    // Test to make sure that the monitor doesn't crash when
    // ConnectionString::connect returns NULL
    MockReplicaSet* replSet = getReplSet();
    replSet->kill(replSet->getSecondaries());

    // Create a monitor with primary as the only seed list and the two secondaries
    // down so a NULL connection object will be stored for these secondaries in
    // the _nodes vector.
    const string replSetName(replSet->getSetName());
    set<HostAndPort> seedList;
    seedList.insert(HostAndPort(replSet->getPrimary()));
    auto monitor = ReplicaSetMonitor::createIfNeeded(replSetName, seedList);

    replSet->kill(replSet->getPrimary());

    // Trigger connection.
    monitor->runScanForMockReplicaSet();
    monitor.reset();
}

namespace {
/**
 * Takes a repl::ReplSetConfig and a node to remove and returns a new config with equivalent
 * members minus the one specified to be removed.  NOTE: Does not copy over properties of the
 * members other than their id and host.
 */
repl::ReplSetConfig _getConfigWithMemberRemoved(const repl::ReplSetConfig& oldConfig,
                                                const HostAndPort& toRemove) {
    BSONObjBuilder newConfigBuilder;
    newConfigBuilder.append("_id", oldConfig.getReplSetName());
    newConfigBuilder.append("version", oldConfig.getConfigVersion());
    newConfigBuilder.append("protocolVersion", oldConfig.getProtocolVersion());

    BSONArrayBuilder membersBuilder(newConfigBuilder.subarrayStart("members"));
    for (repl::ReplSetConfig::MemberIterator member = oldConfig.membersBegin();
         member != oldConfig.membersEnd();
         ++member) {
        if (member->getHostAndPort() == toRemove) {
            continue;
        }

        membersBuilder.append(BSON("_id" << member->getId().getData() << "host"
                                         << member->getHostAndPort().toString()));
    }

    membersBuilder.done();
    repl::ReplSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(newConfigBuilder.obj()));
    ASSERT_OK(newConfig.validate());
    return newConfig;
}
}  // namespace

// Stress test case for a node that is previously a primary being removed from the set.
// This test goes through configurations with different positions for the primary node
// in the host list returned from the isMaster command. The test here is to make sure
// that the ReplicaSetMonitor will not crash under these situations.
TEST(ReplicaSetMonitorTest, PrimaryRemovedFromSetStress) {
    const size_t NODE_COUNT = 5;
    MockReplicaSet replSet("test", NODE_COUNT);
    ConnectionString::ConnectionHook* originalConnHook = ConnectionString::getConnectionHook();
    ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());

    const string replSetName(replSet.getSetName());
    set<HostAndPort> seedList;
    seedList.insert(HostAndPort(replSet.getPrimary()));
    auto replMonitor = ReplicaSetMonitor::createIfNeeded(replSetName, seedList);

    const repl::ReplSetConfig& origConfig = replSet.getReplConfig();

    for (size_t idxToRemove = 0; idxToRemove < NODE_COUNT; idxToRemove++) {
        replSet.setConfig(origConfig);
        // Make sure the monitor sees the change
        replMonitor->runScanForMockReplicaSet();

        string hostToRemove;
        {
            BSONObjBuilder monitorStateBuilder;
            replMonitor->appendInfo(monitorStateBuilder);
            BSONObj monitorState = monitorStateBuilder.done();

            // Stats are under the replica set name, "test".
            BSONElement hostsElem = monitorState["test"]["hosts"];
            BSONElement addrElem = hostsElem[mongo::str::stream() << idxToRemove]["addr"];
            hostToRemove = addrElem.String();
        }

        replSet.setPrimary(hostToRemove);
        // Make sure the monitor sees the new primary
        replMonitor->runScanForMockReplicaSet();

        repl::ReplSetConfig newConfig =
            _getConfigWithMemberRemoved(origConfig, HostAndPort(hostToRemove));
        replSet.setConfig(newConfig);
        replSet.setPrimary(newConfig.getMemberAt(0).getHostAndPort().toString());
        // Force refresh -> should not crash
        replMonitor->runScanForMockReplicaSet();
    }

    replMonitor.reset();
    ReplicaSetMonitor::cleanup();
    ConnectionString::setConnectionHook(originalConnHook);
    mongo::ScopedDbConnection::clearPool();
}

/**
 * Warning: Tests running this fixture cannot be run in parallel with other tests
 * that use ConnectionString::setConnectionHook.
 */
class TwoNodeWithTags : public mongo::unittest::Test {
protected:
    void setUp() {
        _replSet.reset(new MockReplicaSet("test", 2));
        _originalConnectionHook = ConnectionString::getConnectionHook();
        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());

        repl::ReplSetConfig oldConfig = _replSet->getReplConfig();

        mongo::BSONObjBuilder newConfigBuilder;
        newConfigBuilder.append("_id", oldConfig.getReplSetName());
        newConfigBuilder.append("version", oldConfig.getConfigVersion());
        newConfigBuilder.append("protocolVersion", oldConfig.getProtocolVersion());

        mongo::BSONArrayBuilder membersBuilder(newConfigBuilder.subarrayStart("members"));

        {
            const string host(_replSet->getPrimary());
            const mongo::repl::MemberConfig* member =
                oldConfig.findMemberByHostAndPort(HostAndPort(host));
            membersBuilder.append(BSON("_id" << member->getId().getData() << "host" << host
                                             << "tags"
                                             << BSON("dc"
                                                     << "ny"
                                                     << "num"
                                                     << "1")));
        }

        {
            const string host(_replSet->getSecondaries().front());
            const mongo::repl::MemberConfig* member =
                oldConfig.findMemberByHostAndPort(HostAndPort(host));
            membersBuilder.append(BSON("_id" << member->getId().getData() << "host" << host
                                             << "tags"
                                             << BSON("dc"
                                                     << "ny"
                                                     << "num"
                                                     << "2")));
        }

        membersBuilder.done();

        repl::ReplSetConfig newConfig;
        fassert(28572, newConfig.initialize(newConfigBuilder.done()));
        fassert(28571, newConfig.validate());
        _replSet->setConfig(newConfig);
    }

    void tearDown() {
        ConnectionString::setConnectionHook(_originalConnectionHook);
        ReplicaSetMonitor::cleanup();
        _replSet.reset();
    }

    MockReplicaSet* getReplSet() {
        return _replSet.get();
    }

private:
    ConnectionString::ConnectionHook* _originalConnectionHook;
    std::unique_ptr<MockReplicaSet> _replSet;
};

// Tests the case where the connection to secondary went bad and the replica set
// monitor needs to perform a refresh of it's local view then retry the node selection
// again after the refresh as long as the timeout is > 0.
TEST_F(TwoNodeWithTags, SecDownRetryNoTag) {
    MockReplicaSet* replSet = getReplSet();

    set<HostAndPort> seedList;
    seedList.insert(HostAndPort(replSet->getPrimary()));
    auto monitor = ReplicaSetMonitor::createIfNeeded(replSet->getSetName(), seedList);

    const string secHost(replSet->getSecondaries().front());
    replSet->kill(secHost);

    // Make sure monitor sees the dead secondary
    monitor->runScanForMockReplicaSet();

    replSet->restore(secHost);

    HostAndPort node = monitor
                           ->getHostOrRefresh(ReadPreferenceSetting(
                                                  mongo::ReadPreference::SecondaryOnly, TagSet()),
                                              Milliseconds(1))
                           .get();

    ASSERT_FALSE(monitor->isPrimary(node));
    ASSERT_EQUALS(secHost, node.toString());
    monitor.reset();
}

// Tests the case where the connection to secondary went bad and the replica set
// monitor needs to perform a refresh of it's local view then retry the node selection
// with tags again after the refresh as long as the timeout is > 0.
TEST_F(TwoNodeWithTags, SecDownRetryWithTag) {
    MockReplicaSet* replSet = getReplSet();

    set<HostAndPort> seedList;
    seedList.insert(HostAndPort(replSet->getPrimary()));
    auto monitor = ReplicaSetMonitor::createIfNeeded(replSet->getSetName(), seedList);

    const string secHost(replSet->getSecondaries().front());
    replSet->kill(secHost);

    // Make sure monitor sees the dead secondary
    monitor->runScanForMockReplicaSet();

    replSet->restore(secHost);

    TagSet tags(BSON_ARRAY(BSON("dc"
                                << "ny")));
    HostAndPort node =
        monitor
            ->getHostOrRefresh(ReadPreferenceSetting(mongo::ReadPreference::SecondaryOnly, tags),
                               Milliseconds(1))
            .get();

    ASSERT_FALSE(monitor->isPrimary(node));
    ASSERT_EQUALS(secHost, node.toString());
    monitor.reset();
}

// Tests the case where the connection to secondary went bad and the replica set
// monitor needs to perform a refresh of it's local view, but the scan has an expired timeout.
TEST_F(TwoNodeWithTags, SecDownRetryExpiredTimeout) {
    MockReplicaSet* replSet = getReplSet();

    set<HostAndPort> seedList;
    seedList.insert(HostAndPort(replSet->getPrimary()));
    auto monitor = ReplicaSetMonitor::createIfNeeded(replSet->getSetName(), seedList);

    const string secHost(replSet->getSecondaries().front());
    replSet->kill(secHost);

    // Make sure monitor sees the dead secondary
    monitor->runScanForMockReplicaSet();

    replSet->restore(secHost);

    // This will fail, immediately without doing any refreshing.
    auto errorFut = monitor->getHostOrRefresh(
        ReadPreferenceSetting(mongo::ReadPreference::SecondaryOnly, TagSet()), Milliseconds(0));
    ASSERT(errorFut.isReady());
    ASSERT_EQ(errorFut.getNoThrow().getStatus(), ErrorCodes::FailedToSatisfyReadPreference);

    // Because it did not schedule an expedited scan, it will continue failing until someone waits.
    errorFut = monitor->getHostOrRefresh(
        ReadPreferenceSetting(mongo::ReadPreference::SecondaryOnly, TagSet()), Milliseconds(0));
    ASSERT(errorFut.isReady());
    ASSERT_EQ(errorFut.getNoThrow().getStatus(), ErrorCodes::FailedToSatisfyReadPreference);

    // Negative timeouts are handled the same way
    errorFut = monitor->getHostOrRefresh(
        ReadPreferenceSetting(mongo::ReadPreference::SecondaryOnly, TagSet()), Milliseconds(-1234));
    ASSERT(errorFut.isReady());
    ASSERT_EQ(errorFut.getNoThrow().getStatus(), ErrorCodes::FailedToSatisfyReadPreference);

    // This will trigger a rescan. It is the only call in this test with a non-zero timeout.
    HostAndPort node = monitor
                           ->getHostOrRefresh(ReadPreferenceSetting(
                                                  mongo::ReadPreference::SecondaryOnly, TagSet()),
                                              Milliseconds(1))
                           .get();

    ASSERT_FALSE(monitor->isPrimary(node));
    ASSERT_EQUALS(secHost, node.toString());

    // And this will now succeed.
    node = monitor
               ->getHostOrRefresh(
                   ReadPreferenceSetting(mongo::ReadPreference::SecondaryOnly, TagSet()),
                   Milliseconds(0))
               .get();
    ASSERT_FALSE(monitor->isPrimary(node));
    ASSERT_EQUALS(secHost, node.toString());

    monitor.reset();
}

}  // namespace
}  // namespace mongo
