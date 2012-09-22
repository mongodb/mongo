/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 * This file includes integration testing between the MockDBClientBase and MockRemoteDB.
 */

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/jsobj.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/timer.h"

#include <boost/scoped_ptr.hpp>
#include <ctime>
#include <string>
#include <vector>

using mongo::BSONObj;
using mongo::ConnectionString;

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

    TEST(MockDBClientConnTest, SetCmdReply) {
        MockRemoteDBServer server("test");
        server.setCommandReply("serverStatus", BSON("ok" << 1 << "host" << "local"));

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
            isMasterSequence.push_back(BSON("set" << "a"
                    << "isMaster" << true
                    << "ok" << 1));
            isMasterSequence.push_back(BSON("set" << "a"
                    << "isMaster" << false
                    << "ok" << 1));
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
        ASSERT(conn.runCommand("foo.baz", BSON("getLastError" << 1 << "w" << 2
                << "journal" << true), response));

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
            ASSERT(conn.runCommand("foo.baz", BSON("isMaster" << "abc"), response));
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
            ASSERT_THROWS(conn.runCommand("test.user",
                    BSON("serverStatus" << 1), response), mongo::SocketException);
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
            ASSERT_GREATER_THAN_OR_EQUALS(nowInMilliSec, 140);
            ASSERT_LESS_THAN_OR_EQUALS(nowInMilliSec, 160);
        }

        {
            mongo::Timer timer;
            BSONObj response;
            conn.runCommand("x.x", BSON("serverStatus" << 1), response);
            const int nowInMilliSec = timer.millis();
            ASSERT_GREATER_THAN_OR_EQUALS(nowInMilliSec, 140);
            ASSERT_LESS_THAN_OR_EQUALS(nowInMilliSec, 160);
        }

        ASSERT_EQUALS(1U, server.getQueryCount());
        ASSERT_EQUALS(1U, server.getCmdCount());
    }
}
