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


#include <kms_message/kms_message.h>

#include <stdlib.h>

#include "mongo/base/init.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/json.h"
#include "mongo/shell/kms.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/shell/kms_network.h"
#include "mongo/util/base64.h"
#include "mongo/util/kms_message_support.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/text.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
namespace {

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

constexpr auto kAwsKms = "aws"_sd;

/**
 * Manages SSL information and config for how to talk to AWS KMS.
 */
class AWSKMSService final : public KMSService {
public:
    AWSKMSService() = default;

    StringData name() const {
        return kAwsKms;
    }

    static std::unique_ptr<KMSService> create(const AwsKMS& config);

    SecureVector<uint8_t> decrypt(ConstDataRange cdr, BSONObj masterKey) final;

    BSONObj encryptDataKeyByString(ConstDataRange cdr, StringData keyId) final;

private:
    std::vector<uint8_t> encrypt(ConstDataRange cdr, StringData kmsKeyId);

    void initRequest(kms_request_t* request, StringData host, StringData region);

private:
    // SSL Manager
    std::shared_ptr<SSLManagerInterface> _sslManager;

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

    KMSNetworkConnection connection(_sslManager.get());
    auto response = connection.makeOneRequest(_server, ConstDataRange(buffer.get(), buffer_len));

    auto body = kms_response_get_body(response.get(), nullptr);

    BSONObj obj = fromjson(body);

    auto field = obj["__type"];

    if (!field.eoo()) {
        AwsKMSError awsResponse;
        try {
            awsResponse = AwsKMSError::parse(IDLParserContext("awsEncryptError"), obj);
        } catch (DBException& dbe) {
            uasserted(51274,
                      str::stream() << "AWS KMS failed to parse error message: " << dbe.toString()
                                    << ", Response : " << obj);
        }

        uasserted(51224,
                  str::stream() << "AWS KMS failed to encrypt: " << awsResponse.getType() << " : "
                                << awsResponse.getMessage());
    }

    auto awsResponse = AwsEncryptResponse::parse(IDLParserContext("awsEncryptResponse"), obj);

    auto blobStr = base64::decode(awsResponse.getCiphertextBlob().toString());

    return kmsResponseToVector(blobStr);
}

BSONObj AWSKMSService::encryptDataKeyByString(ConstDataRange cdr, StringData keyId) {
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
    auto awsMasterKey = AwsMasterKey::parse(IDLParserContext("awsMasterKey"), masterKey);

    auto request = UniqueKmsRequest(kms_decrypt_request_new(
        reinterpret_cast<const uint8_t*>(cdr.data()), cdr.length(), nullptr));

    if (_server.empty()) {
        _server = getDefaultHost(awsMasterKey.getRegion());
    }

    initRequest(request.get(), _server.host(), awsMasterKey.getRegion());

    auto buffer = UniqueKmsCharBuffer(kms_request_get_signed(request.get()));
    auto buffer_len = strlen(buffer.get());
    KMSNetworkConnection connection(_sslManager.get());
    auto response = connection.makeOneRequest(_server, ConstDataRange(buffer.get(), buffer_len));

    auto body = kms_response_get_body(response.get(), nullptr);

    BSONObj obj = fromjson(body);

    auto field = obj["__type"];

    if (!field.eoo()) {
        AwsKMSError awsResponse;
        try {
            awsResponse = AwsKMSError::parse(IDLParserContext("awsDecryptError"), obj);
        } catch (DBException& dbe) {
            uasserted(51275,
                      str::stream() << "AWS KMS failed to parse error message: " << dbe.toString()
                                    << ", Response : " << obj);
        }

        uasserted(51225,
                  str::stream() << "AWS KMS failed to decrypt: " << awsResponse.getType() << " : "
                                << awsResponse.getMessage());
    }

    auto awsResponse = AwsDecryptResponse::parse(IDLParserContext("awsDecryptResponse"), obj);

    auto blobStr = base64::decode(awsResponse.getPlaintext().toString());

    return kmsResponseToSecureVector(blobStr);
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
    getSSLParamsForNetworkKMS(&params);

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
        return AWSKMSService::create(AwsKMS::parse(IDLParserContext("root"), obj));
    }
};

}  // namespace

MONGO_INITIALIZER(KMSRegisterAWS)(::mongo::InitializerContext*) {
    kms_message_init();
    KMSServiceController::registerFactory(KMSProviderEnum::aws,
                                          std::make_unique<AWSKMSServiceFactory>());
}

}  // namespace mongo
