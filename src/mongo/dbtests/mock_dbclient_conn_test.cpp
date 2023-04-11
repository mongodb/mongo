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
 * This file includes integration testing between the MockDBClientBase and MockRemoteDB.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/timer.h"

#include <ctime>
#include <string>
#include <vector>

using std::string;
using std::vector;

namespace mongo {

TEST(MockDBClientConnTest, ServerAddress) {
    MockRemoteDBServer server("test");
    MockDBClientConnection conn(&server);
    uassertStatusOK(conn.connect(server.getServerHostAndPort(), mongo::StringData(), boost::none));

    ASSERT_EQUALS("test:27017", conn.getServerAddress());
    ASSERT_EQUALS("test:27017", conn.toString());
    ASSERT_EQUALS(mongo::HostAndPort("test"), conn.getServerHostAndPort());
}

TEST(MockDBClientConnTest, QueryCount) {
    MockRemoteDBServer server("test");

    {
        MockDBClientConnection conn(&server);

        ASSERT_EQUALS(0U, server.getQueryCount());
        conn.find(FindCommandRequest(NamespaceString::createNamespaceString_forTest("foo.bar")));
    }

    ASSERT_EQUALS(1U, server.getQueryCount());

    {
        MockDBClientConnection conn(&server);
        conn.find(FindCommandRequest(NamespaceString::createNamespaceString_forTest("foo.bar")));
        ASSERT_EQUALS(2U, server.getQueryCount());
    }
}

TEST(MockDBClientConnTest, SkipBasedOnResumeAfter) {
    MockRemoteDBServer server{"test"};
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.user");

    {
        MockDBClientConnection conn{&server};
        server.insert(nss, BSON("x" << 1));
        server.insert(nss, BSON("y" << 2));
        server.insert(nss, BSON("z" << 3));
    }

    {
        MockDBClientConnection conn{&server};
        FindCommandRequest findRequest{FindCommandRequest{nss}};
        findRequest.setResumeAfter(BSON("n" << 2));

        auto cursor = conn.find(std::move(findRequest));
        ASSERT_EQ(1, cursor->itcount());
    }
}

TEST(MockDBClientConnTest, RequestResumeToken) {
    MockRemoteDBServer server{"test"};
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.user");

    {
        MockDBClientConnection conn{&server};
        server.insert(nss, BSON("_id" << 1));
        server.insert(nss, BSON("_id" << 2));
        server.insert(nss, BSON("_id" << 3));
    }

    {
        MockDBClientConnection conn{&server};
        FindCommandRequest findRequest{FindCommandRequest{nss}};
        findRequest.setRequestResumeToken(true);
        findRequest.setBatchSize(2);

        auto cursor = conn.find(std::move(findRequest));
        ASSERT(cursor->more());
        auto pbrt = cursor->getPostBatchResumeToken();
        ASSERT(pbrt);
        ASSERT_BSONOBJ_EQ(*pbrt, BSON("n" << 2));
    }
}

TEST(MockDBClientConnTest, InsertAndQuery) {
    MockRemoteDBServer server("test");
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.user");

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss));
        ASSERT(!cursor->more());

        server.insert(nss, BSON("x" << 1));
        server.insert(nss, BSON("y" << 2));
    }

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss));

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
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss));

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
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.user");

    server.insert(nss, BSON("x" << 1));

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss));

        ASSERT(cursor->more());
        BSONObj firstDoc = cursor->next();
        ASSERT_EQUALS(1, firstDoc["x"].numberInt());
    }

    server.insert(nss, BSON("y" << 2));

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss));

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
    const NamespaceString nss("test.user");

    server.insert(nss, BSON("x" << 1));
    MockDBClientConnection conn(&server);
    std::unique_ptr<mongo::DBClientCursor> cursor =
        conn.find(FindCommandRequest(NamespaceString::createNamespaceString_forTest("other.ns")));

    ASSERT(!cursor->more());
}

