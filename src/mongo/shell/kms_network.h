/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <string>

#include "mongo/util/kms_message_support.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * Make a request to an HTTP endpoint.
 *
 * Does not maintain a persistent HTTP connection.
 */
class KMSNetworkConnection {
public:
    static constexpr double so_timeout_seconds = 10;

    KMSNetworkConnection(SSLManagerInterface* ssl)
        : _sslManager(ssl),
          _socket(std::make_unique<Socket>(so_timeout_seconds, logv2::LogSeverity::Info())) {}

    UniqueKmsResponse makeOneRequest(const HostAndPort& host, ConstDataRange request);

private:
    UniqueKmsResponse sendRequest(ConstDataRange request);

    void connect(const HostAndPort& host);

private:
    // SSL Manager for connections
    SSLManagerInterface* _sslManager;

    // Synchronous socket
    std::unique_ptr<Socket> _socket;
};

/**
 * Creates an initial SSLParams object for KMS over the network.
 */
void getSSLParamsForNetworkKMS(SSLParams*);

/**
 * Converts a base64 encoded KMS response to a vector of bytes.
 */
std::vector<uint8_t> kmsResponseToVector(const std::string& str);

/**
 * Converts a base64 encoded KMS response to a securely allocated vector of bytes.
 */
SecureVector<uint8_t> kmsResponseToSecureVector(const std::string& str);

/**
 * Base class for KMS services that use OAuth for authorization.
 *
 * Each service only talks to one OAuth endpoint so caching bearer tokens is simple.
 */
class KMSOAuthService {
public:
    KMSOAuthService(HostAndPort oAuthEndpoint, std::shared_ptr<SSLManagerInterface> sslManager)
        : _oAuthEndpoint(oAuthEndpoint), _sslManager(sslManager) {}

    /**
     * Get a bearer token to use to make requests. It may be cached.
     */
    StringData getBearerToken();

protected:
    /**
     * Construct a valid kms request for retrieving a new OAuth Bearer token
     */
    virtual UniqueKmsRequest getOAuthRequest() = 0;

protected:
    // OAuth Service endpoint
    HostAndPort _oAuthEndpoint;

private:
    /**
     * Make a TLS request to a service to fetch a bearer token.
     */
    void makeBearerTokenRequest();

private:
    // SSL Manager
    std::shared_ptr<SSLManagerInterface> _sslManager;

    // Cached access token
    std::string _cachedToken;

    // Expiration datetime of access token
    Date_t _expirationDateTime;
};

}  // namespace mongo
