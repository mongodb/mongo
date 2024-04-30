/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/transport/grpc/service.h"

#include <charconv>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <numeric>

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/rpc_method.h>
#include <grpcpp/impl/rpc_service_method.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/method_handler.h>
#include <grpcpp/support/status_code_enum.h>
#include <grpcpp/support/sync_stream.h>

#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/transport/grpc/grpc_server_context.h"
#include "mongo/transport/grpc/grpc_server_stream.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/util.h"
#include "mongo/util/base64.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {

namespace {

inline Status makeShutdownTerminationStatus() {
    return Status(ErrorCodes::ShutdownInProgress, "gRPC server is shutting down");
}

::grpc::Status parseWireVersion(const MetadataView& clientMetadata, int& wireVersionOut) {
    auto clientWireVersionEntry = clientMetadata.find(util::constants::kWireVersionKey);
    if (clientWireVersionEntry == clientMetadata.end()) {
        return ::grpc::Status(
            ::grpc::StatusCode::FAILED_PRECONDITION,
            fmt::format("Clients must specify the server wire version they are targeting in "
                        "the \"{}\" metadata entry",
                        util::constants::kWireVersionKey));
    }
    if (auto parseResult = std::from_chars(clientWireVersionEntry->second.begin(),
                                           clientWireVersionEntry->second.end(),
                                           wireVersionOut);
        parseResult.ec != std::errc{}) {
        return ::grpc::Status(
            ::grpc::StatusCode::INVALID_ARGUMENT,
            fmt::format("Invalid wire version: \"{}\"", clientWireVersionEntry->second));
    }

    return ::grpc::Status::OK;
}

::grpc::Status verifyClientWireVersion(const MetadataView& clientMetadata,
                                       int clusterMaxWireVersion) {
    int clientWireVersion;
    if (auto parseResult = parseWireVersion(clientMetadata, clientWireVersion); !parseResult.ok()) {
        return parseResult;
    }
    if (clientWireVersion > clusterMaxWireVersion) {
        return ::grpc::Status(
            ::grpc::StatusCode::FAILED_PRECONDITION,
            fmt::format("Provided wire version ({}) exceeds cluster's max wire version ({})",
                        clientWireVersion,
                        clusterMaxWireVersion));
    } else if (auto serverMinWireVersion = WireSpec::getWireSpec(getGlobalServiceContext())
                                               .get()
                                               ->incomingExternalClient.minWireVersion;
               clientWireVersion < serverMinWireVersion) {
        return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION,
                              fmt::format("Provided wire version ({}) is less than this server's "
                                          "minimum accepted wire version ({})",
                                          clientWireVersion,
                                          serverMinWireVersion));
    }

    return ::grpc::Status::OK;
}

::grpc::Status verifyReservedMetadata(const MetadataView& clientMetadata) {
    static const StringDataSet kRecognizedClientMetadataKeys{
        util::constants::kAuthenticationTokenKey,
        util::constants::kClientIdKey,
        util::constants::kClientMetadataKey,
        util::constants::kWireVersionKey};
    static constexpr StringData kReservedMetadataKeyPrefix = "mongodb"_sd;

    for (const auto& entry : clientMetadata) {
        const auto& key = entry.first;
        if (key.startsWith(kReservedMetadataKeyPrefix) &&
            !kRecognizedClientMetadataKeys.contains(key)) {
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                  fmt::format("Unrecognized reserved metadata key: \"{}\"", key));
        }
    }
    return ::grpc::Status::OK;
}

::grpc::Status extractClientId(const MetadataView& clientMetadata,
                               boost::optional<UUID>& clientId) {
    if (auto clientIdEntry = clientMetadata.find(util::constants::kClientIdKey);
        clientIdEntry != clientMetadata.end()) {
        auto clientIdStatus = UUID::parse(clientIdEntry->second);
        if (!clientIdStatus.isOK()) {
            return ::grpc::Status(
                ::grpc::StatusCode::INVALID_ARGUMENT,
                fmt::format("The provided client ID (\"{}\") is not a valid UUID: {}",
                            clientIdEntry->second,
                            clientIdStatus.getStatus().toString()));
        }
        clientId = std::move(clientIdStatus.getValue());
    }
    return ::grpc::Status::OK;
}

/**
 * Logs the metadata document provided by the client, if any.
 * If a clientId was specified by the remote, log at INFO level. Otherwise, log at DEBUG level.
 * If the document is not valid BSON, log at WARNING level.
 */
void logClientMetadataDocument(const IngressSession& session) {
    if (serverGlobalParams.quiet.load()) {
        return;
    }
    try {
        if (auto metadata = session.getClientMetadata()) {
            if (session.getRemoteClientId()) {
                LOGV2_INFO(7401301,
                           "Received client metadata for gRPC stream",
                           "remote"_attr = session.remote(),
                           "remoteClientId"_attr = session.remoteClientIdToString(),
                           "streamId"_attr = session.id(),
                           "doc"_attr = metadata->getDocument());
            } else {
                LOGV2_DEBUG(7401302,
                            2,
                            "Received client metadata for gRPC stream",
                            "remote"_attr = session.remote(),
                            "remoteClientId"_attr = session.remoteClientIdToString(),
                            "streamId"_attr = session.id(),
                            "doc"_attr = metadata->getDocument());
            }
        }
    } catch (const DBException& e) {
        LOGV2_WARNING(7401303,
                      "Received invalid client metadata for gRPC stream",
                      "remote"_attr = session.remote(),
                      "remoteClientId"_attr = session.remoteClientIdToString(),
                      "streamId"_attr = session.id(),
                      "error"_attr = e);
    }
}

