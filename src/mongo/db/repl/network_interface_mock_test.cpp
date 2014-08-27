/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <map>
#include <boost/thread/thread.hpp>

#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/map_util.h"

namespace mongo {
namespace repl {

namespace {

    typedef StatusWith<ReplicationExecutor::NetworkInterface::Response> ResponseStatus;
    static int counter = 0;

    ResponseStatus responseLookup(const ReplicationExecutor::RemoteCommandRequest& request) {
        if (request.target.host() != "localhost") {
            return StatusWith<NetworkInterfaceMock::Response>(ErrorCodes::HostUnreachable,
                                                              "network unavailable");
        }

        return ResponseStatus(NetworkInterfaceMock::Response(BSON("ok"<< 1.0 << "c" << ++counter),
                                                             Milliseconds(2)));
    }

    TEST(NetworkInterfaceMock, MissingMappedRequest) {
        NetworkInterfaceMockWithMap* net = new NetworkInterfaceMockWithMap;
        const ReplicationExecutor::RemoteCommandRequest cmdReq(
                HostAndPort("localhost", 27017),
                "mydb",
                BSON("whatsUp" << "doc"));
        ResponseStatus resp = net->runCommand(cmdReq);
        ASSERT_NOT_OK(resp.getStatus());
        ASSERT_EQUALS(resp.getStatus().code(), ErrorCodes::NoSuchKey);
    }

    TEST(NetworkInterfaceMock, MappedRequest) {
        NetworkInterfaceMockWithMap* net = new NetworkInterfaceMockWithMap;
        const ReplicationExecutor::RemoteCommandRequest cmdReq(
                HostAndPort("localhost", 27017),
                "mydb",
                BSON("whatsUp" << "doc"));

        const StatusWith<BSONObj> expectedResp(BSON("ok" << 1.0 << "a" << 0));
        net->addResponse(cmdReq, expectedResp, false);
        ResponseStatus resp = net->runCommand(cmdReq);

        ASSERT_OK(resp.getStatus());
        ASSERT_EQUALS(resp.getValue().data, expectedResp.getValue());
    }


    TEST(NetworkInterfaceMock, Helper) {
        NetworkInterfaceMock* net = new NetworkInterfaceMock(responseLookup);
        const ReplicationExecutor::RemoteCommandRequest cmdReq(
                HostAndPort("localhost", 27017),
                "mydb",
                BSON("whatsUp" << "doc"));
        ResponseStatus resp = net->runCommand(cmdReq);
        ASSERT_OK(resp.getStatus());
        ASSERT_EQUALS(resp.getValue().data["c"].Int(), 1);
        ASSERT_EQUALS(resp.getValue().elapsedMillis.total_milliseconds(), 2);
        ResponseStatus resp2 = net->runCommand(cmdReq);
        ASSERT_OK(resp2.getStatus());
        ASSERT_EQUALS(resp2.getValue().data["c"].Int(), 2);
        ASSERT_EQUALS(resp2.getValue().elapsedMillis.total_milliseconds(), 2);
    }

    TEST(NetworkInterfaceMock, HelperWithError) {
        NetworkInterfaceMock* net = new NetworkInterfaceMock(responseLookup);
        const ReplicationExecutor::RemoteCommandRequest cmdReq(
                HostAndPort("removeHost", 27017),
                "mydb",
                BSON("whatsUp" << "doc"));
        ResponseStatus resp = net->runCommand(cmdReq);
        ASSERT_NOT_OK(resp.getStatus());
    }
}  // namespace
}  // namespace repl
}  // namespace mongo
