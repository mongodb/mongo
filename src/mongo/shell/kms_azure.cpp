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

#include "kms_message/kms_request.h"
#include "mongo/shell/kms_gen.h"

#include "mongo/platform/basic.h"

#include <fmt/format.h>
#include <kms_message/kms_azure_request.h>
#include <kms_message/kms_b64.h>
#include <kms_message/kms_message.h>

#include "mongo/bson/json.h"
#include "mongo/shell/kms.h"
#include "mongo/shell/kms_network.h"
#include "mongo/util/net/hostandport.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::logv2::LogComponent::kControl


namespace mongo {
namespace {

using namespace fmt::literals;

constexpr auto kAzureKms = "azure"_sd;

// Default endpoints for Azure
constexpr auto kDefaultIdentityPlatformEndpoint = "login.microsoftonline.com"_sd;
// Since scope is passed as URL parameter, it needs to be escaped and kms_message does not escape
// it.
constexpr auto kDefaultOAuthScope = "https%3A%2F%2Fvault.azure.net%2F.default"_sd;

struct AzureConfig {
    // ID for the user in Azure
    std::string tenantId;

    // ID for the application in Azure
    std::string clientId;

    // Secret key for the application in Azure
    std::string clientSecret;

    // Options to pass to kms-message
    UniqueKmsRequestOpts opts;
};

/**
 * Manages OAuth token requests and caching
 */
class AzureKMSOAuthService final : public KMSOAuthService {
public:
    AzureKMSOAuthService(const AzureConfig& config,
                         HostAndPort endpoint,
                         std::shared_ptr<SSLManagerInterface> sslManager)
        : KMSOAuthService(endpoint, sslManager), _config(config) {}

protected:
    UniqueKmsRequest getOAuthRequest() final {
        auto request =
            UniqueKmsRequest(kms_azure_request_oauth_new(_oAuthEndpoint.host().c_str(),
                                                         kDefaultOAuthScope.toString().c_str(),
                                                         _config.tenantId.c_str(),
                                                         _config.clientId.c_str(),
                                                         _config.clientSecret.c_str(),
                                                         _config.opts.get()));

        const char* msg = kms_request_get_error(request.get());
        uassert(5265101, "Internal Azure KMS Error: {}"_format(msg), msg == nullptr);

        return request;
    }

private:
    const AzureConfig& _config;
};

/**
 * Manages SSL information and config for how to talk to Azure KMS.
 */
class AzureKMSService final : public KMSService {
public:
    AzureKMSService() = default;

    static std::unique_ptr<KMSService> create(const AzureKMS& config);

    SecureVector<uint8_t> decrypt(ConstDataRange cdr, BSONObj masterKey) final;

    BSONObj encryptDataKeyByBSONObj(ConstDataRange cdr, BSONObj keyId) final;

    StringData name() const final {
        return kAzureKms;
    }

private:
    template <typename AzureResponseT>
    std::unique_ptr<uint8_t, decltype(std::free)*> makeRequest(kms_request_t* request,
                                                               const HostAndPort& keyVaultEndpoint,
                                                               size_t* raw_len);

private:
    // SSL Manager
    std::shared_ptr<SSLManagerInterface> _sslManager;

    // Azure configuration settings
    AzureConfig _config;

