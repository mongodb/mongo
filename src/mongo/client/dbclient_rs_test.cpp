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
 */

/**
 * This file contains tests for DBClientReplicaSet.
 */

#include "mongo/bson/bson_field.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/unittest/unittest.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

using std::auto_ptr;
using std::map;
using std::make_pair;
using std::pair;
using std::string;
using std::vector;
using boost::scoped_ptr;

using mongo::BSONField;
using mongo::BSONObj;
using mongo::BSONArray;
using mongo::BSONElement;
using mongo::ConnectionString;
using mongo::DBClientCursor;
using mongo::DBClientReplicaSet;
using mongo::HostAndPort;
using mongo::MockReplicaSet;
using mongo::Query;
using mongo::ReadPreference;
using mongo::ReplicaSetMonitor;
using mongo::ScopedDbConnection;
using mongo::TagSet;

namespace mongo {
    // Symbols defined to build the binary correctly.
    CmdLine cmdLine;

    bool inShutdown() {
        return false;
    }

    DBClientBase *createDirectClient() { return NULL; }

    void dbexit(ExitCode rc, const char *why){
        ::_exit(rc);
    }

    bool haveLocalShardingInfo(const string& ns) {
        return false;
    }
}

namespace mongo_test {
    /**
     * Warning: Tests running this fixture cannot be run in parallel with other tests
     * that uses ConnectionString::setConnectionHook
     */
    class TaggedFiveMemberRS: public mongo::unittest::Test {
    protected:
        static const string IdentityNS;
        static const BSONField<string> HostField;

        void setUp() {
            _replSet.reset(new MockReplicaSet("test", 5));
            _originalConnectionHook = ConnectionString::getConnectionHook();
            ConnectionString::setConnectionHook(
                    mongo::MockConnRegistry::get()->getConnStrHook());

            {
                mongo::MockReplicaSet::ReplConfigMap config = _replSet->getReplConfig();

                {
                    const string host(_replSet->getPrimary());
                    map<string, string>& tag = config[host].tags;
                    tag.clear();
                    tag["dc"] = "ny";
                    tag["p"] = "1";
                    _replSet->getNode(host)->insert(IdentityNS, BSON(HostField(host)));
                }

                vector<string> secNodes = _replSet->getSecondaries();
                vector<string>::const_iterator secIter = secNodes.begin();

                {
                    const string host(*secIter);
                    map<string, string>&  tag = config[host].tags;
                    tag.clear();
                    tag["dc"] = "sf";
                    tag["s"] = "1";
                    tag["group"] = "1";
                    _replSet->getNode(host)->insert(IdentityNS, BSON(HostField(host)));
                }

                {
                    ++secIter;
                    const string host(*secIter);
                    map<string, string>&  tag = config[host].tags;
                    tag.clear();
                    tag["dc"] = "ma";
                    tag["s"] = "2";
                    tag["group"] = "1";
                    _replSet->getNode(host)->insert(IdentityNS, BSON(HostField(host)));
                }

                {
                    ++secIter;
                    const string host(*secIter);
                    map<string, string>&  tag = config[host].tags;
                    tag.clear();
                    tag["dc"] = "eu";
                    tag["s"] = "3";
                    _replSet->getNode(host)->insert(IdentityNS, BSON(HostField(host)));
                }

                {
                    ++secIter;
                    const string host(*secIter);
                    map<string, string>&  tag = config[host].tags;
                    tag.clear();
                    tag["dc"] = "jp";
                    tag["s"] = "4";
                    _replSet->getNode(host)->insert(IdentityNS, BSON(HostField(host)));
                }

                _replSet->setConfig(config);
            }
        }

        void tearDown() {
            ConnectionString::setConnectionHook(_originalConnectionHook);
            ReplicaSetMonitor::remove(_replSet->getSetName(), true);
            _replSet.reset();
            mongo::ScopedDbConnection::clearPool();
        }

        MockReplicaSet* getReplSet() {
            return _replSet.get();
        }

    private:
        ConnectionString::ConnectionHook* _originalConnectionHook;
        boost::scoped_ptr<MockReplicaSet> _replSet;
    };

    const string TaggedFiveMemberRS::IdentityNS("local.me");
    const BSONField<string> TaggedFiveMemberRS::HostField("host", "bad");

    TEST_F(TaggedFiveMemberRS, ConnShouldPinIfSameSettings) {
        MockReplicaSet* replSet = getReplSet();
        vector<HostAndPort> seedList;
        seedList.push_back(HostAndPort(replSet->getPrimary()));

        DBClientReplicaSet replConn(replSet->getSetName(), seedList);

        string dest;
        {
            Query query;
            query.readPref(mongo::ReadPreference_PrimaryPreferred, BSONArray());
            auto_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
            BSONObj doc = cursor->next();
            dest = doc[HostField.name()].str();
        }

        {
            Query query;
            query.readPref(mongo::ReadPreference_PrimaryPreferred, BSONArray());
            auto_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
            BSONObj doc = cursor->next();
            const string newDest = doc[HostField.name()].str();
            ASSERT_EQUALS(dest, newDest);
        }
    }

    TEST_F(TaggedFiveMemberRS, ConnShouldNotPinIfDiffMode) {
        MockReplicaSet* replSet = getReplSet();
        vector<HostAndPort> seedList;
        seedList.push_back(HostAndPort(replSet->getPrimary()));

        DBClientReplicaSet replConn(replSet->getSetName(), seedList);

        string dest;
        {
            Query query;
            query.readPref(mongo::ReadPreference_SecondaryPreferred, BSONArray());
            auto_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
            BSONObj doc = cursor->next();
            dest = doc[HostField.name()].str();
            ASSERT_NOT_EQUALS(dest, replSet->getPrimary());
        }

        {
            Query query;
            query.readPref(mongo::ReadPreference_SecondaryOnly, BSONArray());
            auto_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
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

        string dest;
        {
            Query query;
            query.readPref(mongo::ReadPreference_SecondaryPreferred,
                    BSON_ARRAY(BSON("dc" << "sf")));
            auto_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
            BSONObj doc = cursor->next();
            dest = doc[HostField.name()].str();
            ASSERT_NOT_EQUALS(dest, replSet->getPrimary());
        }

        {
            Query query;
            vector<pair<string, string> > tagSet;
            query.readPref(mongo::ReadPreference_SecondaryPreferred,
                    BSON_ARRAY(BSON("group" << 1)));
            auto_ptr<DBClientCursor> cursor = replConn.query(IdentityNS, query);
            BSONObj doc = cursor->next();
            const string newDest = doc[HostField.name()].str();
            ASSERT_NOT_EQUALS(dest, newDest);
        }
    }
}
