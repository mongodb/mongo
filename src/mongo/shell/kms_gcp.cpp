/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <fmt/format.h>
#include <kms_message/kms_gcp_request.h>
#include <kms_message/kms_message.h>

#include "mongo/bson/json.h"
#include "mongo/shell/kms.h"
#include "mongo/shell/kms_network.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
namespace {

using namespace fmt::literals;

constexpr StringData kGcpKms = "gcp"_sd;

// Default endpoints for GCP
constexpr StringData kDefaultOauthEndpoint = "oauth2.googleapis.com"_sd;
constexpr StringData kDefaultOauthScope = "https://www.googleapis.com/auth/cloudkms"_sd;
constexpr StringData kGcpKMSEndpoint = "https://cloudkms.googleapis.com:443"_sd;

// Field names for BSON objects containing key vault information
constexpr StringData kProjectIdField = "projectId"_sd;
constexpr StringData kLocationIdField = "location"_sd;
constexpr StringData kKeyRingField = "keyRing"_sd;
constexpr StringData kKeyNameField = "keyName"_sd;
constexpr StringData kKeyVerisionField = "keyVersion"_sd;

/**
 * GCP configuration settings
 */
struct GCPConfig {
    // E-mail address that will be used for GCP OAuth requests.
    std::string email;

    // PKCS#8 private key
    std::string privateKey;

    // Options to pass to GCP KMS requests
    UniqueKmsRequestOpts opts;
};

void uassertKmsRequestInternal(kms_request_t* request, bool ok) {
    if (!ok) {
        const char* msg = kms_request_get_error(request);
        uasserted(5265000, str::stream() << "Internal GCP KMS Error: " << msg);
    }
}

#define uassertKmsRequest(X) uassertKmsRequestInternal(request, (X))

/**
 * Manages OAuth token requests and caching
 */
class GCPKMSOAuthService final : public KMSOAuthService {
public:
    GCPKMSOAuthService(const GCPConfig& config,
                       HostAndPort endpoint,
                       std::shared_ptr<SSLManagerInterface> sslManager)
        : KMSOAuthService(endpoint, sslManager), _config(config) {}

protected:
    UniqueKmsRequest getOAuthRequest() {
        std::string audience = "https://{}/token"_format(_oAuthEndpoint.host());
        std::string scope;
        if (_oAuthEndpoint.host() != kDefaultOauthEndpoint.toString()) {
            scope = "https://www.{}/auth/cloudkms"_format(_oAuthEndpoint.toString());
        } else {
            scope = kDefaultOauthScope.toString();
        }
        uassert(5365009,
                str::stream() << "Internal GCP KMS Error: Private key not encoded in base64.",
                base64::validate(_config.privateKey));
        std::string privateKeyDecoded = base64::decode(_config.privateKey);

        auto request = UniqueKmsRequest(kms_gcp_request_oauth_new(_oAuthEndpoint.host().c_str(),
                                                                  _config.email.c_str(),
                                                                  audience.c_str(),
                                                                  scope.c_str(),
                                                                  privateKeyDecoded.c_str(),
                                                                  privateKeyDecoded.size(),
                                                                  _config.opts.get()));

        const char* msg = kms_request_get_error(request.get());
        uassert(5265003, str::stream() << "Internal GCP KMS Error: " << msg, msg == nullptr);

        return request;
    }

private:
    const GCPConfig& _config;
};

/**
 * Manages SSL information and config for how to talk to GCP KMS.
 */
class GCPKMSService final : public KMSService {
public:
    GCPKMSService() = default;

    static std::unique_ptr<KMSService> create(const GcpKMS&);


    SecureVector<uint8_t> decrypt(ConstDataRange cdr, BSONObj masterKey) final;

    BSONObj encryptDataKeyByBSONObj(ConstDataRange cdr, BSONObj keyId) final;

    void configureOauthService(HostAndPort endpoint);

