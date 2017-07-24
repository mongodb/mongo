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

}  // namespace mongo