template <typename HandlerType>
auto makeRpcServiceMethod(CommandService* service, const char* name, HandlerType handler) {
    return new ::grpc::internal::RpcServiceMethod(
        name,
        ::grpc::internal::RpcMethod::BIDI_STREAMING,
        new ::grpc::internal::BidiStreamingHandler<CommandService, SharedBuffer, ConstSharedBuffer>(
            [handler = std::move(handler)](
                CommandService* service,
                ::grpc::ServerContext* nativeServerCtx,
                ::grpc::ServerReaderWriter<ConstSharedBuffer, SharedBuffer>* nativeServerStream) {
                try {
                    GRPCServerContext ctx{nativeServerCtx};
                    GRPCServerStream stream{nativeServerStream};
                    return handler(service, ctx, stream);
                } catch (const DBException& e) {
                    return util::convertStatus(e.toStatus());
                }
            },
            service));
}

}  // namespace

CommandService::CommandService(TransportLayer* tl,
                               RPCHandler callback,
                               std::shared_ptr<WireVersionProvider> wvProvider,
                               std::shared_ptr<ClientCache> clientCache)
    : _tl{tl},
      _callback{std::move(callback)},
      _wvProvider{std::move(wvProvider)},
      _clientCache{std::move(clientCache)} {

    if (!_clientCache) {
        _clientCache = std::make_shared<ClientCache>();
    }

    AddMethod(makeRpcServiceMethod(
        this,
        util::constants::kUnauthenticatedCommandStreamMethodName,
        [](CommandService* service, GRPCServerContext& ctx, GRPCServerStream& stream) {
            return service->_handleStream(ctx, stream, boost::none);
        }));

    AddMethod(makeRpcServiceMethod(
        this,
        util::constants::kAuthenticatedCommandStreamMethodName,
        [](CommandService* service, GRPCServerContext& ctx, GRPCServerStream& stream) {
            return service->_handleAuthenticatedStream(ctx, stream);
        }));
}

::grpc::Status CommandService::_handleStream(ServerContext& serverCtx,
                                             ServerStream& stream,
                                             boost::optional<std::string> authToken) {
    auto clusterMaxWireVersion = _wvProvider->getClusterMaxWireVersion();
    serverCtx.addInitialMetadataEntry(util::constants::kClusterMaxWireVersionKey,
                                      std::to_string(_wvProvider->getClusterMaxWireVersion()));

    auto clientMetadata = serverCtx.getClientMetadata();
    if (auto result = verifyClientWireVersion(clientMetadata, clusterMaxWireVersion);
        !result.ok()) {
        return result;
    }
    if (auto result = verifyReservedMetadata(clientMetadata); !result.ok()) {
        return result;
    }
    boost::optional<UUID> clientId;
    if (auto result = extractClientId(clientMetadata, clientId); !result.ok()) {
        return result;
    }

    boost::optional<StringData> base64EncodedClientMetadata;
    if (auto clientMetadataEntry = clientMetadata.find(util::constants::kClientMetadataKey);
        clientMetadataEntry != clientMetadata.end()) {
        base64EncodedClientMetadata = clientMetadataEntry->second;
    }

    auto session = std::make_shared<IngressSession>(
        _tl, &serverCtx, &stream, clientId, authToken, base64EncodedClientMetadata);
    std::list<InSessionPtr>::iterator it;
    {
        stdx::lock_guard lk{_mutex};

        if (_shutdown || !_acceptNewRequests) {
            session->setTerminationStatus(makeShutdownTerminationStatus());
            return util::convertStatus(*session->terminationStatus());
        }
        it = _sessions.insert(_sessions.begin(), session);
    }
    ON_BLOCK_EXIT([&]() {
        stdx::lock_guard lk{_mutex};
        _sessions.erase(it);
        if (_sessions.empty()) {
            _shutdownCV.notify_one();
        }
    });

    if (!clientId || _clientCache->add(*clientId) == ClientCache::AddResult::kCreated) {
        logClientMetadataDocument(*session);
    }

    _callback(session);
    auto status = session->terminationStatus();
    invariant(status.has_value());
    return util::convertStatus(std::move(*status));
}

::grpc::Status CommandService::_handleAuthenticatedStream(ServerContext& serverCtx,
                                                          ServerStream& stream) {
    auto meta = serverCtx.getClientMetadata();
    auto authTokenEntry = meta.find(util::constants::kAuthenticationTokenKey);
    if (MONGO_unlikely(authTokenEntry == meta.end())) {
        return ::grpc::Status{
            ::grpc::StatusCode::UNAUTHENTICATED,
            fmt::format("{} RPCs must contain an authentication token in the \"{}\" metadata entry",
                        util::constants::kAuthenticatedCommandStreamMethodName,
                        util::constants::kAuthenticationTokenKey)};
    }
    return _handleStream(serverCtx, stream, authTokenEntry->second.toString());
}

void CommandService::shutdown() {
    stdx::unique_lock lk{_mutex};

    invariant(!_shutdown,
              fmt::format("Cannot shut down {} gRPC service once it's stopped", name()));
    _shutdown = true;

    auto nSessionsTerminated = _sessions.size();
    for (auto& session : _sessions) {
        session->cancel(makeShutdownTerminationStatus());
    }

    _shutdownCV.wait(lk, [&]() { return _sessions.empty(); });

    LOGV2_DEBUG(7401308,
                1,
                "CommandService shutdown complete",
                "terminatedSessionsCount"_attr = nSessionsTerminated);
}

void CommandService::stopAcceptingRequests() {
    stdx::unique_lock lk{_mutex};
    _acceptNewRequests = false;
}

}  // namespace mongo::transport::grpc
