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

#include "mongo/platform/basic.h"

#include "mongo/crypto/aead_encryption.h"

#include "mongo/base/data_view.h"
#include "mongo/crypto/sha512_block.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/util/secure_compare_memory.h"

namespace mongo {
namespace crypto {

namespace {
constexpr size_t kHmacOutSize = 32;
constexpr size_t kIVSize = 16;

// AssociatedData can be 2^24 bytes but since there needs to be room for the ciphertext in the
// object, a value of 1<<16 was decided to cap the maximum size of AssociatedData.
constexpr int kMaxAssociatedDataLength = 1 << 16;

size_t aesCBCCipherOutputLength(size_t plainTextLen) {
    return aesBlockSize * (1 + plainTextLen / aesBlockSize);
}

std::pair<size_t, size_t> aesCBCExpectedPlaintextLen(size_t cipherTextLength) {
    return {cipherTextLength - aesCBCIVSize - aesBlockSize, cipherTextLength - aesCBCIVSize};
}

void aeadGenerateIV(const SymmetricKey* key, uint8_t* buffer, size_t bufferLen) {
    if (bufferLen < aesCBCIVSize) {
        fassert(51235, "IV buffer is too small for selected mode");
    }

    auto status = engineRandBytes(buffer, aesCBCIVSize);
    if (!status.isOK()) {
        fassert(51236, status);
    }
}

Status _aesEncrypt(const SymmetricKey& key,
                   const std::uint8_t* in,
                   std::size_t inLen,
                   std::uint8_t* out,
                   std::size_t outLen,
                   std::size_t* resultLen,
                   bool ivProvided) try {

    if (!ivProvided) {
        aeadGenerateIV(&key, out, aesCBCIVSize);
    }

    auto encryptor =
        uassertStatusOK(SymmetricEncryptor::create(key, aesMode::cbc, out, aesCBCIVSize));

    const size_t dataSize = outLen - aesCBCIVSize;
    uint8_t* data = out + aesCBCIVSize;

    const auto updateLen = uassertStatusOK(encryptor->update(in, inLen, data, dataSize));
    const auto finalLen =
        uassertStatusOK(encryptor->finalize(data + updateLen, dataSize - updateLen));
    const auto len = updateLen + finalLen;

    // Some cipher modes, such as GCM, will know in advance exactly how large their ciphertexts will
    // be. Others, like CBC, will have an upper bound. When this is true, we must allocate enough
    // memory to store the worst case. We must then set the actual size of the ciphertext so that
    // the buffer it has been written to may be serialized.
    invariant(len <= dataSize);
    *resultLen = aesCBCIVSize + len;

    // Check the returned length, including block size padding
    if (len != aesCBCCipherOutputLength(inLen)) {
        return {ErrorCodes::BadValue,
                str::stream() << "Encrypt error, expected cipher text of length "
                              << aesCBCCipherOutputLength(inLen)
                              << " but found "
                              << len};
    }

    return Status::OK();
} catch (const AssertionException& ex) {
    return ex.toStatus();
}

Status _aesDecrypt(const SymmetricKey& key,
                   ConstDataRange in,
                   std::uint8_t* out,
                   std::size_t outLen,
                   std::size_t* resultLen) try {
    // Check the plaintext buffer can fit the product of decryption
    auto[lowerBound, upperBound] = aesCBCExpectedPlaintextLen(in.length());
    if (upperBound > outLen) {
        return {ErrorCodes::BadValue,
                str::stream() << "Cleartext buffer of size " << outLen
                              << " too small for output which can be as large as "
                              << upperBound
                              << "]"};
    }

    const uint8_t* dataPtr = reinterpret_cast<const std::uint8_t*>(in.data());

    auto decryptor =
        uassertStatusOK(SymmetricDecryptor::create(key, aesMode::cbc, dataPtr, aesCBCIVSize));

    const size_t dataSize = in.length() - aesCBCIVSize;
    const uint8_t* data = dataPtr + aesCBCIVSize;

    const auto updateLen = uassertStatusOK(decryptor->update(data, dataSize, out, outLen));

    const auto finalLen = uassertStatusOK(decryptor->finalize(out + updateLen, outLen - updateLen));

    *resultLen = updateLen + finalLen;
    invariant(*resultLen <= outLen);

    // Check the returned length, excluding headers block padding
    if (*resultLen < lowerBound || *resultLen > upperBound) {
        return {ErrorCodes::BadValue,
                str::stream() << "Decrypt error, expected clear text length in interval"
                              << "["
                              << lowerBound
                              << ","
                              << upperBound
                              << "]"
                              << "but found "
                              << *resultLen};
    }

    return Status::OK();
} catch (const AssertionException& ex) {
    return ex.toStatus();
}

}  // namespace

size_t aeadCipherOutputLength(size_t plainTextLen) {
    // To calculate the size of the byte, we divide by the byte size and add 2 for padding
    // (1 for the attached IV, and 1 for the extra padding). The algorithm will add padding even
    // if the len is a multiple of the byte size, so if the len divides cleanly it will be
    // 32 bytes longer than the original, which is 16 bytes as padding and 16 bytes for the
    // IV. For things that don't divide cleanly, the cast takes care of floor dividing so it will
    // be 0 < x < 16 bytes added for padding and 16 bytes added for the IV.
    size_t aesOutLen = aesBlockSize * (plainTextLen / aesBlockSize + 2);
    return aesOutLen + kHmacOutSize;
}

Status aeadEncrypt(const SymmetricKey& key,
                   const uint8_t* in,
                   const size_t inLen,
                   const uint8_t* associatedData,
                   const uint64_t associatedDataLen,
                   uint8_t* out,
                   size_t outLen) {

    if (associatedDataLen >= kMaxAssociatedDataLength) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "AssociatedData for encryption is too large. Cannot be larger than "
                          << kMaxAssociatedDataLength
                          << " bytes.");
    }