TEST(MockDBClientConnTest, MultiNSInsertAndQuery) {
    MockRemoteDBServer server("test");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("test.user");
    const NamespaceString nss2 = NamespaceString::createNamespaceString_forTest("foo.bar");
    const NamespaceString nss3 = NamespaceString::createNamespaceString_forTest("mongo.db");

    {
        MockDBClientConnection conn(&server);
        conn.insert(nss1, BSON("a" << 1));
        conn.insert(nss2,
                    BSON("ef"
                         << "gh"));
        conn.insert(nss3, BSON("x" << 2));

        conn.insert(nss1, BSON("b" << 3));
        conn.insert(nss2,
                    BSON("jk"
                         << "lm"));

        conn.insert(nss2,
                    BSON("x"
                         << "yz"));
    }

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss1));

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
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss2));

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
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss3));

        ASSERT(cursor->more());
        BSONObj firstDoc = cursor->next();
        ASSERT_EQUALS(2, firstDoc["x"].numberInt());

        ASSERT(!cursor->more());
    }
}

TEST(MockDBClientConnTest, SimpleRemove) {
    MockRemoteDBServer server("test");
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.user");

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss));
        ASSERT(!cursor->more());

        conn.insert(nss, BSON("x" << 1));
        conn.insert(nss, BSON("y" << 1));
    }

    {
        MockDBClientConnection conn(&server);
        conn.remove(nss, BSONObj{} /*filter*/);
    }

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss));

        ASSERT(!cursor->more());
    }

    // Make sure that repeated calls will still give you the same result
    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss));

        ASSERT(!cursor->more());
    }
}

TEST(MockDBClientConnTest, MultiNSRemove) {
    MockRemoteDBServer server("test");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("test.user");
    const NamespaceString nss2 = NamespaceString::createNamespaceString_forTest("foo.bar");
    const NamespaceString nss3 = NamespaceString::createNamespaceString_forTest("mongo.db");

    {
        MockDBClientConnection conn(&server);
        conn.insert(nss1, BSON("a" << 1));
        conn.insert(nss2,
                    BSON("ef"
                         << "gh"));
        conn.insert(nss3, BSON("x" << 2));

        conn.insert(nss1, BSON("b" << 3));
        conn.insert(nss2,
                    BSON("jk"
                         << "lm"));

        conn.insert(nss2,
                    BSON("x"
                         << "yz"));
    }

    {
        MockDBClientConnection conn(&server);
        conn.remove(nss2, BSONObj{} /*filter*/);

        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss2));
        ASSERT(!cursor->more());
    }

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss1));

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
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss3));

        ASSERT(cursor->more());
        BSONObj firstDoc = cursor->next();
        ASSERT_EQUALS(2, firstDoc["x"].numberInt());

        ASSERT(!cursor->more());
    }
}

TEST(MockDBClientConnTest, InsertAfterRemove) {
    MockRemoteDBServer server("test");
    const NamespaceString nss("test.user");

    {
        MockDBClientConnection conn(&server);
        conn.insert(nss, BSON("a" << 1));
        conn.insert(nss, BSON("b" << 3));
        conn.insert(nss,
                    BSON("x"
                         << "yz"));
    }

    {
        MockDBClientConnection conn(&server);
        conn.remove(nss, BSONObj{} /*filter*/);
    }

    {
        MockDBClientConnection conn(&server);
        conn.insert(nss, BSON("x" << 100));
    }

    {
        MockDBClientConnection conn(&server);
        std::unique_ptr<mongo::DBClientCursor> cursor = conn.find(FindCommandRequest(nss));

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
        ASSERT(conn.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "foo"),
                               BSON("serverStatus" << 1),
                               response));
        ASSERT_EQUALS(1, response["ok"].numberInt());
        ASSERT_EQUALS("local", response["host"].str());

        ASSERT_EQUALS(1U, server.getCmdCount());
    }

    // Make sure that repeated calls will still give you the same result
    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "foo"),
                               BSON("serverStatus" << 1),
                               response));
        ASSERT_EQUALS(1, response["ok"].numberInt());
        ASSERT_EQUALS("local", response["host"].str());

        ASSERT_EQUALS(2U, server.getCmdCount());
    }

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "foo"),
                               BSON("serverStatus" << 1),
                               response));
        ASSERT_EQUALS(1, response["ok"].numberInt());
        ASSERT_EQUALS("local", response["host"].str());

        ASSERT_EQUALS(3U, server.getCmdCount());
    }
}

