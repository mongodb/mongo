/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/**
 * This file contains tests for DBClientReplicaSet. The tests mocks the servers
 * the DBClientReplicaSet talks to, so the tests only covers the client side logic.
 */

#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/jsobj.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

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
BSONObj makeMetadata(ReadPreference rp, TagSet tagSet, bool secondaryOk) {
    BSONObjBuilder metadataBob;
    rpc::ServerSelectionMetadata ssm(secondaryOk, ReadPreferenceSetting(rp, tagSet));
    uassertStatusOK(ssm.writeToMetadata(&metadataBob));
    return metadataBob.obj();
}

/**
 * Basic fixture with one primary and one secondary.
 */
class BasicRS : public unittest::Test {
protected:
    void setUp() {
        ReplicaSetMonitor::cleanup();

        // Set the number of consecutive failed checks to 2 so the test doesn't run too long
        ReplicaSetMonitor::maxConsecutiveFailedChecks = 2;

        _replSet.reset(new MockReplicaSet("test", 2));
        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());
    }

    void tearDown() {
        ReplicaSetMonitor::cleanup();
        _replSet.reset();

        mongo::ScopedDbConnection::clearPool();
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
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());
    ReplicaSetMonitor::get(replSet->getSetName())->startOrContinueRefresh().refreshAll();
    bool secondaryOk = (rp != ReadPreference::PrimaryOnly);
    auto tagSet = secondaryOk ? TagSet() : TagSet::primaryOnly();
    // We need the command to be a "SecOk command"
    auto res = replConn.runCommandWithMetadata(
        "foo", "dbStats", makeMetadata(rp, tagSet, secondaryOk), BSON("dbStats" << 1));
    std::unordered_set<HostAndPort> hostSet;
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
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::PrimaryOnly, BSONArray());

    // Note: IdentityNS contains the name of the server.
    unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getPrimary(), doc[HostField.name()].str());
}

TEST_F(BasicRS, CommandPrimary) {
    assertNodeSelected(getReplSet(), ReadPreference::PrimaryOnly, getReplSet()->getPrimary());
}

TEST_F(BasicRS, QuerySecondaryOnly) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::SecondaryOnly, BSONArray());

    // Note: IdentityNS contains the name of the server.
    unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getSecondaries().front(), doc[HostField.name()].str());
}

TEST_F(BasicRS, CommandSecondaryOnly) {
    assertOneOfNodesSelected(
        getReplSet(), ReadPreference::SecondaryOnly, getReplSet()->getSecondaries());
}

TEST_F(BasicRS, QueryPrimaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    // Need up-to-date view, since either host is valid if view is stale.
    ReplicaSetMonitor::get(replSet->getSetName())->startOrContinueRefresh().refreshAll();

    Query query;
    query.readPref(mongo::ReadPreference::PrimaryPreferred, BSONArray());

    // Note: IdentityNS contains the name of the server.
    unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getPrimary(), doc[HostField.name()].str());
}

TEST_F(BasicRS, CommandPrimaryPreferred) {
    assertNodeSelected(getReplSet(), ReadPreference::PrimaryPreferred, getReplSet()->getPrimary());
}

TEST_F(BasicRS, QuerySecondaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    // Need up-to-date view, since either host is valid if view is stale.
    ReplicaSetMonitor::get(replSet->getSetName())->startOrContinueRefresh().refreshAll();

    Query query;
    query.readPref(mongo::ReadPreference::SecondaryPreferred, BSONArray());

    // Note: IdentityNS contains the name of the server.
    unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
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
class AllNodesDown : public unittest::Test {
protected:
    void setUp() {
        ReplicaSetMonitor::cleanup();

        // Set the number of consecutive failed checks to 2 so the test doesn't run too long
        ReplicaSetMonitor::maxConsecutiveFailedChecks = 2;

        _replSet.reset(new MockReplicaSet("test", 2));
        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());

        vector<HostAndPort> hostList(_replSet->getHosts());
        for (vector<HostAndPort>::const_iterator iter = hostList.begin(); iter != hostList.end();
             ++iter) {
            _replSet->kill(iter->toString());
        }
    }

    void tearDown() {
        ReplicaSetMonitor::cleanup();
        _replSet.reset();

        mongo::ScopedDbConnection::clearPool();
    }

    MockReplicaSet* getReplSet() {
        return _replSet.get();
    }

private:
    std::unique_ptr<MockReplicaSet> _replSet;
};