    // According to the rfc on AES encryption, the associatedDataLength is defined as the
    // number of bits in associatedData in BigEndian format. This is what the code segment
    // below describes.
    // RFC: (https://tools.ietf.org/html/draft-mcgrew-aead-aes-cbc-hmac-sha2-01#section-2.1)
    std::array<uint8_t, sizeof(uint64_t)> dataLenBitsEncodedStorage;
    DataRange dataLenBitsEncoded(dataLenBitsEncodedStorage);
    dataLenBitsEncoded.write<BigEndian<uint64_t>>(associatedDataLen * 8);

    ConstDataRange aeadKey(key.getKey(), kAeadAesHmacKeySize);

    if (key.getKeySize() != kFieldLevelEncryptionKeySize) {
        return Status(ErrorCodes::BadValue, "Invalid key size.");
    }

    if (in == nullptr || !in) {
        return Status(ErrorCodes::BadValue, "Invalid AEAD plaintext input.");
    }

    if (key.getAlgorithm() != aesAlgorithm) {
        return Status(ErrorCodes::BadValue, "Invalid algorithm for key.");
    }

    ConstDataRange hmacCDR(nullptr, 0);
    SHA512Block hmacOutput;
    if (associatedData != nullptr &&
        static_cast<int>(associatedData[0]) ==
            FleAlgorithmInt_serializer(FleAlgorithmInt::kDeterministic)) {
        const uint8_t* ivKey = key.getKey() + kAeadAesHmacKeySize;
        hmacOutput = SHA512Block::computeHmac(ivKey,
                                              sym256KeySize,
                                              {ConstDataRange(associatedData, associatedDataLen),
                                               dataLenBitsEncoded,
                                               ConstDataRange(in, inLen)});

        static_assert(SHA512Block::kHashLength >= kIVSize,
                      "Invalid AEAD parameters. Generated IV too short.");

        hmacCDR = ConstDataRange(hmacOutput.data(), kIVSize);
    }
    return aeadEncryptWithIV(aeadKey,
                             in,
                             inLen,
                             reinterpret_cast<const uint8_t*>(hmacCDR.data()),
                             hmacCDR.length(),
                             associatedData,
                             associatedDataLen,
                             dataLenBitsEncoded,
                             out,
                             outLen);
}

