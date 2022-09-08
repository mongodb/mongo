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


#include "mongo/platform/basic.h"

#include <boost/optional.hpp>
#include <cstdint>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/stdx/thread.h"
#include "mongo/tools/bridge_commands.h"
#include "mongo/tools/mongobridge_options.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/service_entry_point_impl.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/future.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kBridge


namespace mongo {

namespace {

boost::optional<HostAndPort> extractHostInfo(const OpMsgRequest& request) {
    // The initial isMaster request made by mongod and mongos processes should contain a hostInfo
    // field that identifies the process by its host:port.
    StringData cmdName = request.getCommandName();
    if (cmdName != "isMaster" && cmdName != "ismaster") {
        return boost::none;
    }

    if (auto hostInfoElem = request.body["hostInfo"]) {
        if (hostInfoElem.type() == String) {
            return HostAndPort{hostInfoElem.valueStringData()};
        }
    }
    return boost::none;
}

}  // namespace

class BridgeContext {
public:
    Status runBridgeCommand(StringData cmdName, BSONObj cmdObj) {
        auto status = BridgeCommand::findCommand(cmdName);
        if (!status.isOK()) {
            return status.getStatus();
        }

        LOGV2(22916, "Processing bridge command", "cmdName"_attr = cmdName, "cmdObj"_attr = cmdObj);

        BridgeCommand* command = status.getValue();
        return command->run(cmdObj, &_settingsMutex, &_settings);
    }

    boost::optional<Status> maybeProcessBridgeCommand(boost::optional<OpMsgRequest> cmdRequest) {
        if (!cmdRequest) {
            return boost::none;
        }

        if (auto forBridge = cmdRequest->body["$forBridge"]) {
            if (forBridge.trueValue()) {
                return runBridgeCommand(cmdRequest->getCommandName(), cmdRequest->body);
            }
            return boost::none;
        }

        return boost::none;
    }

    HostSettings getHostSettings(boost::optional<HostAndPort> host) {
        if (host) {
            stdx::lock_guard<Latch> lk(_settingsMutex);
            return (_settings)[*host];
        }
        return {};
    }

    PseudoRandom makeSeededPRNG() {
        static StaticImmortal g = PseudoRandom{mongoBridgeGlobalParams.seed};
        return PseudoRandom{g->nextInt64()};
    }

    static BridgeContext* get();

private:
    static const ServiceContext::Decoration<BridgeContext> _get;

    Mutex _settingsMutex = MONGO_MAKE_LATCH("BridgeContext::_settingsMutex");
    HostSettingsMap _settings;
};

const ServiceContext::Decoration<BridgeContext> BridgeContext::_get =
    ServiceContext::declareDecoration<BridgeContext>();

BridgeContext* BridgeContext::get() {
    return &_get(getGlobalServiceContext());
}

class ServiceEntryPointBridge;
class ProxiedConnection {
public:
    ProxiedConnection() : _dest(nullptr), _prng(BridgeContext::get()->makeSeededPRNG()) {}

    transport::Session* operator->() {
        return _dest.get();
    }

    transport::SessionHandle& getSession() {
        return _dest;
    }

    void setSession(transport::SessionHandle session) {
        _dest = std::move(session);
    }

    const boost::optional<HostAndPort>& host() const {
        return _host;
    }

    std::string toString() const {
        if (_host) {
            return _host->toString();
        }
        return "<unknown>";
    }

    void extractHostInfo(OpMsgRequest request) {
        if (_seenFirstMessage)
            return;
        _seenFirstMessage = true;

        // The initial isMaster request made by mongod and mongos processes should contain a
        // hostInfo field that identifies the process by its host:port.
        StringData cmdName = request.getCommandName();
        if (cmdName != "isMaster" && cmdName != "ismaster") {
            return;
        }

        if (auto hostInfoElem = request.body["hostInfo"]) {
            if (hostInfoElem.type() == String) {
                _host = HostAndPort{hostInfoElem.valueStringData()};
            }
        }
    }

    double nextCanonicalDouble() {
        return _prng.nextCanonicalDouble();
    }

    void setInExhaust(bool inExhaust) {
        _inExhaust = inExhaust;
    }

    bool inExhaust() const {
        return _inExhaust;
    }

