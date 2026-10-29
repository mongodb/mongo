// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/dbclient_grpc_stream.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/grpc/grpc_transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo {

DBClientGRPCStream::~DBClientGRPCStream() {
    if (auto session = _getSession()) {
        if (auto status = session->finish(); !status.isOK()) {
            LOGV2(8393201,
                  "RPC associated with DBClientGRPCStream did not terminate successfully",
                  "clientId"_attr = session->getClientId(),
                  "remote"_attr = getServerHostAndPort(),
                  "terminationStatus"_attr = status);
        }
    }
}

StatusWith<std::shared_ptr<transport::Session>> DBClientGRPCStream::_makeSession(
    const HostAndPort& host,
    transport::ConnectSSLMode sslMode,
    Milliseconds timeout,
    const boost::optional<TransientSSLParams>& transientSSLParams) {
    auto grpcLayer = checked_cast<transport::grpc::GRPCTransportLayer*>(
        getGlobalServiceContext()->getTransportLayerManager()->getTransportLayer(
            transport::TransportProtocol::GRPC));
    invariant(grpcLayer);
    return grpcLayer->connectWithAuthToken(host, sslMode, std::move(timeout), _authToken);
}

void DBClientGRPCStream::_reconnectSession() {
    if (auto oldSession = _getSession()) {
        auto status = oldSession->finish();
        LOGV2(8393202,
              "Trying to re-establish gRPC stream",
              "remote"_attr = getServerHostAndPort(),
              "priorStreamTerminationStatus"_attr = status);
    } else {
        LOGV2_DEBUG(8057001,
                    _logLevel.toInt(),
                    "Trying to re-establish gRPC stream",
                    "remote"_attr = getServerHostAndPort());
    }

    try {
        connect(_serverAddress, _applicationName, _transientSSLParams);
    } catch (const DBException& e) {
        _markFailed(kSetFlag);
        LOGV2_DEBUG(8057002,
                    _logLevel.toInt(),
                    "gRPC stream re-establishment failed",
                    "remote"_attr = getServerHostAndPort(),
                    "error"_attr = e.toStatus());
        throw;
    }

    LOGV2_DEBUG(8057003,
                _logLevel.toInt(),
                "Successfully re-established gRPC stream",
                "remote"_attr = getServerHostAndPort());
}

transport::grpc::EgressSession* DBClientGRPCStream::_getSession() {
    if (!_session) {
        return nullptr;
    }
    auto egressSession = dynamic_cast<transport::grpc::EgressSession*>(_session.get());
    invariant(egressSession,
              "_session must be an instance of GRPCSession:EgressSession in DBClientGRPCStream.");
    return egressSession;
}

void DBClientGRPCStream::_killSession() {
    transport::grpc::EgressSession* session = _getSession();
    if (!session) {
        return;
    }

    session->cancel(Status(ErrorCodes::CallbackCanceled,
                           "Client is disconnecting, cancelling the outstanding RPC"));
}

int DBClientGRPCStream::getMinWireVersion() {
    return DBClientSession::getMinWireVersion();
};

int DBClientGRPCStream::getMaxWireVersion() {
    transport::grpc::EgressSession* session = _getSession();
    if (!session) {
        return DBClientSession::getMaxWireVersion();
    }

    return session->getClusterMaxWireVersion();
};

}  // namespace mongo
