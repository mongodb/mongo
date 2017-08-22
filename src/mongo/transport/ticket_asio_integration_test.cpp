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

#include "mongo/client/connection_string.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbmessage.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/socket_exception.h"

namespace mongo {
namespace executor {
namespace {

TEST(TicketASIOTest, WireProtocolInvalidLen) {

    const auto sendValid = [](auto conn) {
        auto msg = makeKillCursorsMessage(0);
        conn->say(msg);
    };

    const auto sendInvalid = [](auto conn) {
        auto msg = makeKillCursorsMessage(0);
        MSGHEADER::Layout* header = (MSGHEADER::Layout*)msg.sharedBuffer().get();
        header->messageLength = 0x7FFFFFFF;
        conn->say(msg);
        // Prior call throws SocketException because the server disconnects
    };

    // Do this twice to ensure that the remote didn't abort due to invariant
    for (int i = 0; i < 2; ++i) {
        std::string errMsg;
        auto conn = std::shared_ptr<DBClientBase>(
            unittest::getFixtureConnectionString().connect("integration_test", errMsg));
        uassert(ErrorCodes::SocketException, errMsg, conn);
        sendValid(conn);
        sendValid(conn);
        ASSERT_THROWS(sendInvalid(conn), SocketException);
    }
}

}  // namespace
}  // namespace executor
}  // namespace mongo
