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

/**
 * This file contains tests for DBClientReplicaSet. The tests mocks the servers
 * the DBClientReplicaSet talks to, so the tests only covers the client side logic.
 */

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <absl/container/node_hash_map.h>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/sdam/mock_topology_manager.h"
#include "mongo/client/streamable_replica_set_monitor_for_testing.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/member_id.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

using std::make_pair;
using std::map;
using std::pair;
using std::string;
using std::unique_ptr;
using std::vector;

/**
 * Constructs a metadata object containing the passed server selection metadata.
 */
BSONObj makeMetadata(ReadPreference rp, TagSet tagSet) {
    return ReadPreferenceSetting(rp, std::move(tagSet)).toContainingBSON();
}

/**
 * Ensures a global ServiceContext exists.
 */
class DBClientRSTest : public unittest::Test {
public:
    ClockSource* clock() {
        return _clkSource.get();
    }

    sdam::MockTopologyManager* getTopologyManager() {
        return _rsmMonitor.getTopologyManager();
    }

protected:
    void setUp() {
        auto serviceContext = ServiceContext::make();
        setGlobalServiceContext(std::move(serviceContext));
    }

    std::shared_ptr<ClockSourceMock> _clkSource = std::make_shared<ClockSourceMock>();
    StreamableReplicaSetMonitorForTesting _rsmMonitor;

private:
    RAIIServerParameterControllerForTest _findHostTimeout{"defaultFindReplicaSetHostTimeoutMS",
                                                          100};
};

/**
 * Basic fixture with one primary and one secondary.
 */
class BasicRS : public DBClientRSTest {
protected:
    void setUp() {
        DBClientRSTest::setUp();
        ReplicaSetMonitor::cleanup();

        _replSet.reset(new MockReplicaSet("test", 2));
        _rsmMonitor.setup(_replSet->getURI());
        getTopologyManager()->setTopologyDescription(_replSet->getTopologyDescription(clock()));

        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());
    }

    void tearDown() {
        _replSet.reset();

        mongo::ScopedDbConnection::clearPool();

        ReplicaSetMonitor::shutdown();
        DBClientRSTest::tearDown();
    }

    MockReplicaSet* getReplSet() {
        return _replSet.get();
    }

private:
    std::unique_ptr<MockReplicaSet> _replSet;
};

void assertOneOfNodesSelected(MockReplicaSet* replSet,
                              ReadPreference rp,
                              const std::vector<std::string> hostNames) {
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());
    bool secondaryOk = (rp != ReadPreference::PrimaryOnly);
    auto tagSet = secondaryOk ? TagSet() : TagSet::primaryOnly();
    // We need the command to be a "SecOk command"
    auto res = replConn.runCommand(
        OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::kNotRequired,
                                    DatabaseName::createDatabaseName_forTest(boost::none, "foo"),
                                    BSON("dbStats" << 1),
                                    makeMetadata(rp, tagSet)));
    stdx::unordered_set<HostAndPort> hostSet;
    for (const auto& hostName : hostNames) {
        hostSet.emplace(hostName);
    }
    ASSERT_EQ(hostSet.count(HostAndPort{res->getCommandReply()["host"].str()}), 1u);
}

void assertNodeSelected(MockReplicaSet* replSet, ReadPreference rp, StringData host) {
    assertOneOfNodesSelected(replSet, rp, std::vector<std::string>{host.toString()});
}

TEST_F(BasicRS, QueryPrimary) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    // Note: IdentityNS contains the name of the server.
    FindCommandRequest findCmd{IdentityNS};
    auto cursor =
        replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::PrimaryOnly});
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getPrimary(), doc[HostField.name()].str());
}

TEST_F(BasicRS, CommandPrimary) {
    assertNodeSelected(getReplSet(), ReadPreference::PrimaryOnly, getReplSet()->getPrimary());
}

