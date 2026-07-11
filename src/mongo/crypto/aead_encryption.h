// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/data_view.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/crypto/fle_data_frames.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] crypto {

/**
 * Constants used in the AEAD function
 */

constexpr size_t kFieldLevelEncryptionKeySize = 96;
constexpr size_t kFieldLevelEncryption2KeySize = 64;
constexpr size_t kAeadAesHmacKeySize = 64;

/**
 * Returns the length of the ciphertext output given the plaintext length. Only for AEAD.
 */
size_t aeadCipherOutputLength(size_t plainTextLen);

/**
 * Returns the length of the ciphertext output given the plaintext length. Only for FLE2 AEAD.
 */
size_t fle2AeadCipherOutputLength(size_t plainTextLen, aesMode mode);

/**
 * Returns the length of the ciphertext output given the plaintext length. Only for FLE2.
 */
size_t fle2CipherOutputLength(size_t plainTextLen);

/**
 * Encrypts a dataframe object following the AEAD_AES_256_CBC_HMAC_SHA_512 encryption
 * algorithm. Used for field level encryption.
 */
Status aeadEncryptDataFrame(FLEEncryptionFrame& dataframe);

/**
 * Decrypts a dataframe object following the AEAD_AES_256_CBC_HMAC_SHA_512 decryption
 * algorithm. Used for field level encryption.
 */
Status aeadDecryptDataFrame(FLEDecryptionFrame& dataframe);

/**
 * Uses AEAD_AES_256_CBC_HMAC_SHA_512 encryption to encrypt a local datakey.
 * Writes output to out.
 */
Status aeadEncryptLocalKMS(const SymmetricKey& key, ConstDataRange in, DataRange out);

/**
 * Internal calls for the aeadEncryption algorithm. Only used for testing.
 */
Status aeadEncryptWithIV(ConstDataRange key,
                         ConstDataRange in,
                         ConstDataRange iv,
                         ConstDataRange associatedData,
                         ConstDataRange dataLenBitsEncodedStorage,
                         DataRange out);

/**
 * Internal call for FLE2 aeadEncryption algorithm.
 * Note: parameter "iv" is not required and only used for unit testing
 */
Status fle2AeadEncrypt(ConstDataRange key,
                       ConstDataRange in,
                       ConstDataRange iv,
                       ConstDataRange associatedData,
                       DataRange out,
                       aesMode mode);

/**
 * Internal call for FLE2 encryption algorithm.
 * Note: parameter "iv" is not required and only used for unit testing
 */
Status fle2Encrypt(ConstDataRange key, ConstDataRange in, ConstDataRange iv, DataRange out);

/**
 * Internal call for the aeadDecryption algorithm. Only used for testing.
 */
StatusWith<std::size_t> aeadDecrypt(const SymmetricKey& key,
                                    ConstDataRange ciphertext,
                                    ConstDataRange associatedData,
                                    DataRange out);

/**
 * Internal call for FLE2 aeadDecryption algorithm.
 */
StatusWith<std::size_t> fle2AeadDecrypt(ConstDataRange key,
                                        ConstDataRange in,
                                        ConstDataRange associatedData,
                                        DataRange out,
                                        aesMode mode);

/**
 * Internal call for FLE2 decryption algorithm.
 */
StatusWith<std::size_t> fle2Decrypt(ConstDataRange key, ConstDataRange in, DataRange out);

/**
 * Decrypts the cipherText using AEAD_AES_256_CBC_HMAC_SHA_512 decryption. Writes output
 * to out.
 */
StatusWith<std::size_t> aeadDecryptLocalKMS(const SymmetricKey& key,
                                            ConstDataRange cipher,
                                            DataRange out);

}  // namespace crypto
}  // namespace mongo