void assertRunCommandWithReadPrefThrows(MockReplicaSet* replSet, ReadPreference rp) {
    bool isPrimaryOnly = (rp == ReadPreference::PrimaryOnly);

    bool secondaryOk = !isPrimaryOnly;
    TagSet ts = isPrimaryOnly ? TagSet::primaryOnly() : TagSet();

    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());
    ASSERT_THROWS(replConn.runCommandWithMetadata(
                      "foo", "whoami", makeMetadata(rp, ts, secondaryOk), BSON("dbStats" << 1)),
                  AssertionException);
}

TEST_F(AllNodesDown, QueryPrimary) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::PrimaryOnly, BSONArray());
    ASSERT_THROWS(replConn.query(IdentityNS, query), AssertionException);
}

TEST_F(AllNodesDown, CommandPrimary) {
    assertRunCommandWithReadPrefThrows(getReplSet(), ReadPreference::PrimaryOnly);
}

TEST_F(AllNodesDown, QuerySecondaryOnly) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::SecondaryOnly, BSONArray());
    ASSERT_THROWS(replConn.query(IdentityNS, query), AssertionException);
}

TEST_F(AllNodesDown, CommandSecondaryOnly) {
    assertRunCommandWithReadPrefThrows(getReplSet(), ReadPreference::SecondaryOnly);
}

TEST_F(AllNodesDown, QueryPrimaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::PrimaryPreferred, BSONArray());
    ASSERT_THROWS(replConn.query(IdentityNS, query), AssertionException);
}

TEST_F(AllNodesDown, CommandPrimaryPreferred) {
    assertRunCommandWithReadPrefThrows(getReplSet(), ReadPreference::PrimaryPreferred);
}

TEST_F(AllNodesDown, QuerySecondaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::SecondaryPreferred, BSONArray());
    ASSERT_THROWS(replConn.query(IdentityNS, query), AssertionException);
}

TEST_F(AllNodesDown, CommandSecondaryPreferred) {
    assertRunCommandWithReadPrefThrows(getReplSet(), ReadPreference::SecondaryPreferred);
}

TEST_F(AllNodesDown, QueryNearest) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::Nearest, BSONArray());
    ASSERT_THROWS(replConn.query(IdentityNS, query), AssertionException);
}

TEST_F(AllNodesDown, CommandNearest) {
    assertRunCommandWithReadPrefThrows(getReplSet(), ReadPreference::Nearest);
}

/**
 * Setup for 2 member replica set with the primary down.
 */
class PrimaryDown : public unittest::Test {
protected:
    void setUp() {
        ReplicaSetMonitor::cleanup();

        // Set the number of consecutive failed checks to 2 so the test doesn't run too long
        ReplicaSetMonitor::maxConsecutiveFailedChecks = 2;

        _replSet.reset(new MockReplicaSet("test", 2));
        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());
        _replSet->kill(_replSet->getPrimary());
    }

    void tearDown() {
        ReplicaSetMonitor::cleanup();
        _replSet.reset();

        mongo::ScopedDbConnection::clearPool();
    }

    MockReplicaSet* getReplSet() {
        return _replSet.get();
    }

private:
    std::unique_ptr<MockReplicaSet> _replSet;
};

TEST_F(PrimaryDown, QueryPrimary) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::PrimaryOnly, BSONArray());
    ASSERT_THROWS(replConn.query(IdentityNS, query), AssertionException);
}

TEST_F(PrimaryDown, CommandPrimary) {
    assertRunCommandWithReadPrefThrows(getReplSet(), ReadPreference::PrimaryOnly);
}

TEST_F(PrimaryDown, QuerySecondaryOnly) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::SecondaryOnly, BSONArray());

    // Note: IdentityNS contains the name of the server.
    unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getSecondaries().front(), doc[HostField.name()].str());
}

