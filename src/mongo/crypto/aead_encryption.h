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

#include "mongo/crypto/fle_data_frames.h"
#include "mongo/shell/kms_gen.h"

#include "mongo/base/data_view.h"
#include "mongo/base/status.h"
#include "mongo/crypto/symmetric_key.h"

namespace mongo {
namespace crypto {

/**
 * Constants used in the AEAD function
 */

constexpr size_t kFieldLevelEncryptionKeySize = 96;
constexpr size_t kAeadAesHmacKeySize = 64;

/**
 * Returns the length of the ciphertext output given the plaintext length. Only for AEAD.
 */
size_t aeadCipherOutputLength(size_t plainTextLen);

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
Status aeadEncryptLocalKMS(const SymmetricKey& key,
                           const ConstDataRange in,
                           uint8_t* out,
                           size_t outLen);
/**
 * Internal calls for the aeadEncryption algorithm. Only used for testing.
 */
Status aeadEncryptWithIV(ConstDataRange key,
                         const uint8_t* in,
                         const size_t inLen,
                         const uint8_t* iv,
                         const size_t ivLen,
                         const uint8_t* associatedData,
                         const uint64_t associatedDataLen,
                         ConstDataRange dataLenBitsEncodedStorage,
                         uint8_t* out,
                         size_t outLen);

/**
 * Internal call for the aeadDecryption algorithm. Only used for testing.
 */
Status aeadDecrypt(const SymmetricKey& key,
                   ConstDataRange ciphertext,
                   const uint8_t* associatedData,
                   const uint64_t associatedDataLen,
                   uint8_t* out,
                   size_t* outLen);

/**
 * Decrypts the cipherText using AEAD_AES_256_CBC_HMAC_SHA_512 decryption. Writes output
 * to out.
 */
Status aeadDecryptLocalKMS(const SymmetricKey& key,
                           const ConstDataRange cipher,
                           uint8_t* out,
                           size_t* outLen);

}  // namespace crypto
}  // namespace mongo