TEST(MockDBClientConnTest, CyclingCmd) {
    MockRemoteDBServer server("test");

    {
        vector<mongo::StatusWith<BSONObj>> isMasterSequence;
        isMasterSequence.push_back(BSON("set"
                                        << "a"
                                        << "isMaster" << true << "ok" << 1));
        isMasterSequence.push_back(BSON("set"
                                        << "a"
                                        << "isMaster" << false << "ok" << 1));
        server.setCommandReply("isMaster", isMasterSequence);
    }

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "foo"),
                               BSON("isMaster" << 1),
                               response));
        ASSERT_EQUALS(1, response["ok"].numberInt());
        ASSERT_EQUALS("a", response["set"].str());
        ASSERT(response["isMaster"].trueValue());

        ASSERT_EQUALS(1U, server.getCmdCount());
    }

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "foo"),
                               BSON("isMaster" << 1),
                               response));
        ASSERT_EQUALS(1, response["ok"].numberInt());
        ASSERT_EQUALS("a", response["set"].str());
        ASSERT(!response["isMaster"].trueValue());

        ASSERT_EQUALS(2U, server.getCmdCount());
    }

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "foo"),
                               BSON("isMaster" << 1),
                               response));
        ASSERT_EQUALS(1, response["ok"].numberInt());
        ASSERT_EQUALS("a", response["set"].str());
        ASSERT(response["isMaster"].trueValue());

        ASSERT_EQUALS(3U, server.getCmdCount());
    }
}

TEST(MockDBClientConnTest, MultipleStoredResponse) {
    MockRemoteDBServer server("test");
    server.setCommandReply("serverStatus", BSON("ok" << 0));
    server.setCommandReply("isMaster", BSON("ok" << 1 << "secondary" << false));

    MockDBClientConnection conn(&server);
    {
        BSONObj response;
        ASSERT(conn.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "foo"),
                               BSON("isMaster"
                                    << "abc"),
                               response));
        ASSERT(!response["secondary"].trueValue());
    }

    {
        BSONObj response;
        ASSERT(!conn.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "a"),
                                BSON("serverStatus" << 1),
                                response));
    }
}

TEST(MockDBClientConnTest, CmdCount) {
    MockRemoteDBServer server("test");
    ASSERT_EQUALS(0U, server.getCmdCount());

    server.setCommandReply("serverStatus", BSON("ok" << 1));

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "foo"),
                               BSON("serverStatus" << 1),
                               response));
        ASSERT_EQUALS(1U, server.getCmdCount());
    }

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT(conn.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "baz"),
                               BSON("serverStatus" << 1),
                               response));
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

        ASSERT_THROWS(conn.find(FindCommandRequest(
                          NamespaceString::createNamespaceString_forTest("test.user"))),
                      mongo::NetworkException);
    }

    {
        MockDBClientConnection conn(&server);
        BSONObj response;
        ASSERT_THROWS(conn.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                      BSON("serverStatus" << 1),
                                      response),
                      mongo::NetworkException);
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
    conn1.find(FindCommandRequest(NamespaceString::createNamespaceString_forTest("test.user")));
    BSONObj response;
    conn1.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                     BSON("serverStatus" << 1),
                     response);

    server.shutdown();
    ASSERT_THROWS(
        conn1.find(FindCommandRequest(NamespaceString::createNamespaceString_forTest("test.user"))),
        mongo::NetworkException);

    // New connections shouldn't work either
    MockDBClientConnection conn2(&server);
    ASSERT_THROWS(
        conn2.find(FindCommandRequest(NamespaceString::createNamespaceString_forTest("test.user"))),
        mongo::NetworkException);

    ASSERT_EQUALS(1U, server.getQueryCount());
    ASSERT_EQUALS(1U, server.getCmdCount());

    server.reboot();
    ASSERT(server.isRunning());

    {
        MockDBClientConnection conn(&server);
        conn.find(FindCommandRequest(NamespaceString::createNamespaceString_forTest("test.user")));
    }

    // Old connections still shouldn't work
    ASSERT_THROWS(
        conn1.find(FindCommandRequest(NamespaceString::createNamespaceString_forTest("test.user"))),
        mongo::NetworkException);
    ASSERT_THROWS(
        conn2.find(FindCommandRequest(NamespaceString::createNamespaceString_forTest("test.user"))),
        mongo::NetworkException);

    ASSERT_EQUALS(2U, server.getQueryCount());
    ASSERT_EQUALS(1U, server.getCmdCount());
}

