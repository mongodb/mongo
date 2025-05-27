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