    // Handle response for request with kExhaustSupported flag or response from the exhaust stream.
    // This sets up the internal states for the ProxiedConnection and returns whether there is
    // "moreToCome" from the exhaust stream.
    bool handleExhaustResponse(Message& response) {
        // Only support OP_MSG exhaust cursors.
        invariant(response.operation() == dbMsg);
        uassert(4622300,
                "Response ID did not match the sent message ID.",
                !_lastExhaustRequestId ||
                    response.header().getResponseToMsgId() == _lastExhaustRequestId);
        if (response.operation() == dbCompressed) {
            MessageCompressorManager compressorManager;
            response = uassertStatusOK(compressorManager.decompressMessage(response));
        }
        _inExhaust = OpMsg::isFlagSet(response, OpMsg::kMoreToCome);
        if (_inExhaust) {
            _lastExhaustRequestId = response.header().getId();
        }
        return _inExhaust;
    }

    static ProxiedConnection& get(const transport::SessionHandle& session);

private:
    friend class ServiceEntryPointBridge;

    static const transport::Session::Decoration<ProxiedConnection> _get;
    transport::SessionHandle _dest;
    PseudoRandom _prng;
    boost::optional<HostAndPort> _host;
    bool _seenFirstMessage = false;
    bool _inExhaust = false;
    int _lastExhaustRequestId = 0;
};

const transport::Session::Decoration<ProxiedConnection> ProxiedConnection::_get =
    transport::Session::declareDecoration<ProxiedConnection>();

ProxiedConnection& ProxiedConnection::get(const transport::SessionHandle& session) {
    return _get(*session);
}

class ServiceEntryPointBridge final : public ServiceEntryPointImpl {
public:
    explicit ServiceEntryPointBridge(ServiceContext* svcCtx) : ServiceEntryPointImpl(svcCtx) {}

    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request) noexcept final;
};

