/**
 *    Copyright (C) 2017 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/op_msg.h"
#include "mongo/util/scopeguard.h"


namespace mongo {

TEST(OpMsg, UnknownRequiredFlagClosesConnection) {
    std::string errMsg;
    auto conn = std::unique_ptr<DBClientBase>(
        unittest::getFixtureConnectionString().connect("integration_test", errMsg));
    uassert(ErrorCodes::SocketException, errMsg, conn);

    auto request = OpMsgRequest::fromDBAndBody("admin", BSON("ping" << 1)).serialize();
    OpMsg::setFlag(&request, 1u << 15);  // This should be the last required flag to be assigned.

    Message reply;
    ASSERT(!conn->call(request, reply, /*assertOK*/ false));
}

TEST(OpMsg, UnknownOptionalFlagIsIgnored) {
    std::string errMsg;
    auto conn = std::unique_ptr<DBClientBase>(
        unittest::getFixtureConnectionString().connect("integration_test", errMsg));
    uassert(ErrorCodes::SocketException, errMsg, conn);

    auto request = OpMsgRequest::fromDBAndBody("admin", BSON("ping" << 1)).serialize();
    OpMsg::setFlag(&request, 1u << 31);  // This should be the last optional flag to be assigned.

    Message reply;
    ASSERT(conn->call(request, reply));
    uassertStatusOK(getStatusFromCommandResult(
        conn->parseCommandReplyMessage(conn->getServerAddress(), reply)->getCommandReply()));
}

TEST(OpMsg, FireAndForgetInsertWorks) {
    std::string errMsg;
    auto conn = std::unique_ptr<DBClientBase>(
        unittest::getFixtureConnectionString().connect("integration_test", errMsg));
    uassert(ErrorCodes::SocketException, errMsg, conn);

    conn->dropCollection("test.collection");

    conn->runFireAndForgetCommand(OpMsgRequest::fromDBAndBody("test", fromjson(R"({
        insert: "collection",
        writeConcern: {w: 0},
        documents: [
            {a: 1}
        ]
    })")));

    ASSERT_EQ(conn->count("test.collection"), 1u);
}

TEST(OpMsg, CloseConnectionOnFireAndForgetNotMasterError) {
    const auto connStr = unittest::getFixtureConnectionString();

    // This test only works against a replica set.
    if (connStr.type() != ConnectionString::SET) {
        return;
    }

    bool foundSecondary = false;
    for (auto host : connStr.getServers()) {
        DBClientConnection conn;
        uassertStatusOK(conn.connect(host, "integration_test"));
        bool isMaster;
        ASSERT(conn.isMaster(isMaster));
        if (isMaster)
            continue;
        foundSecondary = true;

        auto request = OpMsgRequest::fromDBAndBody("test", fromjson(R"({
            insert: "collection",
            writeConcern: {w: 0},
            documents: [
                {a: 1}
            ]
        })")).serialize();

        // Round-trip command fails with NotMaster error. Note that this failure is in command
        // dispatch which ignores w:0.
        Message reply;
        ASSERT(conn.call(request, reply, /*assertOK*/ true, nullptr));
        ASSERT_EQ(
            getStatusFromCommandResult(
                conn.parseCommandReplyMessage(conn.getServerAddress(), reply)->getCommandReply()),
            ErrorCodes::NotMaster);

        // Fire-and-forget closes connection when it sees that error. Note that this is using call()
        // rather than say() so that we get an error back when the connection is closed. Normally
        // using call() if kMoreToCome set results in blocking forever.
        OpMsg::setFlag(&request, OpMsg::kMoreToCome);
        ASSERT(!conn.call(request, reply, /*assertOK*/ false, nullptr));

        uassertStatusOK(conn.connect(host, "integration_test"));  // Reconnect.

        // Disable eager checking of master to simulate a stepdown occurring after the check. This
        // should respect w:0.
        BSONObj output;
        ASSERT(conn.runCommand("admin",
                               fromjson(R"({
                                   configureFailPoint: 'skipCheckingForNotMasterInCommandDispatch',
                                   mode: 'alwaysOn'
                               })"),
                               output))
            << output;
        ON_BLOCK_EXIT([&] {
            uassertStatusOK(conn.connect(host, "integration_test-cleanup"));
            ASSERT(conn.runCommand("admin",
                                   fromjson(R"({
                                          configureFailPoint:
                                              'skipCheckingForNotMasterInCommandDispatch',
                                          mode: 'off'
                                      })"),
                                   output))
                << output;
        });


        // Round-trip command claims to succeed due to w:0.
        OpMsg::replaceFlags(&request, 0);
        ASSERT(conn.call(request, reply, /*assertOK*/ true, nullptr));
        ASSERT_OK(getStatusFromCommandResult(
            conn.parseCommandReplyMessage(conn.getServerAddress(), reply)->getCommandReply()));

        // Fire-and-forget should still close connection.
        OpMsg::setFlag(&request, OpMsg::kMoreToCome);
        ASSERT(!conn.call(request, reply, /*assertOK*/ false, nullptr));

        break;
    }
    ASSERT(foundSecondary);
}

}  // namespace mongo