TEST_F(BasicRS, QuerySecondaryOnly) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    // Note: IdentityNS contains the name of the server.
    FindCommandRequest findCmd{IdentityNS};
    auto cursor =
        replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::SecondaryOnly});
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getSecondaries().front(), doc[HostField.name()].str());
}

TEST_F(BasicRS, CommandSecondaryOnly) {
    assertOneOfNodesSelected(
        getReplSet(), ReadPreference::SecondaryOnly, getReplSet()->getSecondaries());
}

TEST_F(BasicRS, QueryPrimaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    // Note: IdentityNS contains the name of the server.
    FindCommandRequest findCmd{IdentityNS};
    auto cursor =
        replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::PrimaryPreferred});
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getPrimary(), doc[HostField.name()].str());
}

TEST_F(BasicRS, CommandPrimaryPreferred) {
    assertNodeSelected(getReplSet(), ReadPreference::PrimaryPreferred, getReplSet()->getPrimary());
}

TEST_F(BasicRS, QuerySecondaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    // Note: IdentityNS contains the name of the server.
    FindCommandRequest findCmd{IdentityNS};
    auto cursor = replConn.find(std::move(findCmd),
                                ReadPreferenceSetting{ReadPreference::SecondaryPreferred});
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getSecondaries().front(), doc[HostField.name()].str());
}

TEST_F(BasicRS, CommandSecondaryPreferred) {
    assertOneOfNodesSelected(
        getReplSet(), ReadPreference::SecondaryPreferred, getReplSet()->getSecondaries());
}

/**
 * Setup for 2 member replica set will all of the nodes down.
 */
class AllNodesDown : public DBClientRSTest {
protected:
    void setUp() {
        DBClientRSTest::setUp();
        ReplicaSetMonitor::cleanup();

        _replSet.reset(new MockReplicaSet("test", 2));
        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());

        vector<HostAndPort> hostList(_replSet->getHosts());
        for (vector<HostAndPort>::const_iterator iter = hostList.begin(); iter != hostList.end();
             ++iter) {
            _replSet->kill(iter->toString());
        }

        _rsmMonitor.setup(_replSet->getURI());
        getTopologyManager()->setTopologyDescription(_replSet->getTopologyDescription(clock()));
    }

    void tearDown() {
        ReplicaSetMonitor::cleanup();
        _replSet.reset();

        mongo::ScopedDbConnection::clearPool();
        DBClientRSTest::tearDown();
    }

    MockReplicaSet* getReplSet() {
        return _replSet.get();
    }

private:
    std::unique_ptr<MockReplicaSet> _replSet;
};

void assertRunCommandWithReadPrefThrows(MockReplicaSet* replSet, ReadPreference rp) {
    bool isPrimaryOnly = (rp == ReadPreference::PrimaryOnly);
    TagSet ts = isPrimaryOnly ? TagSet::primaryOnly() : TagSet();

    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());
    ASSERT_THROWS(replConn.runCommand(OpMsgRequestBuilder::create(
                      auth::ValidatedTenancyScope::kNotRequired,
                      DatabaseName::createDatabaseName_forTest(boost::none, "foo"),
                      BSON("dbStats" << 1),
                      makeMetadata(rp, ts))),
                  AssertionException);
}

TEST_F(AllNodesDown, QueryPrimary) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    FindCommandRequest findCmd{IdentityNS};
    ASSERT_THROWS(
        replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::PrimaryOnly}),
        AssertionException);
}

TEST_F(AllNodesDown, CommandPrimary) {
    assertRunCommandWithReadPrefThrows(getReplSet(), ReadPreference::PrimaryOnly);
}

TEST_F(AllNodesDown, QuerySecondaryOnly) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    FindCommandRequest findCmd{IdentityNS};
    ASSERT_THROWS(
        replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::SecondaryOnly}),
        AssertionException);
}

