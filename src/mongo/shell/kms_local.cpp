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
#include "mongo/base/secure_allocator.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/aead_encryption.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/shell/kms.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/util/base64.h"

namespace mongo {
namespace {

constexpr auto kLocalKms = "local"_sd;

/**
 * Manages Local KMS Information
 */
class LocalKMSService final : public KMSService {
public:
    LocalKMSService(SymmetricKey key) : _key(std::move(key)) {}

    StringData name() const {
        return kLocalKms;
    }

    static std::unique_ptr<KMSService> create(const LocalKMS& config);

    SecureVector<uint8_t> decrypt(ConstDataRange cdr, BSONObj masterKey) final;

    BSONObj encryptDataKeyByString(ConstDataRange cdr, StringData keyId) final;

private:
    std::vector<uint8_t> encrypt(ConstDataRange cdr, StringData kmsKeyId);

private:
    // Key that wraps all KMS encrypted data
    SymmetricKey _key;
};

std::vector<uint8_t> LocalKMSService::encrypt(ConstDataRange cdr, StringData kmsKeyId) {
    std::vector<std::uint8_t> ciphertext(crypto::aeadCipherOutputLength(cdr.length()));

    uassertStatusOK(crypto::aeadEncryptLocalKMS(_key, cdr, {ciphertext}));

    return ciphertext;
}

BSONObj LocalKMSService::encryptDataKeyByString(ConstDataRange cdr, StringData keyId) {
    auto dataKey = encrypt(cdr, keyId);

    LocalMasterKey masterKey;

    LocalMasterKeyAndMaterial keyAndMaterial;
    keyAndMaterial.setKeyMaterial(dataKey);
    keyAndMaterial.setMasterKey(masterKey);

    return keyAndMaterial.toBSON();
}

SecureVector<uint8_t> LocalKMSService::decrypt(ConstDataRange cdr, BSONObj masterKey) {
    SecureVector<uint8_t> plaintext(uassertStatusOK(aeadGetMaximumPlainTextLength(cdr.length())));

    plaintext->resize(uassertStatusOK(crypto::aeadDecryptLocalKMS(_key, cdr, {*plaintext})));

    return plaintext;
}

std::unique_ptr<KMSService> LocalKMSService::create(const LocalKMS& config) {
    uassert(51237,
            str::stream() << "Local KMS key must be 96 bytes, found " << config.getKey().length()
                          << " bytes instead",
            config.getKey().length() == crypto::kFieldLevelEncryptionKeySize);

    SecureVector<uint8_t> aesVector = SecureVector<uint8_t>(
        config.getKey().data(), config.getKey().data() + config.getKey().length());
    SymmetricKey key = SymmetricKey(aesVector, crypto::aesAlgorithm, "local");

    auto localKMS = std::make_unique<LocalKMSService>(std::move(key));

    return localKMS;
}

/**
 * Factory for LocalKMSService if user specifies local config to mongo() JS constructor.
 */
class LocalKMSServiceFactory final : public KMSServiceFactory {
public:
    LocalKMSServiceFactory() = default;
    ~LocalKMSServiceFactory() = default;

    std::unique_ptr<KMSService> create(const BSONObj& config) final {
        auto field = config[KmsProviders::kLocalFieldName];
        if (field.eoo()) {
            return nullptr;
        }

        auto obj = field.Obj();
        return LocalKMSService::create(LocalKMS::parse(IDLParserContext("root"), obj));
    }
};

}  // namespace

MONGO_INITIALIZER(LocalKMSRegister)(::mongo::InitializerContext* context) {
    KMSServiceController::registerFactory(KMSProviderEnum::local,
                                          std::make_unique<LocalKMSServiceFactory>());
}

}  // namespace mongo
