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
