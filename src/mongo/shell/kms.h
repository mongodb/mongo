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

#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/net/hostandport.h"

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
     * Encrypt a plaintext with the specified key and return a encrypted blob.
     */
    virtual std::vector<uint8_t> encrypt(ConstDataRange cdr, StringData keyId) = 0;

    /**
     * Decrypt an encrypted blob and return the plaintext.
     */
    virtual SecureVector<uint8_t> decrypt(ConstDataRange cdr, BSONObj masterKey) = 0;

    /**
     * Encrypt a data key with the specified key and return a BSONObj that describes what needs to
     * be store in the key vault.
     *
     * {
     *   keyMaterial : "<ciphertext>""
     *   masterKey : {
     *     provider : "<provider_name>"
     *     ... <provider specific fields>
     *   }
     * }
     */
    virtual BSONObj encryptDataKey(ConstDataRange cdr, StringData keyId) = 0;
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
    static std::unique_ptr<KMSService> createFromClient(StringData kmsProvider,
                                                        const BSONObj& config);

    /**
     * Creates a KMS Service with the given mongo constructor options and key vault record.
     */
    static std::unique_ptr<KMSService> createFromDisk(const BSONObj& config,
                                                      const BSONObj& kmsProvider);

private:
    static stdx::unordered_map<KMSProviderEnum, std::unique_ptr<KMSServiceFactory>> _factories;
};

/**
 * Parse a basic url of "https://host:port" to a HostAndPort.
 *
 * Does not support URL encoding or anything else.
 */
HostAndPort parseUrl(StringData url);

}  // namespace mongo
