/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include <kms_message/kms_message.h>

#include <stdlib.h>

#include "mongo/base/init.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/json.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/shell/kms.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/util/base64.h"
#include "mongo/util/kms_message_support.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/text.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

/**
 * Make a request to a AWS HTTP endpoint.
 *
 * Does not maintain a persistent HTTP connection.
 */
class AWSConnection {
public:
    AWSConnection(SSLManagerInterface* ssl)
        : _sslManager(ssl), _socket(std::make_unique<Socket>(10, logv2::LogSeverity::Info())) {}

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
 * AWS configuration settings
 */
struct AWSConfig {
    // AWS_ACCESS_KEY_ID
    std::string accessKeyId;

    // AWS_SECRET_ACCESS_KEY
    SecureString secretAccessKey;

    // Optional AWS_SESSION_TOKEN for AWS STS tokens
    boost::optional<std::string> sessionToken;
};

/**
 * Manages SSL information and config for how to talk to AWS KMS.
 */
class AWSKMSService : public KMSService {
public:
    AWSKMSService() = default;
    ~AWSKMSService() final = default;

    static std::unique_ptr<KMSService> create(const AwsKMS& config);

    std::vector<uint8_t> encrypt(ConstDataRange cdr, StringData kmsKeyId) final;

    SecureVector<uint8_t> decrypt(ConstDataRange cdr, BSONObj masterKey) final;

    BSONObj encryptDataKey(ConstDataRange cdr, StringData keyId) final;

private:
    void initRequest(kms_request_t* request, StringData host, StringData region);

private:
    // SSL Manager
    std::unique_ptr<SSLManagerInterface> _sslManager;

    // Server to connect to
    HostAndPort _server;

