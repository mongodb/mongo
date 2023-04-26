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
#include <functional>
#include <numeric>

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/rpc_method.h>
#include <grpcpp/impl/rpc_service_method.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/method_handler.h>
#include <grpcpp/support/sync_stream.h>

#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/transport/grpc/grpc_server_context.h"
#include "mongo/transport/grpc/grpc_server_stream.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo::transport::grpc {

namespace {

const Status kShutdownTerminationStatus{ErrorCodes::ShutdownInProgress,
                                        "gRPC server is shutting down"};

::grpc::Status parseWireVersion(const MetadataView& clientMetadata, int& wireVersionOut) {
    auto clientWireVersionEntry = clientMetadata.find(CommandService::kWireVersionKey);
    if (clientWireVersionEntry == clientMetadata.end()) {
        return ::grpc::Status(
            ::grpc::StatusCode::FAILED_PRECONDITION,
            "Clients must specify the server wire version they are targeting in the \"{}\" metadata entry"_format(
                CommandService::kWireVersionKey));
    }
    if (auto parseResult = std::from_chars(clientWireVersionEntry->second.begin(),
                                           clientWireVersionEntry->second.end(),
                                           wireVersionOut);
        parseResult.ec != std::errc{}) {
        return ::grpc::Status(
            ::grpc::StatusCode::INVALID_ARGUMENT,
            "Invalid wire version: \"{}\""_format(clientWireVersionEntry->second));
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
            "Provided wire version ({}) exceeds cluster's max wire version ({})"_format(
                clientWireVersion, clusterMaxWireVersion));
    } else if (auto serverMinWireVersion =
                   WireSpec::instance().get()->incomingExternalClient.minWireVersion;
               clientWireVersion < serverMinWireVersion) {
        return ::grpc::Status(
            ::grpc::StatusCode::FAILED_PRECONDITION,
            "Provided wire version ({}) is less than this server's minimum accepted wire version ({})"_format(
                clientWireVersion, serverMinWireVersion));
    }

    return ::grpc::Status::OK;
}

::grpc::Status verifyReservedMetadata(const MetadataView& clientMetadata) {
    static const StringDataSet kRecognizedClientMetadataKeys{
        CommandService::kAuthenticationTokenKey,
        CommandService::kClientIdKey,
        CommandService::kClientMetadataKey,
        CommandService::kWireVersionKey};
    static constexpr StringData kReservedMetadataKeyPrefix = "mongodb"_sd;

    for (const auto& entry : clientMetadata) {
        const auto& key = entry.first;
        if (key.startsWith(kReservedMetadataKeyPrefix) &&
            !kRecognizedClientMetadataKeys.contains(key)) {
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                  "Unrecognized reserved metadata key: \"{}\""_format(key));
        }
    }
    return ::grpc::Status::OK;
}