Status aeadEncryptWithIV(ConstDataRange key,
                         const uint8_t* in,
                         const size_t inLen,
                         const uint8_t* iv,
                         const size_t ivLen,
                         const uint8_t* associatedData,
                         const uint64_t associatedDataLen,
                         ConstDataRange dataLenBitsEncoded,
                         uint8_t* out,
                         size_t outLen) {
    if (key.length() != kAeadAesHmacKeySize) {
        return Status(ErrorCodes::BadValue, "Invalid key size.");
    }

    if (!(in && out)) {
        return Status(ErrorCodes::BadValue, "Invalid AEAD parameters.");
    }

    if (outLen != aeadCipherOutputLength(inLen)) {
        return Status(ErrorCodes::BadValue, "Invalid output buffer size.");
    }

    if (associatedDataLen >= kMaxAssociatedDataLength) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "AssociatedData for encryption is too large. Cannot be larger than "
                          << kMaxAssociatedDataLength
                          << " bytes.");
    }

    const uint8_t* macKey = reinterpret_cast<const uint8_t*>(key.data());
    const uint8_t* encKey = reinterpret_cast<const uint8_t*>(key.data() + sym256KeySize);

    size_t aesOutLen = outLen - kHmacOutSize;

    size_t cipherTextLen = 0;

    SymmetricKey symEncKey(encKey, sym256KeySize, aesAlgorithm, "aesKey", 1);

    bool ivProvided = false;
    if (ivLen != 0) {
        invariant(ivLen == 16);
        std::copy(iv, iv + ivLen, out);
        ivProvided = true;
    }

    auto sEncrypt = _aesEncrypt(symEncKey, in, inLen, out, aesOutLen, &cipherTextLen, ivProvided);

    if (!sEncrypt.isOK()) {
        return sEncrypt;
    }

    SHA512Block hmacOutput =
        SHA512Block::computeHmac(macKey,
                                 sym256KeySize,
                                 {ConstDataRange(associatedData, associatedDataLen),
                                  ConstDataRange(out, cipherTextLen),
                                  dataLenBitsEncoded});

    std::copy(hmacOutput.data(), hmacOutput.data() + kHmacOutSize, out + cipherTextLen);
    return Status::OK();
}

Status aeadDecrypt(const SymmetricKey& key,
                   const uint8_t* cipherText,
                   const size_t cipherLen,
                   const uint8_t* associatedData,
                   const uint64_t associatedDataLen,
                   uint8_t* out,
                   size_t* outLen) {
    if (key.getKeySize() < kAeadAesHmacKeySize) {
        return Status(ErrorCodes::BadValue, "Invalid key size.");
    }

    if (!(cipherText && out)) {
        return Status(ErrorCodes::BadValue, "Invalid AEAD parameters.");
    }

    if ((*outLen) != cipherLen) {
        return Status(ErrorCodes::BadValue, "Output buffer must be as long as the cipherText.");
    }

    if (associatedDataLen >= kMaxAssociatedDataLength) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "AssociatedData for encryption is too large. Cannot be larger than "
                          << kMaxAssociatedDataLength
                          << " bytes.");
    }

    const uint8_t* macKey = key.getKey();
    const uint8_t* encKey = key.getKey() + sym256KeySize;

    if (cipherLen < kHmacOutSize) {
        return Status(ErrorCodes::BadValue, "Ciphertext is not long enough.");
    }
    size_t aesLen = cipherLen - kHmacOutSize;

    // According to the rfc on AES encryption, the associatedDataLength is defined as the
    // number of bits in associatedData in BigEndian format. This is what the code segment
    // below describes.
    std::array<uint8_t, sizeof(uint64_t)> dataLenBitsEncodedStorage;
    DataRange dataLenBitsEncoded(dataLenBitsEncodedStorage);
    dataLenBitsEncoded.write<BigEndian<uint64_t>>(associatedDataLen * 8);

    SHA512Block hmacOutput =
        SHA512Block::computeHmac(macKey,
                                 sym256KeySize,
                                 {ConstDataRange(associatedData, associatedDataLen),
                                  ConstDataRange(cipherText, aesLen),
                                  dataLenBitsEncoded});

    if (consttimeMemEqual(reinterpret_cast<const unsigned char*>(hmacOutput.data()),
                          reinterpret_cast<const unsigned char*>(cipherText + aesLen),
                          kHmacOutSize) == false) {
        return Status(ErrorCodes::BadValue, "HMAC data authentication failed.");
    }

    SymmetricKey symEncKey(encKey, sym256KeySize, aesAlgorithm, key.getKeyId(), 1);

    auto sDecrypt = _aesDecrypt(symEncKey, ConstDataRange(cipherText, aesLen), out, aesLen, outLen);
    if (!sDecrypt.isOK()) {
        return sDecrypt;
    }

    return Status::OK();
}

}  // namespace crypto
}  // namespace mongo