Future<DbResponse> ServiceEntryPointBridge::handleRequest(OperationContext* opCtx,
                                                          const Message& request) noexcept try {
    if (request.operation() == dbQuery) {
        DbMessage d(request);
        QueryMessage q(d);
        if (q.queryOptions & QueryOption_Exhaust) {
            uasserted(51755, "Mongobridge does not support OP_QUERY exhaust");
        }
    }

    const auto& source = opCtx->getClient()->session();
    auto& dest = ProxiedConnection::get(source);
    auto brCtx = BridgeContext::get();

    // If the bridge decides to return something else other than a response from an active exhaust
    // stream, make sure we close the exhaust stream properly.
    ScopeGuard earlyExhaustExitGuard([&] {
        if (dest.inExhaust()) {
            LOGV2(4622301, "mongobridge shutting down exhaust stream", "remote"_attr = dest);
            dest.setInExhaust(false);
            // Active exhaust stream should have a session.
            invariant(dest.getSession());
            // Close the connection to the dest server to end the exhaust stream.
            dest->end();
            dest.setSession(nullptr);
        }
    });

    if (!dest.getSession()) {
        dest.setSession([]() -> transport::SessionHandle {
            HostAndPort destAddr{mongoBridgeGlobalParams.destUri};
            const Seconds kConnectTimeout(30);
            auto now = getGlobalServiceContext()->getFastClockSource()->now();
            const auto connectExpiration = now + kConnectTimeout;
            while (now < connectExpiration) {
                auto tl = getGlobalServiceContext()->getTransportLayer();
                auto sws =
                    tl->connect(destAddr, transport::kGlobalSSLMode, connectExpiration - now);
                auto status = sws.getStatus();
                if (!status.isOK()) {
                    LOGV2_WARNING(22924,
                                  "Unable to establish connection to {remoteAddress}: {error}",
                                  "Unable to establish connection",
                                  "remoteAddress"_attr = destAddr,
                                  "error"_attr = status);
                    now = getGlobalServiceContext()->getFastClockSource()->now();
                } else {
                    return std::move(sws.getValue());
                }

                sleepmillis(500);
            }

            return nullptr;
        }());

        if (!dest.getSession()) {
            source->end();
            uasserted(50861, str::stream() << "Unable to connect to " << source->remote());
        }
    }

    const bool isFireAndForgetCommand = OpMsg::isFlagSet(request, OpMsg::kMoreToCome);

    boost::optional<OpMsgRequest> cmdRequest;
    if ((request.operation() == dbQuery &&
         NamespaceString(DbMessage(request).getns()).isCommand()) ||
        request.operation() == dbMsg) {
        cmdRequest = rpc::opMsgRequestFromAnyProtocol(request, opCtx->getClient());

        dest.extractHostInfo(*cmdRequest);

        LOGV2_DEBUG(22917,
                    1,
                    "Received \"{commandName}\" command with arguments "
                    "{arguments} from {remote}",
                    "Received command",
                    "commandName"_attr = cmdRequest->getCommandName(),
                    "arguments"_attr = cmdRequest->body,
                    "remote"_attr = dest);
    }

    // Handle a message intended to configure the mongobridge and return a response.
    // The 'request' is consumed by the mongobridge and does not get forwarded to
    // 'dest'.
    if (auto status = brCtx->maybeProcessBridgeCommand(cmdRequest)) {
        invariant(!isFireAndForgetCommand);

        auto replyBuilder = rpc::makeReplyBuilder(rpc::protocolForMessage(request));
        BSONObj reply;
        StatusWith<BSONObj> commandReply(reply);
        if (!status->isOK()) {
            commandReply = StatusWith<BSONObj>(*status);
        }
        return Future<DbResponse>::makeReady(
            {replyBuilder->setCommandReply(std::move(commandReply)).done()});
    }


    // Get the message handling settings for 'host' if the source of the connection is
    // known. By default, messages are forwarded to 'dest' without any additional delay.
    HostSettings hostSettings = brCtx->getHostSettings(dest.host());

    switch (hostSettings.state) {
        // Close the connection to 'dest'.
        case HostSettings::State::kHangUp:
            LOGV2(22918,
                  "Rejecting connection from {remote}, end connection {source}",
                  "Rejecting connection",
                  "remote"_attr = dest,
                  "source"_attr = source->remote().toString());
            source->end();
            return Future<DbResponse>::makeReady({Message()});
        // Forward the message to 'dest' with probability '1 - hostSettings.loss'.
        case HostSettings::State::kDiscard:
            if (dest.nextCanonicalDouble() < hostSettings.loss) {
                std::string hostName = dest.toString();
                if (cmdRequest) {
                    LOGV2(22919,
                          "Discarding \"{commandName}\" command with arguments "
                          "{arguments} from {hostName}",
                          "Discarding command from host",
                          "commandName"_attr = cmdRequest->getCommandName(),
                          "arguments"_attr = cmdRequest->body,
                          "hostName"_attr = hostName);
                } else {
                    LOGV2(22920,
                          "Discarding {operation} from {hostName}",
                          "Discarding operation from host",
                          "operation"_attr = networkOpToString(request.operation()),
                          "hostName"_attr = hostName);
                }
                return Future<DbResponse>::makeReady({Message()});
            }
            // Forward the message to 'dest' after waiting for 'hostSettings.delay'
            // milliseconds.
            [[fallthrough]];
        case HostSettings::State::kForward:
            sleepmillis(durationCount<Milliseconds>(hostSettings.delay));
            break;
    }

    // If we get another type of request (e.g. exhaust cleanup killCursor request from the service
    // state machine), unset the exhaust mode.
    if (dest.inExhaust() &&
        (request.operation() != dbMsg || !OpMsg::isFlagSet(request, OpMsg::kExhaustSupported))) {
        dest.setInExhaust(false);
    }

    // Skip sending request in exhaust mode.
    if (!dest.inExhaust()) {
        uassertStatusOK(dest->sinkMessage(request));
    }

    // Send the message we received from 'source' to 'dest'. 'dest' returns a response for
    // OP_QUERY, OP_GET_MORE, and OP_MSG messages that we respond with.
    if (!isFireAndForgetCommand &&
        (request.operation() == dbQuery || request.operation() == dbGetMore ||
         request.operation() == dbMsg)) {
        auto response = uassertStatusOK(dest->sourceMessage());

        if (!dest.inExhaust()) {
            uassert(50765,
                    "Response ID did not match the sent message ID.",
                    response.header().getResponseToMsgId() == request.header().getId());
        }

        // Reload the message handling settings for 'host' in case they were changed
        // while waiting for a response from 'dest'.
        hostSettings = brCtx->getHostSettings(dest.host());

        // It's possible that sending 'request' blocked until 'dest' had something to
        // reply with. If the message handling settings were since changed to close
        // connections from 'host', then do so now.
        if (hostSettings.state == HostSettings::State::kHangUp) {
            LOGV2(22921,
                  "Closing connection from {remote}, end connection {source}",
                  "Closing connection",
                  "remote"_attr = dest,
                  "source"_attr = source->remote());
            source->end();
            return Future<DbResponse>::makeReady({Message()});
        }

        // Only support OP_MSG exhaust cursors.
        bool isExhaust = false;
        if (request.operation() == dbMsg && OpMsg::isFlagSet(request, OpMsg::kExhaustSupported)) {
            isExhaust = dest.handleExhaustResponse(response);
            earlyExhaustExitGuard.dismiss();
        }

        // The original checksum won't be valid once the network layer replaces requestId. Remove it
        // because the network layer re-checksums the response.
        OpMsg::removeChecksum(&response);

        // Return a DbResponse with shouldRunAgainForExhaust being set to isExhaust to indicate
        // whether this should be run again to receive more responses from the exhaust stream.
        // We do not need to set 'nextInvocation' in the DbResponse because mongobridge
        // only receives responses but ignores the next request if it is in exhaust mode.
        return Future<DbResponse>::makeReady({std::move(response), isExhaust});
    } else {
        return Future<DbResponse>::makeReady({Message()});
    }
} catch (const DBException& e) {
    LOGV2_ERROR(4879804, "Failed to handle request", "error"_attr = redact(e));
    return e.toStatus();
}

