/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/executor/network_interface_impl.h"

#include "mongo/platform/basic.h"

#include "mongo/db/wire_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/stdx/memory.h"

namespace mongo {
namespace executor {
namespace {

class NetworkInterfaceImplTest : public mongo::unittest::Test {
public:
    void setUp() override {
        _net = stdx::make_unique<NetworkInterfaceImpl>();
        _net->startup();
    }

    NetworkInterfaceImpl& net() {
        return *_net;
    }

private:
    std::unique_ptr<NetworkInterfaceImpl> _net;
};

TEST_F(NetworkInterfaceImplTest, InShutdown) {
    ASSERT_FALSE(net().inShutdown());
    net().shutdown();
    ASSERT(net().inShutdown());
}

TEST_F(NetworkInterfaceImplTest, StartCommandReturnsNotOKIfShutdownHasStarted) {
    TaskExecutor::CallbackHandle cb{};
    net().shutdown();
    ASSERT_NOT_OK(net().startCommand(
        cb, RemoteCommandRequest{}, [&](StatusWith<RemoteCommandResponse> resp) {}));
}

TEST_F(NetworkInterfaceImplTest, SetAlarmReturnsNotOKIfShutdownHasStarted) {
    net().shutdown();
    ASSERT_NOT_OK(net().setAlarm(net().now() + Milliseconds(100), []() {}));
}

}  // namespace
}  // namespace executor
}  // namespace mongo