TEST(MockDBClientConnTest, ClearCounter) {
    MockRemoteDBServer server("test");
    server.setCommandReply("serverStatus", BSON("ok" << 1));

    MockDBClientConnection conn(&server);
    conn.find(FindCommandRequest(
        FindCommandRequest(NamespaceString::createNamespaceString_forTest("test.user"))));
    BSONObj response;
    conn.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                    BSON("serverStatus" << 1),
                    response);

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
        conn.find(FindCommandRequest(NamespaceString::createNamespaceString_forTest("x.x")));
        const int nowInMilliSec = timer.millis();
        // Use a more lenient lower bound since some platforms like Windows
        // don't guarantee that sleeps will not wake up earlier (unlike
        // nanosleep we use for Linux)
        ASSERT_GREATER_THAN_OR_EQUALS(nowInMilliSec, 130);
    }

    {
        mongo::Timer timer;
        BSONObj response;
        conn.runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "x"),
                        BSON("serverStatus" << 1),
                        response);
        const int nowInMilliSec = timer.millis();
        ASSERT_GREATER_THAN_OR_EQUALS(nowInMilliSec, 130);
    }

    ASSERT_EQUALS(1U, server.getQueryCount());
    ASSERT_EQUALS(1U, server.getCmdCount());
}

const auto docObj = [](int i) {
    return BSON("_id" << i);
};
const auto metadata = [](int i) {
    return BSON("$fakeMetaData" << i);
};
const long long cursorId = 123;
const bool moreToCome = true;
const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");

TEST(MockDBClientConnTest, SimulateCallAndRecvResponses) {
    MockRemoteDBServer server("test");
    MockDBClientConnection conn(&server);

    FindCommandRequest findCmd{nss};
    mongo::DBClientCursor cursor(&conn, findCmd, ReadPreferenceSetting{}, true /*isExhaust*/);
    cursor.setBatchSize(2);

    // Two batches from the initial find and getMore command.
    MockDBClientConnection::Responses callResponses = {
        MockDBClientConnection::mockFindResponse(
            nss, cursorId, {docObj(1), docObj(2)}, metadata(1)),
        MockDBClientConnection::mockGetMoreResponse(
            nss, cursorId, {docObj(3), docObj(4)}, metadata(2), moreToCome)};
    conn.setCallResponses(callResponses);

    // Two more batches from the exhaust stream.
    MockDBClientConnection::Responses recvResponses = {
        MockDBClientConnection::mockGetMoreResponse(
            nss, cursorId, {docObj(5), docObj(6)}, metadata(3), moreToCome),
        // Terminal getMore responses with cursorId 0 and no kMoreToCome flag.
        MockDBClientConnection::mockGetMoreResponse(nss, 0, {docObj(7), docObj(8)}, metadata(4))};
    conn.setRecvResponses(recvResponses);

    int numMetaRead = 0;
    conn.setReplyMetadataReader(
        [&](mongo::OperationContext* opCtx, const BSONObj& metadataObj, mongo::StringData target) {
            numMetaRead++;
            // Verify metadata for each batch.
            ASSERT(metadataObj.hasField("$fakeMetaData"));
            ASSERT_EQ(numMetaRead, metadataObj["$fakeMetaData"].number());
            return mongo::Status::OK();
        });

    // First batch from the initial find command.
    ASSERT_TRUE(cursor.init());
    ASSERT_BSONOBJ_EQ(docObj(1), cursor.next());
    ASSERT_BSONOBJ_EQ(docObj(2), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());

    // Second batch from the first getMore command.
    ASSERT_TRUE(cursor.more());
    ASSERT_BSONOBJ_EQ(docObj(3), cursor.next());
    ASSERT_BSONOBJ_EQ(docObj(4), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());

    // Third batch from the exhaust stream.
    ASSERT_TRUE(cursor.more());
    ASSERT_BSONOBJ_EQ(docObj(5), cursor.next());
    ASSERT_BSONOBJ_EQ(docObj(6), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());

    // Last batch from the exhaust stream.
    ASSERT_TRUE(cursor.more());
    ASSERT_BSONOBJ_EQ(docObj(7), cursor.next());
    ASSERT_BSONOBJ_EQ(docObj(8), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());

    // No more batches.
    ASSERT_FALSE(cursor.more());
    ASSERT_TRUE(cursor.isDead());

    // Test that metadata reader is called four times for the four batches.
    ASSERT_EQ(4, numMetaRead);
}