TEST_F(PrimaryDown, CommandSecondaryOnly) {
    assertOneOfNodesSelected(
        getReplSet(), ReadPreference::SecondaryOnly, getReplSet()->getSecondaries());
}

TEST_F(PrimaryDown, QueryPrimaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::PrimaryPreferred, BSONArray());

    // Note: IdentityNS contains the name of the server.
    unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getSecondaries().front(), doc[HostField.name()].str());
}

TEST_F(PrimaryDown, CommandPrimaryPreferred) {
    assertOneOfNodesSelected(
        getReplSet(), ReadPreference::PrimaryPreferred, getReplSet()->getSecondaries());
}

TEST_F(PrimaryDown, QuerySecondaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::SecondaryPreferred, BSONArray());

    // Note: IdentityNS contains the name of the server.
    unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getSecondaries().front(), doc[HostField.name()].str());
}

TEST_F(PrimaryDown, CommandSecondaryPreferred) {
    assertOneOfNodesSelected(
        getReplSet(), ReadPreference::SecondaryPreferred, getReplSet()->getSecondaries());
}

TEST_F(PrimaryDown, Nearest) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::Nearest, BSONArray());
    unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getSecondaries().front(), doc[HostField.name()].str());
}

/**
 * Setup for 2 member replica set with the secondary down.
 */
class SecondaryDown : public unittest::Test {
protected:
    void setUp() {
        ReplicaSetMonitor::cleanup();

        // Set the number of consecutive failed checks to 2 so the test doesn't run too long
        ReplicaSetMonitor::maxConsecutiveFailedChecks = 2;

        _replSet.reset(new MockReplicaSet("test", 2));
        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());

        _replSet->kill(_replSet->getSecondaries().front());
    }

    void tearDown() {
        ReplicaSetMonitor::cleanup();
        _replSet.reset();

        mongo::ScopedDbConnection::clearPool();
    }

    MockReplicaSet* getReplSet() {
        return _replSet.get();
    }

private:
    std::unique_ptr<MockReplicaSet> _replSet;
};

TEST_F(SecondaryDown, QueryPrimary) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::PrimaryOnly, BSONArray());

    // Note: IdentityNS contains the name of the server.
    unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getPrimary(), doc[HostField.name()].str());
}

TEST_F(SecondaryDown, CommandPrimary) {
    assertNodeSelected(getReplSet(), ReadPreference::PrimaryOnly, getReplSet()->getPrimary());
}

TEST_F(SecondaryDown, QuerySecondaryOnly) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::SecondaryOnly, BSONArray());
    ASSERT_THROWS(replConn.query(IdentityNS, query), AssertionException);
}

TEST_F(SecondaryDown, CommandSecondaryOnly) {
    assertRunCommandWithReadPrefThrows(getReplSet(), ReadPreference::SecondaryOnly);
}

TEST_F(SecondaryDown, QueryPrimaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::PrimaryPreferred, BSONArray());

    // Note: IdentityNS contains the name of the server.
    unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getPrimary(), doc[HostField.name()].str());
}

TEST_F(SecondaryDown, CommandPrimaryPreferred) {
    assertNodeSelected(getReplSet(), ReadPreference::PrimaryPreferred, getReplSet()->getPrimary());
}

TEST_F(SecondaryDown, QuerySecondaryPreferred) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::SecondaryPreferred, BSONArray());

    // Note: IdentityNS contains the name of the server.
    unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
    BSONObj doc = cursor->next();
    ASSERT_EQUALS(replSet->getPrimary(), doc[HostField.name()].str());
}

TEST_F(SecondaryDown, CommandSecondaryPreferred) {
    assertNodeSelected(getReplSet(), ReadPreference::PrimaryPreferred, getReplSet()->getPrimary());
}

