/*    Copyright 2012 10gen Inc.
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
 * This file includes integration testing between the MockDBClientBase and MockRemoteDB.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/jsobj.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/timer.h"

#include <ctime>
#include <string>
#include <vector>

using mongo::BSONObj;
using mongo::ConnectionString;
using mongo::MockDBClientConnection;
using mongo::MockRemoteDBServer;
using mongo::Query;

using std::string;
using std::vector;

namespace mongo_test {

TEST(MockDBClientConnTest, ServerAddress) {
    MockRemoteDBServer server("test");
    MockDBClientConnection conn(&server);

    ASSERT_EQUALS("test", conn.getServerAddress());
    ASSERT_EQUALS("test", conn.toString());
}

TEST(MockDBClientConnTest, QueryCount) {
    MockRemoteDBServer server("test");

    {
        MockDBClientConnection conn(&server);

        ASSERT_EQUALS(0U, server.getQueryCount());
        conn.query("foo.bar");
    }

    ASSERT_EQUALS(1U, server.getQueryCount());

    {
        MockDBClientConnection conn(&server);
        conn.query("foo.bar");
        ASSERT_EQUALS(2U, server.getQueryCount());
    }
}

TEST(MockDBClientConnTest, InsertAndQuery) {
    MockRemoteDBServer server("test");
    const string ns("test.user");

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns);
        ASSERT(!cursor->more());

        server.insert(ns, BSON("x" << 1));
        server.insert(ns, BSON("y" << 2));
    }

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns);

        ASSERT(cursor->more());
        BSONObj firstDoc = cursor->next();
        ASSERT_EQUALS(1, firstDoc["x"].numberInt());

        ASSERT(cursor->more());
        BSONObj secondDoc = cursor->next();
        ASSERT_EQUALS(2, secondDoc["y"].numberInt());

        ASSERT(!cursor->more());
    }

    // Make sure that repeated calls will still give you the same result
    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns);

        ASSERT(cursor->more());
        BSONObj firstDoc = cursor->next();
        ASSERT_EQUALS(1, firstDoc["x"].numberInt());

        ASSERT(cursor->more());
        BSONObj secondDoc = cursor->next();
        ASSERT_EQUALS(2, secondDoc["y"].numberInt());

        ASSERT(!cursor->more());
    }
}

TEST(MockDBClientConnTest, InsertAndQueryTwice) {
    MockRemoteDBServer server("test");
    const string ns("test.user");

    server.insert(ns, BSON("x" << 1));

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns);

        ASSERT(cursor->more());
        BSONObj firstDoc = cursor->next();
        ASSERT_EQUALS(1, firstDoc["x"].numberInt());
    }

    server.insert(ns, BSON("y" << 2));

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns);

        ASSERT(cursor->more());
        BSONObj firstDoc = cursor->next();
        ASSERT_EQUALS(1, firstDoc["x"].numberInt());

        ASSERT(cursor->more());
        BSONObj secondDoc = cursor->next();
        ASSERT_EQUALS(2, secondDoc["y"].numberInt());

        ASSERT(!cursor->more());
    }
}

TEST(MockDBClientConnTest, QueryWithNoResults) {
    MockRemoteDBServer server("test");
    const string ns("test.user");

    server.insert(ns, BSON("x" << 1));
    MockDBClientConnection conn(&server);
    std::unique_ptr<mongo::DBClientCursor> cursor = conn.query("other.ns");

    ASSERT(!cursor->more());
}

TEST(MockDBClientConnTest, MultiNSInsertAndQuery) {
    MockRemoteDBServer server("test");
    const string ns1("test.user");
    const string ns2("foo.bar");
    const string ns3("mongo.db");

    {
        MockDBClientConnection conn(&server);
        conn.insert(ns1, BSON("a" << 1));
        conn.insert(ns2,
                    BSON("ef"
                         << "gh"));
        conn.insert(ns3, BSON("x" << 2));

        conn.insert(ns1, BSON("b" << 3));
        conn.insert(ns2,
                    BSON("jk"
                         << "lm"));

        conn.insert(ns2,
                    BSON("x"
                         << "yz"));
    }

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns1);

        ASSERT(cursor->more());
        BSONObj firstDoc = cursor->next();
        ASSERT_EQUALS(1, firstDoc["a"].numberInt());

        ASSERT(cursor->more());
        BSONObj secondDoc = cursor->next();
        ASSERT_EQUALS(3, secondDoc["b"].numberInt());

        ASSERT(!cursor->more());
    }

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns2);

        ASSERT(cursor->more());
        BSONObj firstDoc = cursor->next();
        ASSERT_EQUALS("gh", firstDoc["ef"].String());

        ASSERT(cursor->more());
        BSONObj secondDoc = cursor->next();
        ASSERT_EQUALS("lm", secondDoc["jk"].String());

        ASSERT(cursor->more());
        BSONObj thirdDoc = cursor->next();
        ASSERT_EQUALS("yz", thirdDoc["x"].String());

        ASSERT(!cursor->more());
    }

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns3);

        ASSERT(cursor->more());
        BSONObj firstDoc = cursor->next();
        ASSERT_EQUALS(2, firstDoc["x"].numberInt());

        ASSERT(!cursor->more());
    }
}

TEST(MockDBClientConnTest, SimpleRemove) {
    MockRemoteDBServer server("test");
    const string ns("test.user");

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns);
        ASSERT(!cursor->more());

        conn.insert(ns, BSON("x" << 1));
        conn.insert(ns, BSON("y" << 1));
    }

    {
        MockDBClientConnection conn(&server);
        conn.remove(ns, Query(), false);
    }

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns);

        ASSERT(!cursor->more());
    }

    // Make sure that repeated calls will still give you the same result
    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns);

        ASSERT(!cursor->more());
    }
}

TEST(MockDBClientConnTest, MultiNSRemove) {
    MockRemoteDBServer server("test");
    const string ns1("test.user");
    const string ns2("foo.bar");
    const string ns3("mongo.db");

    {
        MockDBClientConnection conn(&server);
        conn.insert(ns1, BSON("a" << 1));
        conn.insert(ns2,
                    BSON("ef"
                         << "gh"));
        conn.insert(ns3, BSON("x" << 2));

        conn.insert(ns1, BSON("b" << 3));
        conn.insert(ns2,
                    BSON("jk"
                         << "lm"));

        conn.insert(ns2,
                    BSON("x"
                         << "yz"));
    }

    {
        MockDBClientConnection conn(&server);
        conn.remove(ns2, Query(), false);

        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns2);
        ASSERT(!cursor->more());
    }

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns1);

        ASSERT(cursor->more());
        BSONObj firstDoc = cursor->next();
        ASSERT_EQUALS(1, firstDoc["a"].numberInt());

        ASSERT(cursor->more());
        BSONObj secondDoc = cursor->next();
        ASSERT_EQUALS(3, secondDoc["b"].numberInt());

        ASSERT(!cursor->more());
    }

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns3);

        ASSERT(cursor->more());
        BSONObj firstDoc = cursor->next();
        ASSERT_EQUALS(2, firstDoc["x"].numberInt());

        ASSERT(!cursor->more());
    }
}

TEST(MockDBClientConnTest, InsertAfterRemove) {
    MockRemoteDBServer server("test");
    const string ns("test.user");

    {
        MockDBClientConnection conn(&server);
        conn.insert(ns, BSON("a" << 1));
        conn.insert(ns, BSON("b" << 3));
        conn.insert(ns,
                    BSON("x"
                         << "yz"));
    }

    {
        MockDBClientConnection conn(&server);
        conn.remove(ns, Query(), false);
    }

    {
        MockDBClientConnection conn(&server);
        conn.insert(ns, BSON("x" << 100));
    }

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.query(ns);

        ASSERT(cursor->more());
        BSONObj firstDoc = cursor->next();
        ASSERT_EQUALS(100, firstDoc["x"].numberInt());

        ASSERT(!cursor->more());
    }
}

TEST(MockDBClientConnTest, SetCmdReply) {
    MockRemoteDBServer server("test");
    server.setCommandReply("serverStatus",
                           BSON("ok" << 1 << "host"
                                     << "local"));

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand("foo.bar", BSON("serverStatus" << 1), response));
        ASSERT_EQUALS(1, response["ok"].numberInt());
        ASSERT_EQUALS("local", response["host"].str());

        ASSERT_EQUALS(1U, server.getCmdCount());
    }

    // Make sure that repeated calls will still give you the same result
    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand("foo.bar", BSON("serverStatus" << 1), response));
        ASSERT_EQUALS(1, response["ok"].numberInt());
        ASSERT_EQUALS("local", response["host"].str());

        ASSERT_EQUALS(2U, server.getCmdCount());
    }

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand("foo.bar", BSON("serverStatus" << 1), response));
        ASSERT_EQUALS(1, response["ok"].numberInt());
        ASSERT_EQUALS("local", response["host"].str());

        ASSERT_EQUALS(3U, server.getCmdCount());
    }
}

TEST(MockDBClientConnTest, CyclingCmd) {
    MockRemoteDBServer server("test");

    {
        vector<BSONObj> isMasterSequence;
        isMasterSequence.push_back(BSON("set"
                                        << "a"
                                        << "isMaster"
                                        << true
                                        << "ok"
                                        << 1));
        isMasterSequence.push_back(BSON("set"
                                        << "a"
                                        << "isMaster"
                                        << false
                                        << "ok"
                                        << 1));
        server.setCommandReply("isMaster", isMasterSequence);
    }

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand("foo.baz", BSON("isMaster" << 1), response));
        ASSERT_EQUALS(1, response["ok"].numberInt());
        ASSERT_EQUALS("a", response["set"].str());
        ASSERT(response["isMaster"].trueValue());

        ASSERT_EQUALS(1U, server.getCmdCount());
    }

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand("foo.baz", BSON("isMaster" << 1), response));
        ASSERT_EQUALS(1, response["ok"].numberInt());
        ASSERT_EQUALS("a", response["set"].str());
        ASSERT(!response["isMaster"].trueValue());

        ASSERT_EQUALS(2U, server.getCmdCount());
    }

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand("foo.baz", BSON("isMaster" << 1), response));
        ASSERT_EQUALS(1, response["ok"].numberInt());
        ASSERT_EQUALS("a", response["set"].str());
        ASSERT(response["isMaster"].trueValue());

        ASSERT_EQUALS(3U, server.getCmdCount());
    }
}

TEST(MockDBClientConnTest, CmdWithMultiFields) {
    MockRemoteDBServer server("test");
    server.setCommandReply("getLastError", BSON("ok" << 1 << "n" << 10));

    MockDBClientConnection conn(&server);
    BSONObj response;
    ASSERT(conn.runCommand(
        "foo.baz", BSON("getLastError" << 1 << "w" << 2 << "journal" << true), response));

    ASSERT_EQUALS(10, response["n"].numberInt());
}

TEST(MockDBClientConnTest, BadCmd) {
    MockRemoteDBServer server("test");
    server.setCommandReply("getLastError", BSON("ok" << 0));

    MockDBClientConnection conn(&server);
    BSONObj response;
    ASSERT(!conn.runCommand("foo.baz", BSON("getLastError" << 1), response));
}

TEST(MockDBClientConnTest, MultipleStoredResponse) {
    MockRemoteDBServer server("test");
    server.setCommandReply("getLastError", BSON("ok" << 1 << "n" << 10));
    server.setCommandReply("isMaster", BSON("ok" << 1 << "secondary" << false));

    MockDBClientConnection conn(&server);
    {
        BSONObj response;
        ASSERT(conn.runCommand("foo.baz",
                               BSON("isMaster"
                                    << "abc"),
                               response));
        ASSERT(!response["secondary"].trueValue());
    }

    {
        BSONObj response;
        ASSERT(conn.runCommand("a.b", BSON("getLastError" << 1), response));
        ASSERT_EQUALS(10, response["n"].numberInt());
    }
}

TEST(MockDBClientConnTest, CmdCount) {
    MockRemoteDBServer server("test");
    ASSERT_EQUALS(0U, server.getCmdCount());

    server.setCommandReply("serverStatus", BSON("ok" << 1));

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand("foo.bar", BSON("serverStatus" << 1), response));
        ASSERT_EQUALS(1U, server.getCmdCount());
    }

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand("baz.bar", BSON("serverStatus" << 1), response));
        ASSERT_EQUALS(2U, server.getCmdCount());
    }
}

TEST(MockDBClientConnTest, Shutdown) {
    MockRemoteDBServer server("test");
    server.setCommandReply("serverStatus", BSON("ok" << 1));
    ASSERT(server.isRunning());

    {
        MockDBClientConnection conn(&server);

        server.shutdown();
        ASSERT(!server.isRunning());

        ASSERT_THROWS(conn.query("test.user"), mongo::SocketException);
    }

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT_THROWS(conn.runCommand("test.user", BSON("serverStatus" << 1), response),
                      mongo::SocketException);
    }

    ASSERT_EQUALS(0U, server.getQueryCount());
    ASSERT_EQUALS(0U, server.getCmdCount());
}

TEST(MockDBClientConnTest, Restart) {
    MockRemoteDBServer server("test");
    server.setCommandReply("serverStatus", BSON("ok" << 1));

    MockDBClientConnection conn1(&server);

    // Do some queries and commands then check the counters later that
    // new instance still has it
    conn1.query("test.user");
    BSONObj response;
    conn1.runCommand("test.user", BSON("serverStatus" << 1), response);

    server.shutdown();
    ASSERT_THROWS(conn1.query("test.user"), mongo::SocketException);

    // New connections shouldn't work either
    MockDBClientConnection conn2(&server);
    ASSERT_THROWS(conn2.query("test.user"), mongo::SocketException);

    ASSERT_EQUALS(1U, server.getQueryCount());
    ASSERT_EQUALS(1U, server.getCmdCount());

    server.reboot();
    ASSERT(server.isRunning());

    {
        MockDBClientConnection conn(&server);
        conn.query("test.user");
    }

    // Old connections still shouldn't work
    ASSERT_THROWS(conn1.query("test.user"), mongo::SocketException);
    ASSERT_THROWS(conn2.query("test.user"), mongo::SocketException);

    ASSERT_EQUALS(2U, server.getQueryCount());
    ASSERT_EQUALS(1U, server.getCmdCount());
}

TEST(MockDBClientConnTest, ClearCounter) {
    MockRemoteDBServer server("test");
    server.setCommandReply("serverStatus", BSON("ok" << 1));

    MockDBClientConnection conn(&server);
    conn.query("test.user");
    BSONObj response;
    conn.runCommand("test.user", BSON("serverStatus" << 1), response);

    server.clearCounters();
    ASSERT_EQUALS(0U, server.getQueryCount());
    ASSERT_EQUALS(0U, server.getCmdCount());
}

TEST(MockDBClientConnTest, Delay) {
    MockRemoteDBServer server("test");
    server.setCommandReply("serverStatus", BSON("ok" << 1));
    server.setDelay(150);

    MockDBClientConnection conn(&server);

    {
        mongo::Timer timer;
        conn.query("x.x");
        const int nowInMilliSec = timer.millis();
        // Use a more lenient lower bound since some platforms like Windows
        // don't guarantee that sleeps will not wake up earlier (unlike
        // nanosleep we use for Linux)
        ASSERT_GREATER_THAN_OR_EQUALS(nowInMilliSec, 130);
    }

    {
        mongo::Timer timer;
        BSONObj response;
        conn.runCommand("x.x", BSON("serverStatus" << 1), response);
        const int nowInMilliSec = timer.millis();
        ASSERT_GREATER_THAN_OR_EQUALS(nowInMilliSec, 130);
    }

    ASSERT_EQUALS(1U, server.getQueryCount());
    ASSERT_EQUALS(1U, server.getCmdCount());
}
}