TEST_F(AllNodesDown, CommandSecondaryOnly) {
    assertRunCommandWithReadPrefThrows(getReplSet(), ReadPreference::SecondaryOnly);
}

TEST_F(AllNodesDown, QueryPrimaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    FindCommandRequest findCmd{IdentityNS};
    ASSERT_THROWS(
        replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::PrimaryPreferred}),
        AssertionException);
}

TEST_F(AllNodesDown, CommandPrimaryPreferred) {
    assertRunCommandWithReadPrefThrows(getReplSet(), ReadPreference::PrimaryPreferred);
}

TEST_F(AllNodesDown, QuerySecondaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    FindCommandRequest findCmd{IdentityNS};
    ASSERT_THROWS(replConn.find(std::move(findCmd),
                                ReadPreferenceSetting{ReadPreference::SecondaryPreferred}),
                  AssertionException);
}

TEST_F(AllNodesDown, CommandSecondaryPreferred) {
    assertRunCommandWithReadPrefThrows(getReplSet(), ReadPreference::SecondaryPreferred);
}

TEST_F(AllNodesDown, QueryNearest) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    FindCommandRequest findCmd{IdentityNS};
    ASSERT_THROWS(replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::Nearest}),
                  AssertionException);
}

TEST_F(AllNodesDown, CommandNearest) {
    assertRunCommandWithReadPrefThrows(getReplSet(), ReadPreference::Nearest);
}

/**
 * Setup for 2 member replica set with the primary down.
 */
class PrimaryDown : public DBClientRSTest {
protected:
    void setUp() {
        DBClientRSTest::setUp();
        ReplicaSetMonitor::cleanup();

        _replSet.reset(new MockReplicaSet("test", 2));
        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());
        _replSet->kill(_replSet->getPrimary());

        _rsmMonitor.setup(_replSet->getURI());
        getTopologyManager()->setTopologyDescription(_replSet->getTopologyDescription(clock()));
    }

    void tearDown() {
        ReplicaSetMonitor::cleanup();
        _replSet.reset();

        mongo::ScopedDbConnection::clearPool();
        DBClientRSTest::tearDown();
    }

    MockReplicaSet* getReplSet() {
        return _replSet.get();
    }

private:
    std::unique_ptr<MockReplicaSet> _replSet;
};

TEST_F(PrimaryDown, QueryPrimary) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    FindCommandRequest findCmd{IdentityNS};
    ASSERT_THROWS(
        replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::PrimaryOnly}),
        AssertionException);
}

TEST_F(PrimaryDown, CommandPrimary) {
    assertRunCommandWithReadPrefThrows(getReplSet(), ReadPreference::PrimaryOnly);
}

TEST_F(PrimaryDown, QuerySecondaryOnly) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    // Note: IdentityNS contains the name of the server.
    FindCommandRequest findCmd{IdentityNS};
    auto cursor =
        replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::SecondaryOnly});
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getSecondaries().front(), doc[HostField.name()].str());
}

TEST_F(PrimaryDown, CommandSecondaryOnly) {
    assertOneOfNodesSelected(
        getReplSet(), ReadPreference::SecondaryOnly, getReplSet()->getSecondaries());
}

TEST_F(PrimaryDown, QueryPrimaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    // Note: IdentityNS contains the name of the server.
    FindCommandRequest findCmd{IdentityNS};
    auto cursor =
        replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::PrimaryPreferred});
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getSecondaries().front(), doc[HostField.name()].str());
}

TEST_F(PrimaryDown, CommandPrimaryPreferred) {
    assertOneOfNodesSelected(
        getReplSet(), ReadPreference::PrimaryPreferred, getReplSet()->getSecondaries());
}

TEST_F(PrimaryDown, QuerySecondaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    // Note: IdentityNS contains the name of the server.
    FindCommandRequest findCmd{IdentityNS};
    auto cursor = replConn.find(std::move(findCmd),
                                ReadPreferenceSetting{ReadPreference::SecondaryPreferred});
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getSecondaries().front(), doc[HostField.name()].str());
}

