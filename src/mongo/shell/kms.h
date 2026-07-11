// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace mongo {

/**
 * KMSService
 *
 * Represents a Key Management Service. May be a local file KMS or remote.
 *
 * Responsible for securely encrypting and decrypting data. The encrypted data is treated as a
 * blockbox by callers.
 */
class KMSService {
public:
    virtual ~KMSService() = default;

    /**
     * Return name of KMS service for use in error messages.
     */
    virtual std::string_view name() const = 0;

    /**
     * Decrypt an encrypted blob and return the plaintext.
     */
    virtual SecureVector<uint8_t> decrypt(ConstDataRange cdr, BSONObj masterKey) = 0;

    /**
     * Encrypt a data key with the specified key string and return a BSONObj that describes what
     * needs to be store in the key vault.
     *
     * {
     *   keyMaterial : "<ciphertext>""
     *   masterKey : {
     *     provider : "<provider_name>"
     *     ... <provider specific fields>
     *   }
     * }
     */
    virtual BSONObj encryptDataKeyByString(ConstDataRange cdr, std::string_view keyId);

    /**
     * Encrypt a data key with the specified key object and return a BSONObj that describes what
     * needs to be store in the key vault.
     */
    virtual BSONObj encryptDataKeyByBSONObj(ConstDataRange cdr, BSONObj keyId);

    virtual SymmetricKey& getMasterKey() = 0;
};

/**
 * KMSService Factory
 *
 * Provides static registration of KMSService.
 */
class KMSServiceFactory {
public:
    virtual ~KMSServiceFactory() = default;

    /**
     * Create an instance of the KMS service
     */
    virtual std::unique_ptr<KMSService> create(const BSONObj& config) = 0;
};

/**
 * KMSService Controller
 *
 * Provides static registration of KMSServiceFactory
 */
class KMSServiceController {
public:
    /**
     * Create an instance of the KMS service
     */
    static void registerFactory(KMSProviderEnum provider,
                                std::unique_ptr<KMSServiceFactory> factory);


    /**
     * Creates a KMS Service for the specified provider with the config.
     */
    static std::unique_ptr<KMSService> createFromClient(std::string_view kmsProvider,
                                                        const BSONObj& config);

    /**
     * Creates a KMS Service with the given mongo constructor options and key vault record.
     */
    static std::unique_ptr<KMSService> createFromDisk(const BSONObj& config,
                                                      const BSONObj& kmsProvider);

private:
    static stdx::unordered_map<KMSProviderEnum, std::unique_ptr<KMSServiceFactory>> _factories;
};

}  // namespace mongo
