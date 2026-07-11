// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/service.h"

#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/transport/grpc/grpc_server_context.h"
#include "mongo/transport/grpc/grpc_server_stream.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/util.h"
#include "mongo/util/base64.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer.h"

#include <charconv>
#include <cstring>
#include <functional>
#include <numeric>
#include <string_view>

#include <fmt/format.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/rpc_method.h>
#include <grpcpp/impl/rpc_service_method.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/method_handler.h>
#include <grpcpp/support/status_code_enum.h>
#include <grpcpp/support/sync_stream.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {

namespace {
using namespace std::literals::string_view_literals;

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
                                               .getIncomingExternalClient()
                                               .minWireVersion;
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
    static constexpr std::string_view kReservedMetadataKeyPrefix = "mongodb"sv;

    for (const auto& entry : clientMetadata) {
        const auto& key = entry.first;
        if (key.starts_with(kReservedMetadataKeyPrefix) &&
            !kRecognizedClientMetadataKeys.contains(key)) {
            // We do not send the invalid metadata back to the client for security reasons.
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                  "Unrecognized reserved metadata key.");
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
            // We do not send the invalid metadata or error string back to the client for security
            // reasons.
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                  "The provided client ID is not a valid UUID.");
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

    boost::optional<std::string_view> base64EncodedClientMetadata;
    if (auto clientMetadataEntry = clientMetadata.find(util::constants::kClientMetadataKey);
        clientMetadataEntry != clientMetadata.end()) {
        base64EncodedClientMetadata = clientMetadataEntry->second;
    }

    auto session = std::make_shared<IngressSession>(
        _tl, &serverCtx, &stream, clientId, authToken, base64EncodedClientMetadata);
    std::list<InSessionPtr>::iterator it;
    {
        std::lock_guard lk{_mutex};

        if (_shutdown || !_acceptNewRequests) {
            session->setTerminationStatus(makeShutdownTerminationStatus());
            return util::convertStatus(*session->terminationStatus());
        }
        it = _sessions.insert(_sessions.begin(), session);
    }
    ON_BLOCK_EXIT([&]() {
        std::lock_guard lk{_mutex};
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
    return _handleStream(serverCtx, stream, std::string{authTokenEntry->second});
}

void CommandService::shutdown() {
    std::unique_lock lk{_mutex};

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
    std::unique_lock lk{_mutex};
    _acceptNewRequests = false;
}

}  // namespace mongo::transport::grpc