::grpc::Status extractClientId(const MetadataView& clientMetadata,
                               boost::optional<UUID>& clientId) {
    if (auto clientIdEntry = clientMetadata.find(CommandService::kClientIdKey);
        clientIdEntry != clientMetadata.end()) {
        auto clientIdStatus = UUID::parse(clientIdEntry->second);
        if (!clientIdStatus.isOK()) {
            return ::grpc::Status(
                ::grpc::StatusCode::INVALID_ARGUMENT,
                "The provided client ID (\"{}\") is not a valid UUID: {}"_format(
                    clientIdEntry->second, clientIdStatus.getStatus().toString()));
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
void logClientMetadataDocument(const MetadataView& clientMetadata, const IngressSession& session) {
    auto clientMetadataEntry = clientMetadata.find(CommandService::kClientMetadataKey);
    if (clientMetadataEntry == clientMetadata.end()) {
        return;
    }

    try {
        fmt::memory_buffer buffer{};
        base64::decode(buffer, clientMetadataEntry->second);
        BSONObj metadataDocument{buffer.data()};
        auto metadata = ClientMetadata{metadataDocument};

        if (session.clientId()) {
            LOGV2_INFO(7401301,
                       "Received client metadata for gRPC stream",
                       "remote"_attr = session.remote(),
                       "remoteClientId"_attr = session.clientIdStr(),
                       "streamId"_attr = session.id(),
                       "doc"_attr = metadataDocument);
        } else {
            LOGV2_DEBUG(7401302,
                        2,
                        "Received client metadata for gRPC stream",
                        "remote"_attr = session.remote(),
                        "remoteClientId"_attr = session.clientIdStr(),
                        "streamId"_attr = session.id(),
                        "doc"_attr = metadataDocument);
        }
    } catch (const DBException& e) {
        LOGV2_WARNING(7401303,
                      "Received invalid client metadata for gRPC stream",
                      "remote"_attr = session.remote(),
                      "remoteClientId"_attr = session.clientIdStr(),
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
                GRPCServerContext ctx{nativeServerCtx};
                GRPCServerStream stream{nativeServerStream};
                return handler(service, ctx, stream);
            },
            service));
}

}  // namespace

const std::string CommandService::kClusterMaxWireVersionKey = "mongodb-maxwireversion";

CommandService::CommandService(GRPCTransportLayer* tl,
                               RpcHandler callback,
                               std::shared_ptr<WireVersionProvider> wvProvider)
    : _tl{tl},
      _callback{std::move(callback)},
      _wvProvider{std::move(wvProvider)},
      _clientCache{std::make_unique<ClientCache>()} {

    AddMethod(makeRpcServiceMethod(
        this,
        kUnauthenticatedCommandStreamMethodName,
        [](CommandService* service, GRPCServerContext ctx, GRPCServerStream stream) {
            return service->_handleStream(ctx, stream);
        }));

    AddMethod(makeRpcServiceMethod(
        this,
        kAuthenticatedCommandStreamMethodName,
        [](CommandService* service, GRPCServerContext ctx, GRPCServerStream stream) {
            return service->_handleAuthenticatedStream(ctx, stream);
        }));
}

::grpc::Status CommandService::_handleStream(ServerContext& serverCtx, ServerStream& stream) {
    auto clusterMaxWireVersion = _wvProvider->getClusterMaxWireVersion();
    serverCtx.addInitialMetadataEntry(kClusterMaxWireVersionKey,
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

    auto session = std::make_shared<IngressSession>(_tl, &serverCtx, &stream, clientId);
    std::list<InSessionPtr>::iterator it;
    {
        stdx::lock_guard lk{_mutex};

        if (_shutdown) {
            session->terminate(kShutdownTerminationStatus);
            return ::grpc::Status{::grpc::StatusCode::UNAVAILABLE,
                                  kShutdownTerminationStatus.reason()};
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
        logClientMetadataDocument(clientMetadata, *session);
    }

    auto status = _callback(session);
    invariant(!session->isConnected());
    return status;
}

::grpc::Status CommandService::_handleAuthenticatedStream(ServerContext& serverCtx,
                                                          ServerStream& stream) {
    if (auto meta = serverCtx.getClientMetadata();
        meta.find(kAuthenticationTokenKey) == meta.end()) {
        return ::grpc::Status{
            ::grpc::StatusCode::UNAUTHENTICATED,
            "{} RPCs must contain an authentication token in the \"{}\" metadata entry"_format(
                kAuthenticatedCommandStreamMethodName, kAuthenticationTokenKey)};
    }

    return _handleStream(serverCtx, stream);
}

void CommandService::shutdown() {
    stdx::unique_lock lk{_mutex};

    invariant(!_shutdown, "Cannot shut down {} gRPC service once it's stopped"_format(name()));
    _shutdown = true;

    auto nSessionsTerminated = _sessions.size();
    for (auto& session : _sessions) {
        session->terminate(kShutdownTerminationStatus);
    }

    _shutdownCV.wait(lk, [&]() { return _sessions.empty(); });

    LOGV2_DEBUG(7401308,
                1,
                "MongoDB gRPC service shutdown complete",
                "terminatedSessionsCount"_attr = nSessionsTerminated);
}

}  // namespace mongo::transport::grpc
