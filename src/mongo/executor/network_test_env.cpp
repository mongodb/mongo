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

#include "mongo/executor/network_test_env.h"

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_response.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {

namespace executor {

NetworkTestEnv::NetworkTestEnv(TaskExecutor* executor, NetworkInterfaceMock* network)
    : _executor(executor), _mockNetwork(network) {}

void NetworkTestEnv::onCommand(OnCommandFunction func) {
    onCommands({std::move(func)});
}

void NetworkTestEnv::onCommands(std::vector<OnCommandFunction> funcs) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_mockNetwork);

    for (auto&& func : funcs) {
        const NetworkInterfaceMock::NetworkOperationIterator noi =
            _mockNetwork->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();

        auto resultStatus = func(request);

        if (request.fireAndForget) {
            _mockNetwork->blackHole(noi);
        } else if (resultStatus.isOK()) {
            BSONObjBuilder result(std::move(resultStatus.getValue()));
            CommandHelpers::appendCommandStatusNoThrow(result, resultStatus.getStatus());
            const RemoteCommandResponse response =
                RemoteCommandResponse::make_forTest(result.obj(), Milliseconds(1));

            _mockNetwork->scheduleResponse(noi, _mockNetwork->now(), response);
        } else {
            _mockNetwork->scheduleResponse(
                noi,
                _mockNetwork->now(),
                RemoteCommandResponse::make_forTest(resultStatus.getStatus(), Milliseconds(0)));
        }
    }

    _mockNetwork->runReadyNetworkOperations();
}

void NetworkTestEnv::onCommandWithMetadata(OnCommandWithMetadataFunction func) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_mockNetwork);

    const NetworkInterfaceMock::NetworkOperationIterator noi = _mockNetwork->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();

    auto cmdResponseStatus = func(request);

    if (request.fireAndForget) {
        _mockNetwork->blackHole(noi);
    } else if (cmdResponseStatus.isOK()) {
        BSONObjBuilder result(std::move(cmdResponseStatus.data));
        CommandHelpers::appendCommandStatusNoThrow(result, cmdResponseStatus.status);
        const RemoteCommandResponse response =
            RemoteCommandResponse::make_forTest(result.obj(), Milliseconds(1));

        _mockNetwork->scheduleResponse(noi, _mockNetwork->now(), response);
    } else {
        _mockNetwork->scheduleResponse(
            noi,
            _mockNetwork->now(),
            RemoteCommandResponse::make_forTest(cmdResponseStatus.status));
    }

    _mockNetwork->runReadyNetworkOperations();
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

        const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
            request.dbname, request.cmdObj.firstElement().String());
        BSONObjBuilder result;
        appendCursorResponseObject(0LL, nss, arr.arr(), boost::none, &result);

        return result.obj();
    });
}

void NetworkTestEnv::onFindWithMetadataCommand(OnFindCommandWithMetadataFunction func) {
    onCommandWithMetadata([&func](const RemoteCommandRequest& request) -> RemoteCommandResponse {
        const auto& resultStatus = func(request);

        if (!resultStatus.isOK()) {
            return RemoteCommandResponse::make_forTest(resultStatus.getStatus());
        }

        std::vector<BSONObj> result;
        BSONObj metadata;
        std::tie(result, metadata) = resultStatus.getValue();

        BSONArrayBuilder arr;
        for (const auto& obj : result) {
            arr.append(obj);
        }

        const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
            request.dbname, request.cmdObj.firstElement().String());
        BSONObjBuilder resultBuilder(std::move(metadata));
        appendCursorResponseObject(0LL, nss, arr.arr(), boost::none, &resultBuilder);

        return RemoteCommandResponse::make_forTest(resultBuilder.obj(), Milliseconds(1));
    });
}

}  // namespace executor
}  // namespace mongo
