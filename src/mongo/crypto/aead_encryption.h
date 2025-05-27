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

#include "mongo/base/data_range.h"
#include "mongo/base/data_view.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/crypto/fle_data_frames.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/crypto/symmetric_key.h"

#include <cstddef>
#include <cstdint>

namespace mongo {
namespace crypto {

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