int bridgeMain(int argc, char** argv) {

    registerShutdownTask([&] {
        // NOTE: This function may be called at any time. It must not
        // depend on the prior execution of mongo initializers or the
        // existence of threads.
        if (hasGlobalServiceContext()) {
            auto sc = getGlobalServiceContext();
            if (sc->getTransportLayer())
                sc->getTransportLayer()->shutdown();

            if (sc->getServiceEntryPoint()) {
                sc->getServiceEntryPoint()->endAllSessions(transport::Session::kEmptyTagMask);
                sc->getServiceEntryPoint()->shutdown(Seconds{10});
            }
        }
    });

    setupSignalHandlers();
    runGlobalInitializersOrDie(std::vector<std::string>(argv, argv + argc));
    startSignalProcessingThread(LogFileStatus::kNoLogFileToRotate);

    auto serviceContextHolder = ServiceContext::make();
    serviceContextHolder->registerClientObserver(std::make_unique<LockerNoopClientObserver>());
    setGlobalServiceContext(std::move(serviceContextHolder));
    auto serviceContext = getGlobalServiceContext();

    serviceContext->setServiceEntryPoint(std::make_unique<ServiceEntryPointBridge>(serviceContext));

    {
        transport::TransportLayerASIO::Options opts;
        opts.ipList.emplace_back("0.0.0.0");
        opts.port = mongoBridgeGlobalParams.port;

        auto tl = std::make_unique<mongo::transport::TransportLayerASIO>(
            opts, serviceContext->getServiceEntryPoint());
        serviceContext->setTransportLayer(std::move(tl));
    }

    if (auto status = serviceContext->getServiceEntryPoint()->start(); !status.isOK()) {
        LOGV2(4907203, "Error starting service entry point", "error"_attr = status);
    }

    if (auto status = serviceContext->getTransportLayer()->setup(); !status.isOK()) {
        LOGV2(22922, "Error setting up transport layer", "error"_attr = status);
        return static_cast<int>(ExitCode::netError);
    }

    if (auto status = serviceContext->getTransportLayer()->start(); !status.isOK()) {
        LOGV2(22923, "Error starting transport layer", "error"_attr = status);
        return static_cast<int>(ExitCode::netError);
    }

    serviceContext->notifyStartupComplete();
    return static_cast<int>(waitForShutdown());
}

}  // namespace mongo

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables bridgeMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[]) {
    mongo::quickExit(mongo::bridgeMain(argc, mongo::WindowsCommandLine(argc, argvW).argv()));
}
#else
int main(int argc, char* argv[]) {
    mongo::quickExit(mongo::bridgeMain(argc, argv));
}
#endif