TEST_F(SecondaryDown, QueryNearest) {
    MockReplicaSet* replSet = getReplSet();
    DBClientReplicaSet replConn(replSet->getSetName(), replSet->getHosts());

    Query query;
    query.readPref(mongo::ReadPreference::Nearest, BSONArray());

    // Note: IdentityNS contains the name of the server.
    unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
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
class TaggedFiveMemberRS : public unittest::Test {
protected:
    void setUp() {
        // Tests for pinning behavior require this.
        ReplicaSetMonitor::useDeterministicHostSelection = true;

        // This shuts down the background RSMWatcher thread and prevents it from running. These
        // tests depend on controlling when the RSMs are updated.
        ReplicaSetMonitor::cleanup();

        // Set the number of consecutive failed checks to 2 so the test doesn't run too long
        ReplicaSetMonitor::maxConsecutiveFailedChecks = 2;

        _replSet.reset(new MockReplicaSet("test", 5));
        _originalConnectionHook = ConnectionString::getConnectionHook();
        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());

        {
            mongo::repl::ReplicaSetConfig oldConfig = _replSet->getReplConfig();

            mongo::BSONObjBuilder newConfigBuilder;
            newConfigBuilder.append("_id", oldConfig.getReplSetName());
            newConfigBuilder.append("version", oldConfig.getConfigVersion());

            mongo::BSONArrayBuilder membersBuilder(newConfigBuilder.subarrayStart("members"));
            {
                const string host(_replSet->getPrimary());
                const mongo::repl::MemberConfig* member =
                    oldConfig.findMemberByHostAndPort(HostAndPort(host));
                membersBuilder.append(
                    BSON("_id" << member->getId() << "host" << host << "tags" << BSON("dc"
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
                membersBuilder.append(
                    BSON("_id" << member->getId() << "host" << host << "tags" << BSON("dc"
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
                membersBuilder.append(
                    BSON("_id" << member->getId() << "host" << host << "tags" << BSON("dc"
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
                membersBuilder.append(
                    BSON("_id" << member->getId() << "host" << host << "tags" << BSON("dc"
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
                membersBuilder.append(
                    BSON("_id" << member->getId() << "host" << host << "tags" << BSON("dc"
                                                                                      << "jp"
                                                                                      << "s"
                                                                                      << "4")));
                _replSet->getNode(host)->insert(IdentityNS, BSON(HostField(host)));
            }

            membersBuilder.done();
            mongo::repl::ReplicaSetConfig newConfig;
            fassert(28569, newConfig.initialize(newConfigBuilder.done()));
            fassert(28568, newConfig.validate());
            _replSet->setConfig(newConfig);
        }
    }

    void tearDown() {
        ReplicaSetMonitor::useDeterministicHostSelection = false;

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

TEST_F(TaggedFiveMemberRS, ConnShouldPinIfSameSettings) {
    MockReplicaSet* replSet = getReplSet();
    vector<HostAndPort> seedList;
    seedList.push_back(HostAndPort(replSet->getPrimary()));

    DBClientReplicaSet replConn(replSet->getSetName(), seedList);

    string dest;
    {
        Query query;
        query.readPref(mongo::ReadPreference::PrimaryPreferred, BSONArray());

        // Note: IdentityNS contains the name of the server.
        unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
        BSONObj doc = cursor->next();
        dest = doc[HostField.name()].str();
    }

    {
        Query query;
        query.readPref(mongo::ReadPreference::PrimaryPreferred, BSONArray());
        unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
        BSONObj doc = cursor->next();
        const string newDest = doc[HostField.name()].str();
        ASSERT_EQUALS(dest, newDest);
    }
}

TEST_F(TaggedFiveMemberRS, ConnShouldNotPinIfHostMarkedAsFailed) {
    MockReplicaSet* replSet = getReplSet();
    vector<HostAndPort> seedList;
    seedList.push_back(HostAndPort(replSet->getPrimary()));

    DBClientReplicaSet replConn(replSet->getSetName(), seedList);

    string dest;
    {
        Query query;
        query.readPref(mongo::ReadPreference::PrimaryPreferred, BSONArray());

        // Note: IdentityNS contains the name of the server.
        unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
        BSONObj doc = cursor->next();
        dest = doc[HostField.name()].str();
    }

    // This is the only difference from ConnShouldPinIfSameSettings which tests that we *do* pin
    // in if the host is still marked as up. Note that this only notifies the RSM, and does not
    // directly effect the DBClientRS.
    ReplicaSetMonitor::get(replSet->getSetName())->failedHost(HostAndPort(dest));

    {
        Query query;
        query.readPref(mongo::ReadPreference::PrimaryPreferred, BSONArray());
        unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
        BSONObj doc = cursor->next();
        const string newDest = doc[HostField.name()].str();
        ASSERT_NOT_EQUALS(dest, newDest);
    }
}

TEST_F(TaggedFiveMemberRS, ConnShouldNotPinIfDiffMode) {
    MockReplicaSet* replSet = getReplSet();
    vector<HostAndPort> seedList;
    seedList.push_back(HostAndPort(replSet->getPrimary()));

    DBClientReplicaSet replConn(replSet->getSetName(), seedList);

    // Need up-to-date view to ensure there are multiple valid choices.
    ReplicaSetMonitor::get(replSet->getSetName())->startOrContinueRefresh().refreshAll();

    string dest;
    {
        Query query;
        query.readPref(mongo::ReadPreference::SecondaryPreferred, BSONArray());

        // Note: IdentityNS contains the name of the server.
        unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
        BSONObj doc = cursor->next();
        dest = doc[HostField.name()].str();
        ASSERT_NOT_EQUALS(dest, replSet->getPrimary());
    }

    {
        Query query;
        query.readPref(mongo::ReadPreference::SecondaryOnly, BSONArray());
        unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
        BSONObj doc = cursor->next();
        const string newDest = doc[HostField.name()].str();
        ASSERT_NOT_EQUALS(dest, newDest);
    }
}

TEST_F(TaggedFiveMemberRS, ConnShouldNotPinIfDiffTag) {
    MockReplicaSet* replSet = getReplSet();
    vector<HostAndPort> seedList;
    seedList.push_back(HostAndPort(replSet->getPrimary()));

    DBClientReplicaSet replConn(replSet->getSetName(), seedList);

    // Need up-to-date view to ensure there are multiple valid choices.
    ReplicaSetMonitor::get(replSet->getSetName())->startOrContinueRefresh().refreshAll();

    string dest;
    {
        Query query;
        query.readPref(mongo::ReadPreference::SecondaryPreferred,
                       BSON_ARRAY(BSON("dc"
                                       << "sf")));

        // Note: IdentityNS contains the name of the server.
        unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
        BSONObj doc = cursor->next();
        dest = doc[HostField.name()].str();
        ASSERT_NOT_EQUALS(dest, replSet->getPrimary());
    }

    {
        Query query;
        vector<pair<string, string>> tagSet;
        query.readPref(mongo::ReadPreference::SecondaryPreferred, BSON_ARRAY(BSON("group" << 1)));
        unique_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
        BSONObj doc = cursor->next();
        const string newDest = doc[HostField.name()].str();
        ASSERT_NOT_EQUALS(dest, newDest);
    }
}

// Note: slaveConn is dangerous and should be deprecated! Also see SERVER-7801.
TEST_F(TaggedFiveMemberRS, SlaveConnReturnsSecConn) {
    MockReplicaSet* replSet = getReplSet();
    vector<HostAndPort> seedList;
    seedList.push_back(HostAndPort(replSet->getPrimary()));

    DBClientReplicaSet replConn(replSet->getSetName(), seedList);

    // Need up-to-date view since slaveConn() uses SecondaryPreferred, and this test assumes it
    // knows about at least one secondary.
    ReplicaSetMonitor::get(replSet->getSetName())->startOrContinueRefresh().refreshAll();

    string dest;
    mongo::DBClientConnection& secConn = replConn.slaveConn();

    // Note: IdentityNS contains the name of the server.
    unique_ptr<DBClientCursor> cursor = secConn.query(IdentityNS, Query());
    BSONObj doc = cursor->next();
    dest = doc[HostField.name()].str();
    ASSERT_NOT_EQUALS(dest, replSet->getPrimary());
}

}  // namespace
}  // namespace mongo
