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


#include "mongo/platform/basic.h"

#include "mongo/shell/kms_network.h"

#include "mongo/bson/json.h"
#include "mongo/shell/kms.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/util/text.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

void KMSNetworkConnection::connect(const HostAndPort& host) {
    auto makeAddress = [](const auto& host) -> SockAddr {
        try {
            return SockAddr::create(host.host().c_str(), host.port(), AF_UNSPEC);
        } catch (const DBException& ex) {
            uasserted(51136, "Unable to resolve KMS server address" + causedBy(ex));
        }

        MONGO_UNREACHABLE;
    };

    auto addr = makeAddress(host);

    size_t attempt = 0;
    constexpr size_t kMaxAttempts = 20;
    while (!_socket->connect(addr)) {
        ++attempt;
        if (attempt > kMaxAttempts) {
            uasserted(51137,
                      str::stream() << "Could not connect to KMS server " << addr.toString());
        }
    }

    if (!_socket->secure(_sslManager, host.host())) {
        uasserted(51138,
                  str::stream() << "Failed to perform SSL handshake with the KMS server "
                                << addr.toString());
    }
}

// Sends a request message to the KMS server and creates a KMS Response.
UniqueKmsResponse KMSNetworkConnection::sendRequest(ConstDataRange request) {
    std::array<char, 512> resp;

    _socket->send(reinterpret_cast<const char*>(request.data()), request.length(), "KMS request");

    auto parser = UniqueKmsResponseParser(kms_response_parser_new());
    int bytes_to_read = 0;

    while ((bytes_to_read = kms_response_parser_wants_bytes(parser.get(), resp.size())) > 0) {
        bytes_to_read = std::min(bytes_to_read, static_cast<int>(resp.size()));
        bytes_to_read = _socket->unsafe_recv(resp.data(), bytes_to_read);

        uassert(51139,
                "kms_response_parser_feed failed",
                kms_response_parser_feed(
                    parser.get(), reinterpret_cast<uint8_t*>(resp.data()), bytes_to_read));
    }

    auto response = UniqueKmsResponse(kms_response_parser_get_response(parser.get()));

    return response;
}

UniqueKmsResponse KMSNetworkConnection::makeOneRequest(const HostAndPort& host,
                                                       ConstDataRange request) {
    connect(host);

    auto resp = sendRequest(request);

    _socket->close();

    return resp;
}

void getSSLParamsForNetworkKMS(SSLParams* params) {
    params->sslPEMKeyFile = "";
    params->sslPEMKeyPassword = "";
    params->sslClusterFile = "";
    params->sslClusterPassword = "";
    params->sslCAFile = "";

    params->sslCRLFile = "";

    // Copy the rest from the global SSL manager options.
    params->sslFIPSMode = sslGlobalParams.sslFIPSMode;

    // KMS servers never should have invalid certificates
    params->sslAllowInvalidCertificates = false;
    params->sslAllowInvalidHostnames = false;

    params->sslDisabledProtocols =
        std::vector({SSLParams::Protocols::TLS1_0, SSLParams::Protocols::TLS1_1});
}

std::vector<uint8_t> kmsResponseToVector(StringData str) {
    std::vector<uint8_t> blob;

    std::transform(std::begin(str), std::end(str), std::back_inserter(blob), [](auto c) {
        return static_cast<uint8_t>(c);
    });

    return blob;
}

SecureVector<uint8_t> kmsResponseToSecureVector(StringData str) {
    SecureVector<uint8_t> blob(str.size());

    std::transform(std::begin(str), std::end(str), blob->data(), [](auto c) {
        return static_cast<uint8_t>(c);
    });

    return blob;
}

StringData KMSOAuthService::getBearerToken() {

    if (!_cachedToken.empty() && _expirationDateTime > Date_t::now()) {
        return _cachedToken;
    }

    makeBearerTokenRequest();

    return _cachedToken;
}

void KMSOAuthService::makeBearerTokenRequest() {
    UniqueKmsRequest request = getOAuthRequest();

    auto buffer = UniqueKmsCharBuffer(kms_request_to_string(request.get()));
    auto buffer_len = strlen(buffer.get());

    KMSNetworkConnection connection(_sslManager.get());
    auto response =
        connection.makeOneRequest(_oAuthEndpoint, ConstDataRange(buffer.get(), buffer_len));

    auto body = kms_response_get_body(response.get(), nullptr);

    BSONObj obj = fromjson(body);

    auto field = obj[OAuthErrorResponse::kErrorFieldName];

    if (!field.eoo()) {
        OAuthErrorResponse oAuthErrorResponse;
        try {
            oAuthErrorResponse = OAuthErrorResponse::parse(IDLParserContext("oauthError"), obj);
        } catch (DBException& dbe) {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "Failed to parse error message: " << dbe.toString()
                                    << ", Response : " << obj);
        }

        std::string description;
        if (oAuthErrorResponse.getError_description().has_value()) {
            description = str::stream()
                << " : " << oAuthErrorResponse.getError_description().value().toString();
        }
        uasserted(ErrorCodes::OperationFailed,
                  str::stream() << "Failed to make oauth request: " << oAuthErrorResponse.getError()
                                << description);
    }

    auto kmsResponse = OAuthResponse::parse(IDLParserContext("OAuthResponse"), obj);

    _cachedToken = kmsResponse.getAccess_token().toString();

    // Offset the expiration time by a the socket timeout as proxy for round-trip time to the OAuth
    // server. This approximation will compute the expiration time a litte earlier then needed but
    // will ensure that it uses a stale bearer token.
    Seconds requestBufferTime = 2 * Seconds((int)KMSNetworkConnection::so_timeout_seconds);

    // expires_in is optional but Azure and GCP always return it but to be safe, we pick a default
    _expirationDateTime =
        Date_t::now() + Seconds(kmsResponse.getExpires_in().value_or(60)) - requestBufferTime;
}

}  // namespace mongo
