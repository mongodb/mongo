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
#include <cstdint>
#include <set>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/crypto/symmetric_key.h"

namespace mongo {
namespace crypto {

/**
 * Encryption algorithm identifiers and block sizes
 */
constexpr uint8_t aesAlgorithm = 0x1;

/**
 * Block and key sizes
 */
constexpr size_t aesBlockSize = 16;
constexpr size_t sym256KeySize = 32;

/**
 * Min and max symmetric key lengths
 */
constexpr size_t minKeySize = 16;
constexpr size_t maxKeySize = 32;

/**
 * CBC fixed constants
 */
constexpr size_t aesCBCIVSize = aesBlockSize;

/**
 * GCM tunable parameters
 */
constexpr size_t aesGCMTagSize = 12;
constexpr size_t aesGCMIVSize = 12;

/**
 * Encryption mode identifiers
 */
enum class aesMode : uint8_t { cbc, gcm };

/**
 * Algorithm names which this module recognizes
 */
const std::string aes256CBCName = "AES256-CBC";
const std::string aes256GCMName = "AES256-GCM";

aesMode getCipherModeFromString(const std::string& mode);
std::string getStringFromCipherMode(aesMode);

/**
 * Generates a new, random, symmetric key for use with AES.
 */
SymmetricKey aesGenerate(size_t keySize, SymmetricKeyId keyId);

/* Platform specific engines should implement these. */

/**
 * Interface to a symmetric cryptography engine.
 * For use with encrypting payloads.
 */
class SymmetricEncryptor {
public:
    virtual ~SymmetricEncryptor() = default;

    /**
     * Process a chunk of data from <in> and store the ciphertext in <out>.
     * Returns the number of bytes written to <out> which will not exceed <outLen>.
     * Because <inLen> for this and/or previous calls may not lie on a block boundary,
     * the number of bytes written to <out> may be more or less than <inLen>.
     */
    virtual StatusWith<size_t> update(const uint8_t* in,
                                      size_t inLen,
                                      uint8_t* out,
                                      size_t outLen) = 0;

    /**
     * Append Additional AuthenticatedData (AAD) to a GCM encryption stream.
     */
    virtual Status addAuthenticatedData(const uint8_t* in, size_t inLen) = 0;

    /**
     * Finish an encryption by flushing any buffered bytes for a partial cipherblock to <out>.
     * Returns the number of bytes written, not to exceed <outLen>.
     */
    virtual StatusWith<size_t> finalize(uint8_t* out, size_t outLen) = 0;

    /**
     * For aesMode::gcm, writes the GCM tag to <out>.
     * Returns the number of bytes used, not to exceed <outLen>.
     */
    virtual StatusWith<size_t> finalizeTag(uint8_t* out, size_t outLen) = 0;

    /**
     * Create an instance of a SymmetricEncryptor object from the currently available
     * cipher engine (e.g. OpenSSL).
     */
    static StatusWith<std::unique_ptr<SymmetricEncryptor>> create(const SymmetricKey& key,
                                                                  aesMode mode,
                                                                  const uint8_t* iv,
                                                                  size_t inLen);
};

/**
 * Interface to a symmetric cryptography engine.
 * For use with encrypting payloads.
 */
class SymmetricDecryptor {
public:
    virtual ~SymmetricDecryptor() = default;

    /**
     * Process a chunk of data from <in> and store the decrypted text in <out>.
     * Returns the number of bytes written to <out> which will not exceed <outLen>.
     * Because <inLen> for this and/or previous calls may not lie on a block boundary,
     * the number of bytes written to <out> may be more or less than <inLen>.
     */
    virtual StatusWith<size_t> update(const uint8_t* in,
                                      size_t inLen,
                                      uint8_t* out,
                                      size_t outLen) = 0;

    /**
     * For aesMode::gcm, inform the cipher engine of additional authenticated data (AAD).
     */
    virtual Status addAuthenticatedData(const uint8_t* in, size_t inLen) = 0;

    /**
     * For aesMode::gcm, informs the cipher engine of the GCM tag associated with this data stream.
     */
    virtual Status updateTag(const uint8_t* tag, size_t tagLen) = 0;

    /**
     * Finish an decryption by flushing any buffered bytes for a partial cipherblock to <out>.
     * Returns the number of bytes written, not to exceed <outLen>.
     */
    virtual StatusWith<size_t> finalize(uint8_t* out, size_t outLen) = 0;

    /**
     * Create an instance of a SymmetricDecryptor object from the currently available
     * cipher engine (e.g. OpenSSL).
     */
    static StatusWith<std::unique_ptr<SymmetricDecryptor>> create(const SymmetricKey& key,
                                                                  aesMode mode,
                                                                  const uint8_t* iv,
                                                                  size_t ivLen);
};

/**
 * Returns a list of cipher modes supported by the cipher engine.
 * e.g. {"AES256-CBC", "AES256-GCM"}
 */
std::set<std::string> getSupportedSymmetricAlgorithms();

/**
 * Generate a quantity of random bytes from the cipher engine.
 */
Status engineRandBytes(uint8_t* buffer, size_t len);

}  // namespace crypto
}  // namespace mongo
