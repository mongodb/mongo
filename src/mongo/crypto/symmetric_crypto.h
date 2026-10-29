// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] crypto {

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
 * CTR tunable parameters
 */
constexpr size_t aesCTRIVSize = 16;

/**
 * Encryption mode identifiers
 */
enum class aesMode : uint8_t { cbc, gcm, ctr };

/**
 * Algorithm names which this module recognizes
 */
const std::string aes256CBCName = "AES256-CBC";
const std::string aes256GCMName = "AES256-GCM";
const std::string aes256CTRName = "AES256-CTR";

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
     * Returns the number of bytes written to <out> which will not exceed <out.length()>.
     * Because <in.length()> for this and/or previous calls may not lie on a block boundary,
     * the number of bytes written to <out> may be more or less than <in.length()>.
     */
    virtual StatusWith<std::size_t> update(ConstDataRange in, DataRange out) = 0;

    /**
     * Append Additional AuthenticatedData (AAD) to a GCM encryption stream.
     */
    virtual Status addAuthenticatedData(ConstDataRange authData) = 0;

    /**
     * Finish an encryption by flushing any buffered bytes for a partial cipherblock to <out>.
     * Returns the number of bytes written, not to exceed <out.length()>.
     */
    virtual StatusWith<std::size_t> finalize(DataRange out) = 0;

    /**
     * For aesMode::gcm, writes the GCM tag to <out>.
     * Returns the number of bytes used, not to exceed <out.length()>.
     */
    virtual StatusWith<std::size_t> finalizeTag(DataRange out) = 0;

    /**
     * Create an instance of a SymmetricEncryptor object from the currently available
     * cipher engine (e.g. OpenSSL).
     */
    static StatusWith<std::unique_ptr<SymmetricEncryptor>> create(const SymmetricKey& key,
                                                                  aesMode mode,
                                                                  ConstDataRange iv);
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
     * Returns the number of bytes written to <out> which will not exceed <out.length()>.
     * Because <in.length()> for this and/or previous calls may not lie on a block boundary,
     * the number of bytes written to <out> may be more or less than <in.length()>.
     */
    virtual StatusWith<std::size_t> update(ConstDataRange in, DataRange out) = 0;

    /**
     * For aesMode::gcm, inform the cipher engine of additional authenticated data (AAD).
     */
    virtual Status addAuthenticatedData(ConstDataRange authData) = 0;

    /**
     * For aesMode::gcm, informs the cipher engine of the GCM tag associated with this data stream.
     */
    virtual Status updateTag(ConstDataRange tag) = 0;

    /**
     * Finish an decryption by flushing any buffered bytes for a partial cipherblock to <out>.
     * Returns the number of bytes written, not to exceed <out.length()>.
     */
    virtual StatusWith<std::size_t> finalize(DataRange out) = 0;

    /**
     * Create an instance of a SymmetricDecryptor object from the currently available
     * cipher engine (e.g. OpenSSL).
     */
    static StatusWith<std::unique_ptr<SymmetricDecryptor>> create(const SymmetricKey& key,
                                                                  aesMode mode,
                                                                  ConstDataRange iv);
};

/**
 * Returns a list of cipher modes supported by the cipher engine.
 * e.g. {"AES256-CBC", "AES256-GCM"}
 */
std::set<std::string> getSupportedSymmetricAlgorithms();

/**
 * Generate a quantity of random bytes from the cipher engine.
 */
Status engineRandBytes(DataRange buffer);

}  // namespace crypto
}  // namespace mongo