TEST_F(PrimaryDown, CommandSecondaryPreferred) {
    assertOneOfNodesSelected(
        getReplSet(), ReadPreference::SecondaryPreferred, getReplSet()->getSecondaries());
}

TEST_F(PrimaryDown, Nearest) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    FindCommandRequest findCmd{IdentityNS};
    auto cursor = replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::Nearest});
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getSecondaries().front(), doc[HostField.name()].str());
}

/**
 * Setup for 2 member replica set with the secondary down.
 */
class SecondaryDown : public DBClientRSTest {
protected:
    void setUp() {
        DBClientRSTest::setUp();
        ReplicaSetMonitor::cleanup();

        _replSet.reset(new MockReplicaSet("test", 2));
        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());

        _replSet->kill(_replSet->getSecondaries().front());

        _rsmMonitor.setup(_replSet->getURI());
        getTopologyManager()->setTopologyDescription(_replSet->getTopologyDescription(clock()));
    }

    void tearDown() {
        ReplicaSetMonitor::cleanup();
        _replSet.reset();

        mongo::ScopedDbConnection::clearPool();
        DBClientRSTest::tearDown();
    }

    MockReplicaSet* getReplSet() {
        return _replSet.get();
    }

private:
    std::unique_ptr<MockReplicaSet> _replSet;
};

TEST_F(SecondaryDown, QueryPrimary) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    // Note: IdentityNS contains the name of the server.
    FindCommandRequest findCmd{IdentityNS};
    auto cursor =
        replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::PrimaryOnly});
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getPrimary(), doc[HostField.name()].str());
}

TEST_F(SecondaryDown, CommandPrimary) {
    assertNodeSelected(getReplSet(), ReadPreference::PrimaryOnly, getReplSet()->getPrimary());
}

TEST_F(SecondaryDown, QuerySecondaryOnly) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    FindCommandRequest findCmd{IdentityNS};
    ASSERT_THROWS(
        replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::SecondaryOnly}),
        AssertionException);
}

TEST_F(SecondaryDown, CommandSecondaryOnly) {
    assertRunCommandWithReadPrefThrows(getReplSet(), ReadPreference::SecondaryOnly);
}

TEST_F(SecondaryDown, QueryPrimaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    // Note: IdentityNS contains the name of the server.
    FindCommandRequest findCmd{IdentityNS};
    auto cursor =
        replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::PrimaryPreferred});
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getPrimary(), doc[HostField.name()].str());
}

TEST_F(SecondaryDown, CommandPrimaryPreferred) {
    assertNodeSelected(getReplSet(), ReadPreference::PrimaryPreferred, getReplSet()->getPrimary());
}

TEST_F(SecondaryDown, QuerySecondaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    FindCommandRequest findCmd{IdentityNS};
    auto cursor = replConn.find(std::move(findCmd),
                                ReadPreferenceSetting{ReadPreference::SecondaryPreferred});
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getPrimary(), doc[HostField.name()].str());
}

TEST_F(SecondaryDown, CommandSecondaryPreferred) {
    assertNodeSelected(getReplSet(), ReadPreference::PrimaryPreferred, getReplSet()->getPrimary());
}

TEST_F(SecondaryDown, QueryNearest) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts(), StringData());

    FindCommandRequest findCmd{IdentityNS};
    auto cursor = replConn.find(std::move(findCmd), ReadPreferenceSetting{ReadPreference::Nearest});
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getPrimary(), doc[HostField.name()].str());
}

TEST_F(SecondaryDown, CommandNearest) {
    assertNodeSelected(getReplSet(), ReadPreference::Nearest, getReplSet()->getPrimary());
}

/**
 * Warning: Tests running this fixture cannot be run in parallel with other tests
 * that uses ConnectionString::setConnectionHook
 */