TEST(MockDBClientConnTest, SimulateCallErrors) {
    MockRemoteDBServer server("test");
    MockDBClientConnection conn(&server);

    mongo::DBClientCursor cursor(&conn, FindCommandRequest{nss}, ReadPreferenceSetting{}, false);

    // Test network exception and error response for the initial find.
    MockDBClientConnection::Responses callResponses = {
        // Network exception during call().
        mongo::Status{mongo::ErrorCodes::NetworkTimeout, "Fake socket timeout"},
        // Error response from the find command.
        MockDBClientConnection::mockErrorResponse(mongo::ErrorCodes::Interrupted)};
    conn.setCallResponses(callResponses);

    // Throw call exception.
    ASSERT_THROWS_CODE_AND_WHAT(cursor.init(),
                                mongo::DBException,
                                mongo::ErrorCodes::NetworkTimeout,
                                "Fake socket timeout");
    ASSERT_TRUE(cursor.isDead());

    // Throw exception on non-OK response.
    ASSERT_THROWS_CODE(cursor.init(), mongo::DBException, mongo::ErrorCodes::Interrupted);
    ASSERT_TRUE(cursor.isDead());
}

void runUntilExhaustRecv(MockDBClientConnection* conn, mongo::DBClientCursor* cursor) {
    cursor->setBatchSize(1);

    // Two batches from the initial find and getMore command.
    MockDBClientConnection::Responses callResponses = {
        MockDBClientConnection::mockFindResponse(nss, cursorId, {docObj(1)}, metadata(1)),
        MockDBClientConnection::mockGetMoreResponse(
            nss, cursorId, {docObj(2)}, metadata(2), moreToCome)};
    conn->setCallResponses(callResponses);

    // First batch from the initial find command.
    ASSERT_TRUE(cursor->init());
    ASSERT_BSONOBJ_EQ(docObj(1), cursor->next());
    ASSERT_FALSE(cursor->moreInCurrentBatch());

    // Second batch from the first getMore command.
    ASSERT_TRUE(cursor->more());
    ASSERT_BSONOBJ_EQ(docObj(2), cursor->next());
    ASSERT_FALSE(cursor->moreInCurrentBatch());
}

TEST(MockDBClientConnTest, SimulateRecvErrors) {
    MockRemoteDBServer server("test");
    MockDBClientConnection conn(&server);

    mongo::DBClientCursor cursor(
        &conn, FindCommandRequest{nss}, ReadPreferenceSetting{}, true /*isExhaust*/);

    runUntilExhaustRecv(&conn, &cursor);

    // Test network exception and error response from exhaust stream.
    MockDBClientConnection::Responses recvResponses = {
        // Network exception during recv().
        mongo::Status{mongo::ErrorCodes::NetworkTimeout, "Fake socket timeout"},
        // Error response from the exhaust cursor.
        MockDBClientConnection::mockErrorResponse(mongo::ErrorCodes::Interrupted)};
    conn.setRecvResponses(recvResponses);

    // The first recv() call gets a network exception.
    ASSERT_THROWS_CODE_AND_WHAT(cursor.more(),
                                mongo::DBException,
                                mongo::ErrorCodes::NetworkTimeout,
                                "Fake socket timeout");
    // Cursor is still valid on network exceptions.
    ASSERT_FALSE(cursor.isDead());

    // Throw exception on non-OK response.
    ASSERT_THROWS_CODE(cursor.more(), mongo::DBException, mongo::ErrorCodes::Interrupted);
    // Cursor is dead on command errors.
    ASSERT_TRUE(cursor.isDead());
}