    // AWS configuration settings
    AWSConfig _config;
};

void uassertKmsRequestInternal(kms_request_t* request, bool ok) {
    if (!ok) {
        const char* msg = kms_request_get_error(request);
        uasserted(51135, str::stream() << "Internal AWS KMS Error: " << msg);
    }
}

#define uassertKmsRequest(X) uassertKmsRequestInternal(request, (X));

void AWSKMSService::initRequest(kms_request_t* request, StringData host, StringData region) {

    // use current time
    uassertKmsRequest(kms_request_set_date(request, nullptr));

    uassertKmsRequest(kms_request_set_region(request, region.toString().c_str()));

    // kms is always the name of the service
    uassertKmsRequest(kms_request_set_service(request, "kms"));

    uassertKmsRequest(kms_request_set_access_key_id(request, _config.accessKeyId.c_str()));
    uassertKmsRequest(kms_request_set_secret_key(request, _config.secretAccessKey->c_str()));

    // Set host to be the host we are targeting instead of defaulting to kms.<region>.amazonaws.com
    uassertKmsRequest(kms_request_add_header_field(request, "Host", host.toString().c_str()));

    if (!_config.sessionToken.value_or("").empty()) {
        // TODO: move this into kms-message
        uassertKmsRequest(kms_request_add_header_field(
            request, "X-Amz-Security-Token", _config.sessionToken.get().c_str()));
    }
}

std::vector<uint8_t> toVector(const std::string& str) {
    std::vector<uint8_t> blob;

    std::transform(std::begin(str), std::end(str), std::back_inserter(blob), [](auto c) {
        return static_cast<uint8_t>(c);
    });

    return blob;
}

SecureVector<uint8_t> toSecureVector(const std::string& str) {
    SecureVector<uint8_t> blob(str.length());

    std::transform(std::begin(str), std::end(str), blob->data(), [](auto c) {
        return static_cast<uint8_t>(c);
    });

    return blob;
}

/**
 * Takes in a CMK of the format arn:partition:service:region:account-id:resource (minimum). We
 * care about extracting the region. This function ensures that there are at least 6 partitions,
 * parses the provider, and returns a pair of provider and the region.
 */
std::string parseCMK(StringData cmk) {
    std::vector<std::string> cmkTokenized = StringSplitter::split(cmk.toString(), ":");
    uassert(31040, "Invalid AWS KMS Customer Master Key.", cmkTokenized.size() > 5);
    return cmkTokenized[3];
}

HostAndPort getDefaultHost(StringData region) {
    std::string hostname = str::stream() << "kms." << region << ".amazonaws.com";
    return HostAndPort(hostname, 443);
}

std::vector<uint8_t> AWSKMSService::encrypt(ConstDataRange cdr, StringData kmsKeyId) {
    auto request =
        UniqueKmsRequest(kms_encrypt_request_new(reinterpret_cast<const uint8_t*>(cdr.data()),
                                                 cdr.length(),
                                                 kmsKeyId.toString().c_str(),
                                                 NULL));

    auto region = parseCMK(kmsKeyId);

    if (_server.empty()) {
        _server = getDefaultHost(region);
    }

    initRequest(request.get(), _server.host(), region);

    auto buffer = UniqueKmsCharBuffer(kms_request_get_signed(request.get()));
    auto buffer_len = strlen(buffer.get());

    AWSConnection connection(_sslManager.get());
    auto response = connection.makeOneRequest(_server, ConstDataRange(buffer.get(), buffer_len));

    auto body = kms_response_get_body(response.get(), nullptr);

    BSONObj obj = fromjson(body);

    auto field = obj["__type"];

    if (!field.eoo()) {
        AwsKMSError awsResponse;
        try {
            awsResponse = AwsKMSError::parse(IDLParserErrorContext("awsEncryptError"), obj);
        } catch (DBException& dbe) {
            uasserted(51274,
                      str::stream() << "AWS KMS failed to parse error message: " << dbe.toString()
                                    << ", Response : " << obj);
        }

        uasserted(51224,
                  str::stream() << "AWS KMS failed to encrypt: " << awsResponse.getType() << " : "
                                << awsResponse.getMessage());
    }

    auto awsResponse = AwsEncryptResponse::parse(IDLParserErrorContext("awsEncryptResponse"), obj);

    auto blobStr = base64::decode(awsResponse.getCiphertextBlob().toString());

    return toVector(blobStr);
}

BSONObj AWSKMSService::encryptDataKey(ConstDataRange cdr, StringData keyId) {
    auto dataKey = encrypt(cdr, keyId);

    AwsMasterKey masterKey;
    masterKey.setKey(keyId);
    masterKey.setRegion(parseCMK(keyId));
    masterKey.setEndpoint(boost::optional<StringData>(_server.toString()));

    AwsMasterKeyAndMaterial keyAndMaterial;
    keyAndMaterial.setKeyMaterial(dataKey);
    keyAndMaterial.setMasterKey(masterKey);

    return keyAndMaterial.toBSON();
}

SecureVector<uint8_t> AWSKMSService::decrypt(ConstDataRange cdr, BSONObj masterKey) {
    auto awsMasterKey = AwsMasterKey::parse(IDLParserErrorContext("awsMasterKey"), masterKey);

    auto request = UniqueKmsRequest(kms_decrypt_request_new(
        reinterpret_cast<const uint8_t*>(cdr.data()), cdr.length(), nullptr));

    if (_server.empty()) {
        _server = getDefaultHost(awsMasterKey.getRegion());
    }

    initRequest(request.get(), _server.host(), awsMasterKey.getRegion());

    auto buffer = UniqueKmsCharBuffer(kms_request_get_signed(request.get()));
    auto buffer_len = strlen(buffer.get());
    AWSConnection connection(_sslManager.get());
    auto response = connection.makeOneRequest(_server, ConstDataRange(buffer.get(), buffer_len));

    auto body = kms_response_get_body(response.get(), nullptr);

    BSONObj obj = fromjson(body);

    auto field = obj["__type"];

    if (!field.eoo()) {
        AwsKMSError awsResponse;
        try {
            awsResponse = AwsKMSError::parse(IDLParserErrorContext("awsDecryptError"), obj);
        } catch (DBException& dbe) {
            uasserted(51275,
                      str::stream() << "AWS KMS failed to parse error message: " << dbe.toString()
                                    << ", Response : " << obj);
        }

        uasserted(51225,
                  str::stream() << "AWS KMS failed to decrypt: " << awsResponse.getType() << " : "
                                << awsResponse.getMessage());
    }

    auto awsResponse = AwsDecryptResponse::parse(IDLParserErrorContext("awsDecryptResponse"), obj);

    auto blobStr = base64::decode(awsResponse.getPlaintext().toString());

    return toSecureVector(blobStr);
}

void AWSConnection::connect(const HostAndPort& host) {
    SockAddr server(host.host().c_str(), host.port(), AF_UNSPEC);

    uassert(51136,
            str::stream() << "AWS KMS server address " << host.host() << " is invalid.",
            server.isValid());

    int attempt = 0;
    bool connected = false;
    while ((connected == false) && (attempt < 20)) {
        connected = _socket->connect(server);
        attempt++;
    }
    uassert(51137,
            str::stream() << "Could not connect to AWS KMS server " << server.toString(),
            connected);

    uassert(51138,
            str::stream() << "Failed to perform SSL handshake with the AWS KMS server "
                          << host.toString(),
            _socket->secure(_sslManager, host.host()));
}

// Sends a request message to the AWS KMS server and creates a KMS Response.
UniqueKmsResponse AWSConnection::sendRequest(ConstDataRange request) {
    std::array<char, 512> resp;

    _socket->send(
        reinterpret_cast<const char*>(request.data()), request.length(), "AWS KMS request");

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

UniqueKmsResponse AWSConnection::makeOneRequest(const HostAndPort& host, ConstDataRange request) {
    connect(host);

    auto resp = sendRequest(request);

    _socket->close();

    return resp;
}

boost::optional<std::string> toString(boost::optional<StringData> str) {
    if (str) {
        return {str.get().toString()};
    }
    return boost::none;
}

std::unique_ptr<KMSService> AWSKMSService::create(const AwsKMS& config) {
    auto awsKMS = std::make_unique<AWSKMSService>();

    SSLParams params;
    params.sslPEMKeyFile = "";
    params.sslPEMKeyPassword = "";
    params.sslClusterFile = "";
    params.sslClusterPassword = "";
    params.sslCAFile = "";

    params.sslCRLFile = "";

    // Copy the rest from the global SSL manager options.
    params.sslFIPSMode = sslGlobalParams.sslFIPSMode;

    // KMS servers never should have invalid certificates
    params.sslAllowInvalidCertificates = false;
    params.sslAllowInvalidHostnames = false;

    params.sslDisabledProtocols =
        std::vector({SSLParams::Protocols::TLS1_0, SSLParams::Protocols::TLS1_1});

    // Leave the CA file empty so we default to system CA but for local testing allow it to inherit
    // the CA file.
    if (!config.getUrl().value_or("").empty()) {
        params.sslCAFile = sslGlobalParams.sslCAFile;
        awsKMS->_server = parseUrl(config.getUrl().get());
    }

    awsKMS->_sslManager = SSLManagerInterface::create(params, false);

    awsKMS->_config.accessKeyId = config.getAccessKeyId().toString();

    awsKMS->_config.secretAccessKey = config.getSecretAccessKey().toString();

    awsKMS->_config.sessionToken = toString(config.getSessionToken());

    return awsKMS;
}

/**
 * Factory for AWSKMSService if user specifies aws config to mongo() JS constructor.
 */
class AWSKMSServiceFactory final : public KMSServiceFactory {
public:
    AWSKMSServiceFactory() = default;
    ~AWSKMSServiceFactory() = default;

    std::unique_ptr<KMSService> create(const BSONObj& config) final {
        auto field = config[KmsProviders::kAwsFieldName];
        if (field.eoo()) {
            return nullptr;
        }
        auto obj = field.Obj();
        return AWSKMSService::create(AwsKMS::parse(IDLParserErrorContext("root"), obj));
    }
};

}  // namespace

MONGO_INITIALIZER(KMSRegister)(::mongo::InitializerContext* context) {
    kms_message_init();
    KMSServiceController::registerFactory(KMSProviderEnum::aws,
                                          std::make_unique<AWSKMSServiceFactory>());
    return Status::OK();
}

}  // namespace mongo
