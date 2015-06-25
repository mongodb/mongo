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

#include "mongo/base/status_with.h"
#include "mongo/client/remote_command_runner.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/cursor_responses.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/network_interface_mock.h"

namespace mongo {

namespace executor {

NetworkTestEnv::NetworkTestEnv(repl::ReplicationExecutor* executor, NetworkInterfaceMock* network)
    : _executor(executor), _mockNetwork(network) {}

void NetworkTestEnv::onCommand(OnCommandFunction func) {
    _mockNetwork->enterNetwork();

    const NetworkInterfaceMock::NetworkOperationIterator noi = _mockNetwork->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();

    const auto& resultStatus = func(request);

    BSONObjBuilder result;

    if (resultStatus.isOK()) {
        result.appendElements(resultStatus.getValue());
    }

    Command::appendCommandStatus(result, resultStatus.getStatus());

    const RemoteCommandResponse response(result.obj(), Milliseconds(1));

    _mockNetwork->scheduleResponse(noi, _mockNetwork->now(), response);
    _mockNetwork->runReadyNetworkOperations();
    _mockNetwork->exitNetwork();
}

void NetworkTestEnv::onFindCommand(OnFindCommandFunction func) {
    onCommand([&func](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        const auto& resultStatus = func(request);

        if (!resultStatus.isOK()) {
            return resultStatus.getStatus();
        }

        BSONArrayBuilder arr;
        for (const auto& obj : resultStatus.getValue()) {
            arr.append(obj);
        }

        const NamespaceString nss =
            NamespaceString(request.dbname, request.cmdObj.firstElement().String());
        BSONObjBuilder result;
        appendCursorResponseObject(0LL, nss.toString(), arr.arr(), &result);

        return result.obj();
    });
}

void NetworkTestEnv::startUp() {
    _executorThread = stdx::thread([this] { _executor->run(); });
}

void NetworkTestEnv::shutDown() {
    _executorThread.join();
}

}  // namespace executor
}  // namespace mongo
