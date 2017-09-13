/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/executor/task_executor_test_fixture.h"

#include "mongo/base/status.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace executor {

Status TaskExecutorTest::getDetectableErrorStatus() {
    return Status(ErrorCodes::InternalError, "Not mutated");
}

RemoteCommandRequest TaskExecutorTest::assertRemoteCommandNameEquals(
    StringData cmdName, const RemoteCommandRequest& request) {
    auto&& cmdObj = request.cmdObj;
    ASSERT_FALSE(cmdObj.isEmpty());
    if (cmdName != cmdObj.firstElementFieldName()) {
        std::string msg = str::stream()
            << "Expected command name \"" << cmdName << "\" in remote command request but found \""
            << cmdObj.firstElementFieldName() << "\" instead: " << request.toString();
        FAIL(msg);
    }
    return request;
}

TaskExecutorTest::~TaskExecutorTest() = default;

void TaskExecutorTest::setUp() {
    auto net = stdx::make_unique<NetworkInterfaceMock>();
    _net = net.get();
    _executor = makeTaskExecutor(std::move(net));
}

void TaskExecutorTest::launchExecutorThread() {
    _executor->startup();
    postExecutorThreadLaunch();
}

void TaskExecutorTest::shutdownExecutorThread() {
    _executor->shutdown();
}

void TaskExecutorTest::joinExecutorThread() {
    _net->exitNetwork();
    _executor->join();
}

void TaskExecutorTest::_doTest() {
    MONGO_UNREACHABLE;
}

void TaskExecutorTest::postExecutorThreadLaunch() {}

}  // namespace executor
}  // namespace mongo