    // Service fr managing OAuth requests and token cache
    std::unique_ptr<AzureKMSOAuthService> _oauthService;
};

std::unique_ptr<KMSService> AzureKMSService::create(const AzureKMS& config) {
    auto azureKMS = std::make_unique<AzureKMSService>();

    SSLParams params;
    getSSLParamsForNetworkKMS(&params);

    HostAndPort identityPlatformHostAndPort(kDefaultIdentityPlatformEndpoint.toString(), 443);
    if (config.getIdentityPlatformEndpoint().has_value()) {
        // Leave the CA file empty so we default to system CA but for local testing allow it to
        // inherit the CA file.
        params.sslCAFile = sslGlobalParams.sslCAFile;
        identityPlatformHostAndPort = parseUrl(config.getIdentityPlatformEndpoint().get());
    }

    azureKMS->_sslManager = SSLManagerInterface::create(params, false);

    azureKMS->_config.opts = UniqueKmsRequestOpts(kms_request_opt_new());
    kms_request_opt_set_provider(azureKMS->_config.opts.get(), KMS_REQUEST_PROVIDER_AZURE);

    azureKMS->_config.clientSecret = config.getClientSecret().toString();

    azureKMS->_config.clientId = config.getClientId().toString();

    azureKMS->_config.tenantId = config.getTenantId().toString();

    azureKMS->_oauthService = std::make_unique<AzureKMSOAuthService>(
        azureKMS->_config, identityPlatformHostAndPort, azureKMS->_sslManager);

    return azureKMS;
}

HostAndPort parseEndpoint(StringData endpoint) {
    HostAndPort host(endpoint);

    if (host.hasPort()) {
        return host;
    }

    return {host.host(), 443};
}

template <typename AzureResponseT>
std::unique_ptr<uint8_t, decltype(std::free)*> AzureKMSService::makeRequest(
    kms_request_t* request, const HostAndPort& keyVaultEndpoint, size_t* raw_len) {
    auto buffer = UniqueKmsCharBuffer(kms_request_to_string(request));
    auto buffer_len = strlen(buffer.get());
    KMSNetworkConnection connection(_sslManager.get());
    auto response =
        connection.makeOneRequest(keyVaultEndpoint, ConstDataRange(buffer.get(), buffer_len));

    auto body = kms_response_get_body(response.get(), nullptr);

    BSONObj obj = fromjson(body);

    if (obj.hasField("error")) {
        AzureKMSError azureResponse;
        try {
            azureResponse =
                AzureKMSError::parse(IDLParserContext("azureError"), obj["error"].Obj());
        } catch (DBException& dbe) {
            uasserted(5265102,
                      "Azure KMS failed to parse error message: {}, Response : {}"_format(
                          dbe.toString(), obj.toString()));
        }

        uasserted(5265103,
                  "Azure KMS failed, response: {} : {}"_format(azureResponse.getCode(),
                                                               azureResponse.getMessage()));
    }

    auto azureResponse = AzureResponseT::parse(IDLParserContext("azureResponse"), obj);

    auto b64Url = azureResponse.getValue().toString();
    std::unique_ptr<uint8_t, decltype(std::free)*> raw_str(
        kms_message_b64url_to_raw(b64Url.c_str(), raw_len), std::free);
    uassert(5265104, "Azure KMS failed to convert key blob from base64 URL.", raw_str != nullptr);

    return raw_str;
}


SecureVector<uint8_t> AzureKMSService::decrypt(ConstDataRange cdr, BSONObj masterKey) {
    auto azureMasterKey = AzureMasterKey::parse(IDLParserContext("azureMasterKey"), masterKey);
    StringData bearerToken = _oauthService->getBearerToken();

    HostAndPort keyVaultEndpoint = parseEndpoint(azureMasterKey.getKeyVaultEndpoint());

    auto request = UniqueKmsRequest(kms_azure_request_unwrapkey_new(
        keyVaultEndpoint.host().c_str(),
        bearerToken.toString().c_str(),
        azureMasterKey.getKeyName().toString().c_str(),
        azureMasterKey.getKeyVersion().value_or(""_sd).toString().c_str(),
        reinterpret_cast<const uint8_t*>(cdr.data()),
        cdr.length(),
        _config.opts.get()));

    size_t raw_len;
    auto raw_str = makeRequest<AzureDecryptResponse>(request.get(), keyVaultEndpoint, &raw_len);

    return kmsResponseToSecureVector(
        StringData(reinterpret_cast<const char*>(raw_str.get()), raw_len));
}

BSONObj AzureKMSService::encryptDataKeyByBSONObj(ConstDataRange cdr, BSONObj keyId) {
    StringData bearerToken = _oauthService->getBearerToken();
    AzureMasterKey masterKey = AzureMasterKey::parse(IDLParserContext("azureMasterKey"), keyId);

    HostAndPort keyVaultEndpoint = parseEndpoint(masterKey.getKeyVaultEndpoint());

    auto request = UniqueKmsRequest(
        kms_azure_request_wrapkey_new(keyVaultEndpoint.host().c_str(),
                                      bearerToken.toString().c_str(),
                                      masterKey.getKeyName().toString().c_str(),
                                      masterKey.getKeyVersion().value_or(""_sd).toString().c_str(),
                                      reinterpret_cast<const uint8_t*>(cdr.data()),
                                      cdr.length(),
                                      _config.opts.get()));

    size_t raw_len;
    auto raw_str = makeRequest<AzureDecryptResponse>(request.get(), keyVaultEndpoint, &raw_len);

    auto dataKey =
        kmsResponseToVector(StringData(reinterpret_cast<const char*>(raw_str.get()), raw_len));

    AzureMasterKeyAndMaterial keyAndMaterial;
    keyAndMaterial.setKeyMaterial(std::move(dataKey));
    keyAndMaterial.setMasterKey(std::move(masterKey));

    return keyAndMaterial.toBSON();
}

/**
 * Factory for AzureKMSService if user specifies azure config to mongo() JS constructor.
 */
class AzureKMSServiceFactory final : public KMSServiceFactory {
public:
    AzureKMSServiceFactory() = default;
    ~AzureKMSServiceFactory() = default;

    std::unique_ptr<KMSService> create(const BSONObj& config) final {
        auto field = config[KmsProviders::kAzureFieldName];
        if (field.eoo()) {
            return nullptr;
        }

        uassert(5265106,
                "Misconfigured Azure KMS Config: {}"_format(field.toString()),
                field.type() == BSONType::Object);

        auto obj = field.Obj();
        return AzureKMSService::create(AzureKMS::parse(IDLParserContext("root"), obj));
    }
};

}  // namespace

MONGO_INITIALIZER(KMSRegisterAzure)(::mongo::InitializerContext*) {
    kms_message_init();
    KMSServiceController::registerFactory(KMSProviderEnum::azure,
                                          std::make_unique<AzureKMSServiceFactory>());
}

}  // namespace mongo
