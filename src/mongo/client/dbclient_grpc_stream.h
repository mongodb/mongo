// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/dbclient_session.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_options.h"

#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {

class ClientAPIVersionParameters;

/**
 *  A basic connection to the database, backed by a gRPC stream.
 *  This is the main entry point for talking to a simple Mongo setup through gRPC.
 */
class DBClientGRPCStream : public DBClientSession {
public:
    DBClientGRPCStream(boost::optional<std::string> authToken = boost::none,
                       bool _autoReconnect = false,
                       double so_timeout = 0,
                       MongoURI uri = {},
                       const HandshakeValidationHook& hook = HandshakeValidationHook(),
                       const ClientAPIVersionParameters* apiParameters = nullptr)
        : DBClientSession(_autoReconnect, so_timeout, std::move(uri), hook, apiParameters),
          _authToken{std::move(authToken)} {}

    ~DBClientGRPCStream() override;

    /**
     * Logout is not implemented for gRPC, throws an exception.
     */
    void logout(const DatabaseName& dbname, BSONObj& info) override {
        uasserted(ErrorCodes::NotImplemented, "gRPC does not support logout() command.");
    }

    /**
     * Authentication is not implemented for gRPC, throws an exception.
     */
    void authenticateInternalUser(auth::StepDownBehavior stepDownBehavior =
                                      auth::StepDownBehavior::kKillConnection) override {
        uasserted(ErrorCodes::NotImplemented, "gRPC does not support user authentication.");
    }

    /**
     * The value returned from the initial connection handshake's minWireVersion.
     */
    int getMinWireVersion() override;

    /**
     * clusterMaxWireVersion for gRPC EgressSession, or the value returned from the
     * DBClientSession::getMaxWireVersion() if connect() has not been called.
     */
    int getMaxWireVersion() override;

#ifdef MONGO_CONFIG_SSL
    const SSLConfiguration* getSSLConfiguration() override {
        if (!_session) {
            return nullptr;
        }
        return _session->getSSLConfiguration();
    }

    bool isTLS() override {
        return true;
    }
#endif

    bool isGRPC() override {
        return true;
    }

private:
    StatusWith<std::shared_ptr<transport::Session>> _makeSession(
        const HostAndPort& host,
        transport::ConnectSSLMode sslMode,
        Milliseconds timeout,
        const boost::optional<TransientSSLParams>& transientSSLParams = boost::none) override;
    void _reconnectSession() override;
    void _killSession() override;
    transport::grpc::EgressSession* _getSession();

    boost::optional<std::string> _authToken;
};

}  // namespace mongo
