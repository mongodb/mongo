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

#include "mongo/executor/network_test_env.h"

#include "mongo/base/status_with.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/cursor_response.h"

namespace mongo {

namespace executor {

NetworkTestEnv::NetworkTestEnv(TaskExecutor* executor, NetworkInterfaceMock* network)
    : _executor(executor), _mockNetwork(network) {}

void NetworkTestEnv::onCommand(OnCommandFunction func) {
    _mockNetwork->enterNetwork();

    const NetworkInterfaceMock::NetworkOperationIterator noi = _mockNetwork->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();

    auto resultStatus = func(request);

    if (resultStatus.isOK()) {
        BSONObjBuilder result(std::move(resultStatus.getValue()));
        CommandHelpers::appendCommandStatusNoThrow(result, resultStatus.getStatus());
        const RemoteCommandResponse response(result.obj(), BSONObj(), Milliseconds(1));

        _mockNetwork->scheduleResponse(noi, _mockNetwork->now(), response);
    } else {
        _mockNetwork->scheduleResponse(
            noi, _mockNetwork->now(), {resultStatus.getStatus(), Milliseconds(0)});
    }

    _mockNetwork->runReadyNetworkOperations();
    _mockNetwork->exitNetwork();
}

void NetworkTestEnv::onCommandWithMetadata(OnCommandWithMetadataFunction func) {
    _mockNetwork->enterNetwork();

    const NetworkInterfaceMock::NetworkOperationIterator noi = _mockNetwork->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();

    auto cmdResponseStatus = func(request);

    if (cmdResponseStatus.isOK()) {
        BSONObjBuilder result(std::move(cmdResponseStatus.data));
        CommandHelpers::appendCommandStatusNoThrow(result, cmdResponseStatus.status);
        const RemoteCommandResponse response(
            result.obj(), cmdResponseStatus.metadata, Milliseconds(1));

        _mockNetwork->scheduleResponse(noi, _mockNetwork->now(), response);
    } else {
        _mockNetwork->scheduleResponse(noi, _mockNetwork->now(), cmdResponseStatus.status);
    }

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

void NetworkTestEnv::onFindWithMetadataCommand(OnFindCommandWithMetadataFunction func) {
    onCommandWithMetadata([&func](const RemoteCommandRequest& request) -> RemoteCommandResponse {
        const auto& resultStatus = func(request);

        if (!resultStatus.isOK()) {
            return resultStatus.getStatus();
        }

        std::vector<BSONObj> result;
        BSONObj metadata;
        std::tie(result, metadata) = resultStatus.getValue();

        BSONArrayBuilder arr;
        for (const auto& obj : result) {
            arr.append(obj);
        }

        const NamespaceString nss =
            NamespaceString(request.dbname, request.cmdObj.firstElement().String());
        BSONObjBuilder resultBuilder;
        appendCursorResponseObject(0LL, nss.toString(), arr.arr(), &resultBuilder);

        return RemoteCommandResponse(resultBuilder.obj(), metadata, Milliseconds(1));
    });
}

}  // namespace executor
}  // namespace mongo