    StringData name() const {
        return kGcpKms;
    }

private:
    std::vector<uint8_t> encrypt(ConstDataRange cdr, const BSONObj& kmsKeyId);

private:
    // SSL Manager
    std::shared_ptr<SSLManagerInterface> _sslManager;

    // Server to connect to
    HostAndPort _server;

    // GCP configuration settings
    GCPConfig _config;

    // Service for managing oauth requests and token cache
    std::unique_ptr<GCPKMSOAuthService> _oauthService;
};

std::vector<uint8_t> GCPKMSService::encrypt(ConstDataRange cdr, const BSONObj& kmsKeyId) {
    StringData bearerToken = _oauthService->getBearerToken();
    GcpMasterKey masterKey = GcpMasterKey::parse(IDLParserContext("gcpMasterKey"), kmsKeyId);

    auto request = UniqueKmsRequest(kms_gcp_request_encrypt_new(
        _server.host().c_str(),
        bearerToken.toString().c_str(),
        masterKey.getProjectId().toString().c_str(),
        masterKey.getLocation().toString().c_str(),
        masterKey.getKeyRing().toString().c_str(),
        masterKey.getKeyName().toString().c_str(),
        masterKey.getKeyVersion().has_value() ? masterKey.getKeyVersion().value().toString().c_str()
                                              : nullptr,
        reinterpret_cast<const uint8_t*>(cdr.data()),
        cdr.length(),
        _config.opts.get()));

    auto buffer = UniqueKmsCharBuffer(kms_request_to_string(request.get()));
    auto buffer_len = strlen(buffer.get());

    KMSNetworkConnection connection(_sslManager.get());
    auto response = connection.makeOneRequest(_server, ConstDataRange(buffer.get(), buffer_len));

    auto body = kms_response_get_body(response.get(), nullptr);

    BSONObj obj = fromjson(body);

    if (obj.hasField("error")) {
        GcpKMSError gcpResponse;
        try {
            gcpResponse =
                GcpKMSError::parse(IDLParserContext("gcpEncryptError"), obj["error"].Obj());
        } catch (DBException& dbe) {
            uasserted(5265005,
                      str::stream() << "GCP KMS failed to parse error message: " << dbe.toString()
                                    << ", Response : " << obj);
        }

        uasserted(5256006,
                  str::stream() << "GCP KMS failed to encrypt: " << gcpResponse.getCode() << " "
                                << gcpResponse.getStatus() << " : " << gcpResponse.getMessage());
    }

    auto gcpResponce = GcpEncryptResponse::parse(IDLParserContext("gcpEncryptResponse"), obj);

    auto blobStr = base64::decode(gcpResponce.getCiphertext());

    return kmsResponseToVector(blobStr);
}

SecureVector<uint8_t> GCPKMSService::decrypt(ConstDataRange cdr, BSONObj masterKey) {
    auto gcpMasterKey = GcpMasterKey::parse(IDLParserContext("gcpMasterKey"), masterKey);
    StringData bearerToken = _oauthService->getBearerToken();

    auto request =
        UniqueKmsRequest(kms_gcp_request_decrypt_new(_server.host().c_str(),
                                                     bearerToken.toString().c_str(),
                                                     gcpMasterKey.getProjectId().toString().c_str(),
                                                     gcpMasterKey.getLocation().toString().c_str(),
                                                     gcpMasterKey.getKeyRing().toString().c_str(),
                                                     gcpMasterKey.getKeyName().toString().c_str(),
                                                     reinterpret_cast<const uint8_t*>(cdr.data()),
                                                     cdr.length(),
                                                     _config.opts.get()));

    auto buffer = UniqueKmsCharBuffer(kms_request_to_string(request.get()));
    auto buffer_len = strlen(buffer.get());
    KMSNetworkConnection connection(_sslManager.get());
    auto response = connection.makeOneRequest(_server, ConstDataRange(buffer.get(), buffer_len));

    auto body = kms_response_get_body(response.get(), nullptr);

    BSONObj obj = fromjson(body);

    if (obj.hasField("error")) {
        GcpKMSError gcpResponse;
        try {
            gcpResponse =
                GcpKMSError::parse(IDLParserContext("gcpDecryptError"), obj["error"].Obj());
        } catch (DBException& dbe) {
            uasserted(5265007,
                      str::stream() << "GCP KMS failed to parse error message: " << dbe.toString()
                                    << ", Response : " << obj);
        }

        uasserted(5256008,
                  str::stream() << "GCP KMS failed to decrypt: " << gcpResponse.getCode() << " "
                                << gcpResponse.getStatus() << " : " << gcpResponse.getMessage());
    }

    auto gcpResponce = GcpDecryptResponse::parse(IDLParserContext("gcpDecryptResponse"), obj);

    auto blobStr = base64::decode(gcpResponce.getPlaintext());

    return kmsResponseToSecureVector(blobStr);
}

BSONObj GCPKMSService::encryptDataKeyByBSONObj(ConstDataRange cdr, BSONObj keyId) {
    auto dataKey = encrypt(cdr, keyId);

    GcpMasterKey masterKey = GcpMasterKey::parse(IDLParserContext("gcpMasterKey"), keyId);

    GcpMasterKeyAndMaterial keyAndMaterial;
    keyAndMaterial.setKeyMaterial(std::move(dataKey));
    keyAndMaterial.setMasterKey(std::move(masterKey));

    return keyAndMaterial.toBSON();
}

std::unique_ptr<KMSService> GCPKMSService::create(const GcpKMS& config) {
    auto gcpKMS = std::make_unique<GCPKMSService>();

    SSLParams params;
    getSSLParamsForNetworkKMS(&params);

    HostAndPort oauthHostAndPort(kDefaultOauthEndpoint.toString(), 443);
    if (config.getEndpoint().has_value()) {
        // Leave the CA file empty so we default to system CA but for local testing allow it to
        // inherit the CA file.
        params.sslCAFile = sslGlobalParams.sslCAFile;
        oauthHostAndPort = parseUrl(config.getEndpoint().get());
    }

    gcpKMS->_sslManager = SSLManagerInterface::create(params, false);

    gcpKMS->configureOauthService(oauthHostAndPort);

    gcpKMS->_server = parseUrl(config.getEndpoint().value_or(kGcpKMSEndpoint));

    gcpKMS->_config.email = config.getEmail().toString();

    gcpKMS->_config.opts = UniqueKmsRequestOpts(kms_request_opt_new());
    kms_request_opt_set_provider(gcpKMS->_config.opts.get(), KMS_REQUEST_PROVIDER_GCP);

    gcpKMS->_config.privateKey = config.getPrivateKey().toString();

    return gcpKMS;
}

void GCPKMSService::configureOauthService(HostAndPort endpoint) {
    _oauthService = std::make_unique<GCPKMSOAuthService>(_config, endpoint, _sslManager);
}

/**
 * Factory for GCPKMSService if user specifies gcp config to mongo() JS constructor.
 */
class GCPKMSServiceFactory final : public KMSServiceFactory {
public:
    GCPKMSServiceFactory() = default;
    ~GCPKMSServiceFactory() = default;

    std::unique_ptr<KMSService> create(const BSONObj& config) final {
        auto field = config[KmsProviders::kGcpFieldName];
        if (field.eoo()) {
            return nullptr;
        }
        uassert(5265009,
                "Misconfigured GCP KMS Config: {}"_format(field.toString()),
                field.type() == BSONType::Object);
        auto obj = field.Obj();
        return GCPKMSService::create(GcpKMS::parse(IDLParserContext("root"), obj));
    }
};

}  // namespace

MONGO_INITIALIZER(KMSRegisterGCP)(::mongo::InitializerContext*) {
    kms_message_init();
    KMSServiceController::registerFactory(KMSProviderEnum::gcp,
                                          std::make_unique<GCPKMSServiceFactory>());
}

}  // namespace mongo