bool blockedOnNetworkSoon(MockDBClientConnection* conn) {
    // Wait up to 10 seconds.
    for (auto i = 0; i < 100; i++) {
        if (conn->isBlockedOnNetwork()) {
            return true;
        }
        mongo::sleepmillis(100);
    }
    return false;
}

TEST(MockDBClientConnTest, BlockingNetwork) {
    MockRemoteDBServer server("test");
    MockDBClientConnection conn(&server);

    mongo::DBClientCursor cursor(
        &conn, FindCommandRequest{nss}, ReadPreferenceSetting{}, true /*isExhaust*/);
    cursor.setBatchSize(1);

    mongo::stdx::thread cursorThread([&] {
        // First batch from the initial find command.
        ASSERT_TRUE(cursor.init());
        ASSERT_BSONOBJ_EQ(docObj(1), cursor.next());
        ASSERT_FALSE(cursor.moreInCurrentBatch());

        // Second batch from the first getMore command.
        ASSERT_TRUE(cursor.more());
        ASSERT_BSONOBJ_EQ(docObj(2), cursor.next());
        ASSERT_FALSE(cursor.moreInCurrentBatch());

        // Last batch from the exhaust stream.
        ASSERT_TRUE(cursor.more());
        ASSERT_BSONOBJ_EQ(docObj(3), cursor.next());
        ASSERT_FALSE(cursor.moreInCurrentBatch());
        ASSERT_TRUE(cursor.isDead());
    });

    // Cursor should be blocked on the first find command.
    ASSERT_TRUE(blockedOnNetworkSoon(&conn));
    auto m = conn.getLastSentMessage();
    auto msg = mongo::OpMsg::parse(m);
    ASSERT_EQ(mongo::StringData(msg.body.firstElement().fieldName()), "find");
    // Set the response for the find command and unblock network call().
    conn.setCallResponses(
        {MockDBClientConnection::mockFindResponse(nss, cursorId, {docObj(1)}, metadata(1))});

    // Cursor should be blocked on the getMore command.
    ASSERT_TRUE(blockedOnNetworkSoon(&conn));
    m = conn.getLastSentMessage();
    msg = mongo::OpMsg::parse(m);
    ASSERT_EQ(mongo::StringData(msg.body.firstElement().fieldName()), "getMore");
    // Set the response for the getMore command and unblock network call().
    conn.setCallResponses({MockDBClientConnection::mockGetMoreResponse(
        nss, cursorId, {docObj(2)}, metadata(2), moreToCome)});

    // Cursor should be blocked on the exhaust stream.
    ASSERT_TRUE(blockedOnNetworkSoon(&conn));
    // Set the response for the exhaust stream and unblock network recv().
    conn.setRecvResponses(
        {MockDBClientConnection::mockGetMoreResponse(nss, 0, {docObj(3)}, metadata(3))});

    cursorThread.join();
}

TEST(MockDBClientConnTest, ShutdownServerBeforeCall) {
    MockRemoteDBServer server("test");
    MockDBClientConnection conn(&server);

    ASSERT_OK(
        conn.connect(mongo::HostAndPort("localhost", 12345), mongo::StringData(), boost::none));
    mongo::DBClientCursor cursor(
        &conn, FindCommandRequest{nss}, ReadPreferenceSetting{}, true /*isExhaust*/);

    // Shut down server before call.
    server.shutdown();

    // Connection is broken before call.
    ASSERT_THROWS_CODE(cursor.init(), mongo::DBException, mongo::ErrorCodes::SocketException);

    // Reboot the mock server but the cursor.init() would still fail because the connection does not
    // support autoreconnect.
    server.reboot();
    ASSERT_THROWS_CODE(cursor.init(), mongo::DBException, mongo::ErrorCodes::SocketException);
}

