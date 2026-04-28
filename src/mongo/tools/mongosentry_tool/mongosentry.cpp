/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

/**
 * Test-only binary that listens on a port and invariants if any MongoDB wire protocol message
 * is received. Used to detect accidental traffic to retired server ports after a standby
 * cluster transition.
 */

#include "mongo/base/initializer.h"
#include "mongo/base/parse_number.h"
#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/asio/asio_session_manager.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/future.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"  // IWYU pragma: keep
#include "mongo/util/time_support.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

/**
 * A ServiceEntryPoint that invariants upon receiving any message. This ensures that no traffic
 * reaches a retired server port after a standby cluster transition.
 */
class ServiceEntryPointSentry final : public ServiceEntryPoint {
public:
    explicit ServiceEntryPointSentry(int port) : _port(port) {}

    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request,
                                     Date_t started) final {
        auto* client = opCtx->getClient();
        auto remote = (client && client->hasRemote()) ? client->getRemote().toString()
                                                      : std::string("unknown");
        std::string commandName = "unknown";
        bool isFromServer = false;
        try {
            auto opMsgRequest = rpc::opMsgRequestFromAnyProtocol(request, client);
            commandName = std::string(opMsgRequest.getCommandName());
            isFromServer = !opMsgRequest.body["internalClient"].eoo();
        } catch (...) {
        }

        // We allow requests from non-server processes (e.g. the mongo shell) because
        // ReplicaSetMonitor::drop() does not explicitly close pooled connections to monitored
        // hosts. After _forgetReplSet removes the monitor, the connection pool may still attempt
        // to refresh idle connections to old hosts, sending a hello to the sentry.
        if (!isFromServer) {
            LOGV2_WARNING(12319004,
                          "mongosentry: ignoring non-server command on retired port",
                          "command"_attr = commandName,
                          "remote"_attr = remote,
                          "port"_attr = _port);
            return Future<DbResponse>::makeReady(
                Status{ErrorCodes::IllegalOperation,
                       "mongosentry: this port is retired and should not receive traffic"});
        }

        invariant(false,
                  str::stream() << "mongosentry: received server command '" << commandName
                                << "' from " << remote << " on retired port " << _port
                                << "; this port should not be receiving any server traffic "
                                   "after the standby cluster transition");
        MONGO_UNREACHABLE;
    }

private:
    int _port;
};

int mongosentry_main(int argc, char** argv) {
    // Extract --port from argv before passing remaining args to the global initializers, since
    // --port is not registered as a global option in mongosentry. This is done because this binary
    // only has the port as an option and so linking the server global params or adding a separate
    // idl was unneccesary.
    int port = -1;
    std::vector<std::string> initArgs{argv[0]};

    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--port" && i + 1 < argc) {
            ++i;
            int parsedPort = 0;
            auto status = NumberParser{}.base(10)(argv[i], &parsedPort);
            invariant(
                status.isOK(),
                "mongosentry: --port must be followed by a valid integer with no extra characters");
            port = parsedPort;
        } else {
            initArgs.emplace_back(argv[i]);
        }
    }

    invariant(port > 0, "mongosentry: --port <port> is required");

    registerShutdownTask([&] {
        if (hasGlobalServiceContext()) {
            if (auto* tl = getGlobalServiceContext()->getTransportLayerManager()) {
                tl->endAllSessions(Client::kEmptyTagMask);
                tl->shutdown();
            }
        }
    });

    setupSignalHandlers();
    runGlobalInitializersOrDie(initArgs);
    startSignalProcessingThread(LogFileStatus::kNoLogFileToRotate);

    auto serviceContextHolder = ServiceContext::make();
    setGlobalServiceContext(std::move(serviceContextHolder));
    auto serviceContext = getGlobalServiceContext();

    serviceContext->getService()->setServiceEntryPoint(
        std::make_unique<ServiceEntryPointSentry>(port));

    {
        transport::AsioTransportLayer::Options opts;
        opts.ipList.emplace_back("0.0.0.0");
        opts.port = port;

        auto sm = std::make_unique<transport::AsioSessionManager>(serviceContext);
        auto tl = std::make_unique<transport::AsioTransportLayer>(opts, std::move(sm));

        serviceContext->setTransportLayerManager(
            std::make_unique<transport::TransportLayerManagerImpl>(std::move(tl)));
    }

    transport::ServiceExecutor::startupAll(serviceContext);

    if (auto status = serviceContext->getTransportLayerManager()->setup(); !status.isOK()) {
        LOGV2(12319001, "mongosentry: error setting up transport layer", "error"_attr = status);
        return static_cast<int>(ExitCode::netError);
    }

    if (auto status = serviceContext->getTransportLayerManager()->start(); !status.isOK()) {
        LOGV2(12319002, "mongosentry: error starting transport layer", "error"_attr = status);
        return static_cast<int>(ExitCode::netError);
    }

    serviceContext->notifyStorageStartupRecoveryComplete();
    return static_cast<int>(waitForShutdown());
}

}  // namespace mongo

#if defined(_WIN32)
int wmain(int argc, wchar_t* argvW[]) {
    mongo::quickExit(mongo::mongosentry_main(argc, mongo::WindowsCommandLine(argc, argvW).argv()));
}
#else
int main(int argc, char* argv[]) {
    mongo::quickExit(mongo::mongosentry_main(argc, argv));
}
#endif
