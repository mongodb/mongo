// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/network_test_env.h"

#include "mongo/base/status_with.h"
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