TEST(MockDBClientConnTest, ShutdownServerAfterCall) {
    MockRemoteDBServer server("test");
    MockDBClientConnection conn(&server);

    mongo::DBClientCursor cursor(
        &conn, FindCommandRequest{nss}, ReadPreferenceSetting{}, true /*isExhaust*/);

    mongo::stdx::thread cursorThread([&] {
        ASSERT_THROWS_CODE(cursor.init(), mongo::DBException, mongo::ErrorCodes::HostUnreachable);
    });

    // Cursor should be blocked on the first find command.
    ASSERT_TRUE(blockedOnNetworkSoon(&conn));

    // Shutting down the server doesn't interrupt the blocking network call, so we shut down the
    // connection as well to simulate shutdown of the server.
    server.shutdown();
    conn.shutdown();

    cursorThread.join();
}

TEST(MockDBClientConnTest, ConnectionAutoReconnect) {
    const bool autoReconnect = true;
    MockRemoteDBServer server("test");
    MockDBClientConnection conn(&server, autoReconnect);

    ASSERT_OK(
        conn.connect(mongo::HostAndPort("localhost", 12345), mongo::StringData(), boost::none));
    mongo::DBClientCursor cursor(
        &conn, FindCommandRequest{nss}, ReadPreferenceSetting{}, true /*isExhaust*/);

    server.shutdown();

    // Connection is broken before call.
    ASSERT_THROWS_CODE(cursor.init(), mongo::DBException, mongo::ErrorCodes::SocketException);

    // AutoReconnect fails because the server is down.
    ASSERT_THROWS_CODE(cursor.init(), mongo::DBException, mongo::ErrorCodes::HostUnreachable);

    // Reboot the mock server and the cursor.init() will reconnect and succeed.
    server.reboot();

    conn.setCallResponses(
        {MockDBClientConnection::mockFindResponse(nss, cursorId, {docObj(1)}, metadata(1))});
    ASSERT_TRUE(cursor.init());
    ASSERT_BSONOBJ_EQ(docObj(1), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());
}

TEST(MockDBClientConnTest, ShutdownServerBeforeRecv) {
    const bool autoReconnect = true;
    MockRemoteDBServer server("test");
    MockDBClientConnection conn(&server, autoReconnect);

    mongo::DBClientCursor cursor(
        &conn, FindCommandRequest{nss}, ReadPreferenceSetting{}, true /*isExhaust*/);

    runUntilExhaustRecv(&conn, &cursor);

    server.shutdown();

    // cursor.more() will call recv on the exhaust stream.
    ASSERT_THROWS_CODE(cursor.more(), mongo::DBException, mongo::ErrorCodes::SocketException);

    server.reboot();
    // Reconnect is not possible for exhaust recv.
    ASSERT_THROWS_CODE(cursor.more(), mongo::DBException, mongo::ErrorCodes::SocketException);
}

TEST(MockDBClientConnTest, ShutdownServerAfterRecv) {
    MockRemoteDBServer server("test");
    MockDBClientConnection conn(&server);

    mongo::DBClientCursor cursor(
        &conn, FindCommandRequest{nss}, ReadPreferenceSetting{}, true /*isExhaust*/);

    runUntilExhaustRecv(&conn, &cursor);

    mongo::stdx::thread cursorThread([&] {
        ASSERT_THROWS_CODE(cursor.more(), mongo::DBException, mongo::ErrorCodes::HostUnreachable);
    });

    // Cursor should be blocked on the recv.
    ASSERT_TRUE(blockedOnNetworkSoon(&conn));

    // Shutting down the server doesn't interrupt the blocking network call, so we shut down the
    // connection as well to simulate shutdown of the server.
    server.shutdown();
    conn.shutdown();

    cursorThread.join();
}
}  // namespace mongo
