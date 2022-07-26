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
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/util/uuid.h"

namespace mongo {
constexpr int kAssociatedDataLength = 18;
constexpr size_t kHmacOutSize = 32;

/**
 * Returns the length of the plaintext output given the ciphertext length. Only for AEAD.
 */
inline StatusWith<size_t> aeadGetMaximumPlainTextLength(size_t cipherTextLen) {
    if (cipherTextLen > (crypto::aesCBCIVSize + kHmacOutSize)) {
        return cipherTextLen - crypto::aesCBCIVSize - kHmacOutSize;
    }

    return Status(ErrorCodes::BadValue, "Invalid cipher text length");
}

/**
 * Returns the length of the plaintext output given the ciphertext length. Only for FLE2 AEAD.
 */
inline StatusWith<size_t> fle2AeadGetPlainTextLength(size_t cipherTextLen) {
    if (cipherTextLen > (crypto::aesCTRIVSize + kHmacOutSize)) {
        return cipherTextLen - crypto::aesCTRIVSize - kHmacOutSize;
    }

    return Status(ErrorCodes::BadValue, "Invalid cipher text length");
}

/**
 * Returns the length of the plaintext output given the ciphertext length. Only for FLE2.
 */
inline StatusWith<size_t> fle2GetPlainTextLength(size_t cipherTextLen) {
    if (cipherTextLen > (crypto::aesCTRIVSize)) {
        return cipherTextLen - crypto::aesCTRIVSize;
    }

    return Status(ErrorCodes::BadValue, "Invalid cipher text length");
}

/**
 * This class is a helper for encryption. It holds a ConstDataRange over the
 * plaintext to be encrypted and owns a buffer where the BinData subtype 6 is
 * written out to. The encrypt function can only be called after the constructor
 * to initialize the plaintext, the associated data, and the key has been called.
 */
class FLEEncryptionFrame {

public:
    FLEEncryptionFrame(std::shared_ptr<SymmetricKey> key,
                       FleAlgorithmInt algorithm,
                       UUID uuid,
                       BSONType type,
                       ConstDataRange plaintext,
                       size_t cipherLength)
        : _key(std::move(key)), _plaintext(plaintext) {
        // As per the description of the encryption algorithm for FLE, the
        // associated data is constructed of the following -
        // associatedData[0] = the FleAlgorithmEnum
        //      - either a 1 or a 2 depending on whether the iv is provided.
        // associatedData[1-16] = the uuid in bytes
        // associatedData[17] = the bson type
        if (BSONType::BinData == type) {
            BinDataType subType = BSONElement::binDataType(plaintext.data(), plaintext.length());
            uassert(6409402,
                    "Encrypting already encrypted data prohibited",
                    BinDataType::Encrypt != subType);
        }

        _data.resize(kAssociatedDataLength + cipherLength);
        _data[0] = FleAlgorithmInt_serializer(algorithm);
        auto uuidCDR = uuid.toCDR();
        invariant(uuidCDR.length() == 16);
        std::copy(
            uuidCDR.data<uint8_t>(), uuidCDR.data<uint8_t>() + uuidCDR.length(), _data.begin() + 1);
        _data[17] = static_cast<uint8_t>(type);
    }

    FLEEncryptionFrame() : _plaintext(ConstDataRange(nullptr, 0)){};

    ConstDataRange get() const& {
        return ConstDataRange(_data);
    }

    std::shared_ptr<SymmetricKey> getKey() const {
        return _key;
    }

    ConstDataRange getPlaintext() const {
        return _plaintext;
    }

    ConstDataRange getAssociatedData() const {
        return ConstDataRange(_data.data(), kAssociatedDataLength);
    }

    uint8_t* getCiphertextMutable() & {
        invariant(_data.size() > kAssociatedDataLength);
        return _data.data() + kAssociatedDataLength;
    }

    uint8_t* getCiphertextMutable() && = delete;

    FleAlgorithmInt getFLEAlgorithmType() {
        return FleAlgorithmInt_parse(IDLParserContext("root"), _data[0]);
    }

    size_t getDataLength() const {
        return _data.size() - kAssociatedDataLength;
    }

private:
    std::shared_ptr<SymmetricKey> _key;
    std::vector<uint8_t> _data;
    ConstDataRange _plaintext;
};

/**
 * This is a helper class for decryption. It has a ConstDataRange over a
 * vector owned by the instantiator of this class and allows
 * containing the plaintext object after it has been decrypted.
 */
class FLEDecryptionFrame {
public:
    FLEDecryptionFrame(ConstDataRange data) : _data(data) {
        uassert(ErrorCodes::BadValue,
                "Ciphertext blob too small",
                _data.length() > kAssociatedDataLength);
        uassert(ErrorCodes::BadValue,
                "Ciphertext blob algorithm unknown",
                (getFLEAlgorithmType() == FleAlgorithmInt::kDeterministic ||
                 getFLEAlgorithmType() == FleAlgorithmInt::kRandom));

        _plaintext.resize(
            uassertStatusOK(aeadGetMaximumPlainTextLength(data.length() - kAssociatedDataLength)));
    }

    void setKey(std::shared_ptr<SymmetricKey> key) {
        _key = key;
    }

    std::shared_ptr<SymmetricKey> getKey() {
        return _key;
    }

    ConstDataRange getAssociatedData() const {
        return ConstDataRange(_data.data(), kAssociatedDataLength);
    }

    UUID getUUID() const {
        auto uuid = UUID::fromCDR(ConstDataRange(_data.data() + 1, UUID::kNumBytes));
        return uuid;
    }

    ConstDataRange getCiphertext() const {
        return ConstDataRange(_data.data() + kAssociatedDataLength, getDataLength());
    }

    ConstDataRange getPlaintext() const {
        return ConstDataRange(_plaintext);
    }

    std::vector<uint8_t>& getPlaintextMutable() {
        return _plaintext;
    }

    uint8_t getBSONType() {
        return *(_data.data<uint8_t>() + 17);
    }

private:
    FleAlgorithmInt getFLEAlgorithmType() const {
        return FleAlgorithmInt_parse(IDLParserContext("root"), *_data.data<uint8_t>());
    }

    size_t getDataLength() const {
        return _data.length() - kAssociatedDataLength;
    }


    std::shared_ptr<SymmetricKey> _key;
    ConstDataRange _data;
    std::vector<uint8_t> _plaintext;
};

}  // namespace mongo