class TaggedFiveMemberRS : public DBClientRSTest {
protected:
    void setUp() {
        DBClientRSTest::setUp();

        // This shuts down the background RSMWatcher thread and prevents it from running. These
        // tests depend on controlling when the RSMs are updated.
        ReplicaSetMonitor::cleanup();

        _replSet.reset(new MockReplicaSet("test", 5));
        _originalConnectionHook = ConnectionString::getConnectionHook();
        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());

        {
            mongo::repl::ReplSetConfig oldConfig = _replSet->getReplConfig();

            mongo::BSONObjBuilder newConfigBuilder;
            newConfigBuilder.append("_id", oldConfig.getReplSetName());
            newConfigBuilder.append("version", oldConfig.getConfigVersion());
            newConfigBuilder.append("protocolVersion", 1);

            mongo::BSONArrayBuilder membersBuilder(newConfigBuilder.subarrayStart("members"));
            {
                const string host(_replSet->getPrimary());
                const mongo::repl::MemberConfig* member =
                    oldConfig.findMemberByHostAndPort(HostAndPort(host));
                membersBuilder.append(BSON("_id" << member->getId().getData() << "host" << host
                                                 << "tags"
                                                 << BSON("dc"
                                                         << "ny"
                                                         << "p"
                                                         << "1")));
                _replSet->getNode(host)->insert(IdentityNS, BSON(HostField(host)));
            }

            vector<string> secNodes = _replSet->getSecondaries();
            vector<string>::const_iterator secIter = secNodes.begin();

            {
                const string host(*secIter);
                const mongo::repl::MemberConfig* member =
                    oldConfig.findMemberByHostAndPort(HostAndPort(host));
                membersBuilder.append(BSON("_id" << member->getId().getData() << "host" << host
                                                 << "tags"
                                                 << BSON("dc"
                                                         << "sf"
                                                         << "s"
                                                         << "1"
                                                         << "group"
                                                         << "1")));
                _replSet->getNode(host)->insert(IdentityNS, BSON(HostField(host)));
            }

            {
                ++secIter;
                const string host(*secIter);
                const mongo::repl::MemberConfig* member =
                    oldConfig.findMemberByHostAndPort(HostAndPort(host));
                membersBuilder.append(BSON("_id" << member->getId().getData() << "host" << host
                                                 << "tags"
                                                 << BSON("dc"
                                                         << "ma"
                                                         << "s"
                                                         << "2"
                                                         << "group"
                                                         << "1")));
                _replSet->getNode(host)->insert(IdentityNS, BSON(HostField(host)));
            }

            {
                ++secIter;
                const string host(*secIter);
                const mongo::repl::MemberConfig* member =
                    oldConfig.findMemberByHostAndPort(HostAndPort(host));
                membersBuilder.append(BSON("_id" << member->getId().getData() << "host" << host
                                                 << "tags"
                                                 << BSON("dc"
                                                         << "eu"
                                                         << "s"
                                                         << "3")));
                _replSet->getNode(host)->insert(IdentityNS, BSON(HostField(host)));
            }

            {
                ++secIter;
                const string host(*secIter);
                const mongo::repl::MemberConfig* member =
                    oldConfig.findMemberByHostAndPort(HostAndPort(host));
                membersBuilder.append(BSON("_id" << member->getId().getData() << "host" << host
                                                 << "tags"
                                                 << BSON("dc"
                                                         << "jp"
                                                         << "s"
                                                         << "4")));
                _replSet->getNode(host)->insert(IdentityNS, BSON(HostField(host)));
            }

            membersBuilder.done();
            auto newConfig = mongo::repl::ReplSetConfig::parse(newConfigBuilder.done());
            fassert(28568, newConfig.validate());
            _replSet->setConfig(newConfig);
        }

        _rsmMonitor.setup(_replSet->getURI());
        getTopologyManager()->setTopologyDescription(_replSet->getTopologyDescription(clock()));
    }

    void tearDown() {
        ConnectionString::setConnectionHook(_originalConnectionHook);
        ReplicaSetMonitor::cleanup();
        _replSet.reset();

        mongo::ScopedDbConnection::clearPool();
        DBClientRSTest::tearDown();
    }

    MockReplicaSet* getReplSet() {
        return _replSet.get();
    }

private:
    ConnectionString::ConnectionHook* _originalConnectionHook;
    std::unique_ptr<MockReplicaSet> _replSet;
};

TEST_F(TaggedFiveMemberRS, ConnShouldPinIfSameSettings) {
    MockReplicaSet* replSet = getReplSet();
    vector<HostAndPort> seedList;
    seedList.push_back(HostAndPort(replSet->getPrimary()));

    DBClientReplicaSet replConn(replSet->getSetName(), seedList, StringData());

    string dest;
    {
        // Note: IdentityNS contains the name of the server.
        std::unique_ptr<DBClientCursor> cursor =
            replConn.find(FindCommandRequest{IdentityNS},
                          ReadPreferenceSetting{ReadPreference::PrimaryPreferred});
        BSONObj doc = cursor->next();
        dest = doc[HostField.name()].str();
    }

    {
        std::unique_ptr<DBClientCursor> cursor =
            replConn.find(FindCommandRequest{IdentityNS},
                          ReadPreferenceSetting{ReadPreference::PrimaryPreferred});
        BSONObj doc = cursor->next();
        const string newDest = doc[HostField.name()].str();
        ASSERT_EQUALS(dest, newDest);
    }
}

TEST_F(TaggedFiveMemberRS, ConnShouldNotPinIfHostMarkedAsFailed) {
    MockReplicaSet* replSet = getReplSet();
    vector<HostAndPort> seedList;
    seedList.push_back(HostAndPort(replSet->getPrimary()));

    DBClientReplicaSet replConn(replSet->getSetName(), seedList, StringData());

    string dest;
    {
        // Note: IdentityNS contains the name of the server.
        std::unique_ptr<DBClientCursor> cursor =
            replConn.find(FindCommandRequest{IdentityNS},
                          ReadPreferenceSetting{ReadPreference::PrimaryPreferred});
        BSONObj doc = cursor->next();
        dest = doc[HostField.name()].str();
    }

    // This is the only difference from ConnShouldPinIfSameSettings which tests that we *do* pin
    // in if the host is still marked as up. Note that this only notifies the RSM, and does not
    // directly effect the DBClientRS.
    getReplSet()->getNode(dest)->shutdown();
    getTopologyManager()->setTopologyDescription(getReplSet()->getTopologyDescription(clock()));

    {
        std::unique_ptr<DBClientCursor> cursor =
            replConn.find(FindCommandRequest{IdentityNS},
                          ReadPreferenceSetting{ReadPreference::PrimaryPreferred});
        BSONObj doc = cursor->next();
        const string newDest = doc[HostField.name()].str();
        ASSERT_NOT_EQUALS(dest, newDest);
    }
}

// Note: secondaryConn is dangerous and should be deprecated! Also see SERVER-7801.
TEST_F(TaggedFiveMemberRS, SecondaryConnReturnsSecConn) {
    MockReplicaSet* replSet = getReplSet();
    vector<HostAndPort> seedList;
    seedList.push_back(HostAndPort(replSet->getPrimary()));

    DBClientReplicaSet replConn(replSet->getSetName(), seedList, StringData());

    mongo::DBClientConnection& secConn = replConn.secondaryConn();

    // Note: IdentityNS contains the name of the server.
    std::unique_ptr<DBClientCursor> cursor = secConn.find(FindCommandRequest{IdentityNS});
    BSONObj doc = cursor->next();
    std::string dest = doc[HostField.name()].str();
    ASSERT_NOT_EQUALS(dest, replSet->getPrimary());
}

}  // namespace
}  // namespace mongo
