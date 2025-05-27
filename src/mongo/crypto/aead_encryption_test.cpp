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

#include "aead_encryption.h"

#include "mongo/base/data_range.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/hex.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

constexpr size_t ivAndHmacLen = 16 + 32;
constexpr size_t cbcBlockLen = 16;

// The first test is to ensure that the length of the cipher is correct when
// calling AEAD encrypt.
TEST(AEAD, aeadCipherOutputLength) {
    size_t plainTextLen = 16;
    auto cipherLen = crypto::aeadCipherOutputLength(plainTextLen);
    ASSERT_EQ(cipherLen, size_t(80));

    plainTextLen = 10;
    cipherLen = crypto::aeadCipherOutputLength(plainTextLen);
    ASSERT_EQ(cipherLen, size_t(64));
}

TEST(AEAD, EncryptAndDecrypt) {
    // Test case from RFC:
    // https://tools.ietf.org/html/draft-mcgrew-aead-aes-cbc-hmac-sha2-05#section-5.4

    const uint8_t aesAlgorithm = 0x1;

    std::array<uint8_t, 64> symKey = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
        0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
        0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
        0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f};

    SecureVector<uint8_t> aesVector = SecureVector<uint8_t>(symKey.begin(), symKey.end());
    SymmetricKey key = SymmetricKey(aesVector, aesAlgorithm, "aeadEncryptDecryptTest");

    const std::array<uint8_t, 128> plainTextTest = {
        0x41, 0x20, 0x63, 0x69, 0x70, 0x68, 0x65, 0x72, 0x20, 0x73, 0x79, 0x73, 0x74, 0x65, 0x6d,
        0x20, 0x6d, 0x75, 0x73, 0x74, 0x20, 0x6e, 0x6f, 0x74, 0x20, 0x62, 0x65, 0x20, 0x72, 0x65,
        0x71, 0x75, 0x69, 0x72, 0x65, 0x64, 0x20, 0x74, 0x6f, 0x20, 0x62, 0x65, 0x20, 0x73, 0x65,
        0x63, 0x72, 0x65, 0x74, 0x2c, 0x20, 0x61, 0x6e, 0x64, 0x20, 0x69, 0x74, 0x20, 0x6d, 0x75,
        0x73, 0x74, 0x20, 0x62, 0x65, 0x20, 0x61, 0x62, 0x6c, 0x65, 0x20, 0x74, 0x6f, 0x20, 0x66,
        0x61, 0x6c, 0x6c, 0x20, 0x69, 0x6e, 0x74, 0x6f, 0x20, 0x74, 0x68, 0x65, 0x20, 0x68, 0x61,
        0x6e, 0x64, 0x73, 0x20, 0x6f, 0x66, 0x20, 0x74, 0x68, 0x65, 0x20, 0x65, 0x6e, 0x65, 0x6d,
        0x79, 0x20, 0x77, 0x69, 0x74, 0x68, 0x6f, 0x75, 0x74, 0x20, 0x69, 0x6e, 0x63, 0x6f, 0x6e,
        0x76, 0x65, 0x6e, 0x69, 0x65, 0x6e, 0x63, 0x65};

    std::array<uint8_t, 16> iv = {0x1a,
                                  0xf3,
                                  0x8c,
                                  0x2d,
                                  0xc2,
                                  0xb9,
                                  0x6f,
                                  0xfd,
                                  0xd8,
                                  0x66,
                                  0x94,
                                  0x09,
                                  0x23,
                                  0x41,
                                  0xbc,
                                  0x04};

    std::array<uint8_t, 42> associatedData = {
        0x54, 0x68, 0x65, 0x20, 0x73, 0x65, 0x63, 0x6f, 0x6e, 0x64, 0x20, 0x70, 0x72, 0x69,
        0x6e, 0x63, 0x69, 0x70, 0x6c, 0x65, 0x20, 0x6f, 0x66, 0x20, 0x41, 0x75, 0x67, 0x75,
        0x73, 0x74, 0x65, 0x20, 0x4b, 0x65, 0x72, 0x63, 0x6b, 0x68, 0x6f, 0x66, 0x66, 0x73};

    std::array<uint8_t, sizeof(uint64_t)> dataLenBitsEncodedStorage;
    DataRange dataLenBitsEncoded(dataLenBitsEncodedStorage);
    dataLenBitsEncoded.write<BigEndian<uint64_t>>(associatedData.size() * 8);

    std::array<uint8_t, 192> cryptoBuffer = {};
    const size_t outLen = crypto::aeadCipherOutputLength(plainTextTest.size());
    ASSERT_EQ(outLen, cryptoBuffer.size());

    ASSERT_OK(crypto::aeadEncryptWithIV(
        symKey, {plainTextTest}, {iv}, {associatedData}, dataLenBitsEncoded, {cryptoBuffer}));

    std::array<uint8_t, 192> cryptoBufferTest = {
        0x1a, 0xf3, 0x8c, 0x2d, 0xc2, 0xb9, 0x6f, 0xfd, 0xd8, 0x66, 0x94, 0x09, 0x23, 0x41, 0xbc,
        0x04, 0x4a, 0xff, 0xaa, 0xad, 0xb7, 0x8c, 0x31, 0xc5, 0xda, 0x4b, 0x1b, 0x59, 0x0d, 0x10,
        0xff, 0xbd, 0x3d, 0xd8, 0xd5, 0xd3, 0x02, 0x42, 0x35, 0x26, 0x91, 0x2d, 0xa0, 0x37, 0xec,
        0xbc, 0xc7, 0xbd, 0x82, 0x2c, 0x30, 0x1d, 0xd6, 0x7c, 0x37, 0x3b, 0xcc, 0xb5, 0x84, 0xad,
        0x3e, 0x92, 0x79, 0xc2, 0xe6, 0xd1, 0x2a, 0x13, 0x74, 0xb7, 0x7f, 0x07, 0x75, 0x53, 0xdf,
        0x82, 0x94, 0x10, 0x44, 0x6b, 0x36, 0xeb, 0xd9, 0x70, 0x66, 0x29, 0x6a, 0xe6, 0x42, 0x7e,
        0xa7, 0x5c, 0x2e, 0x08, 0x46, 0xa1, 0x1a, 0x09, 0xcc, 0xf5, 0x37, 0x0d, 0xc8, 0x0b, 0xfe,
        0xcb, 0xad, 0x28, 0xc7, 0x3f, 0x09, 0xb3, 0xa3, 0xb7, 0x5e, 0x66, 0x2a, 0x25, 0x94, 0x41,
        0x0a, 0xe4, 0x96, 0xb2, 0xe2, 0xe6, 0x60, 0x9e, 0x31, 0xe6, 0xe0, 0x2c, 0xc8, 0x37, 0xf0,
        0x53, 0xd2, 0x1f, 0x37, 0xff, 0x4f, 0x51, 0x95, 0x0b, 0xbe, 0x26, 0x38, 0xd0, 0x9d, 0xd7,
        0xa4, 0x93, 0x09, 0x30, 0x80, 0x6d, 0x07, 0x03, 0xb1, 0xf6, 0x4d, 0xd3, 0xb4, 0xc0, 0x88,
        0xa7, 0xf4, 0x5c, 0x21, 0x68, 0x39, 0x64, 0x5b, 0x20, 0x12, 0xbf, 0x2e, 0x62, 0x69, 0xa8,
        0xc5, 0x6a, 0x81, 0x6d, 0xbc, 0x1b, 0x26, 0x77, 0x61, 0x95, 0x5b, 0xc5};

    ASSERT_EQ(cryptoBuffer.size(), cryptoBufferTest.size());
    ASSERT_EQ(0, std::memcmp(cryptoBuffer.data(), cryptoBufferTest.data(), cryptoBuffer.size()));

    std::array<uint8_t, 144> plainText = {};
    auto swPlainTextLen = crypto::aeadDecrypt(key, {cryptoBuffer}, {associatedData}, {plainText});
    ASSERT_OK(swPlainTextLen.getStatus());

    ASSERT_EQ(plainTextTest.size(), swPlainTextLen.getValue());
    ASSERT_EQ(0, std::memcmp(plainText.data(), plainTextTest.data(), plainTextTest.size()));

    // Decrypt should fail if we alter the key.
    (*aesVector)[0] ^= 1;
    key = SymmetricKey(aesVector, aesAlgorithm, "aeadEncryptDecryptTest");
    ASSERT_NOT_OK(
        crypto::aeadDecrypt(key, {cryptoBuffer}, {associatedData}, {plainText}).getStatus());
}

TEST(AEADFLE2, Fle2AeadCipherOutputLength) {
    size_t plainTextLen = 16;
    auto ctrCipherLen = crypto::fle2AeadCipherOutputLength(plainTextLen, crypto::aesMode::ctr);
    auto cbcCipherLen = crypto::fle2AeadCipherOutputLength(plainTextLen, crypto::aesMode::cbc);
    ASSERT_EQ(ctrCipherLen, (ivAndHmacLen + plainTextLen));
    ASSERT_EQ(cbcCipherLen, (ivAndHmacLen + plainTextLen + cbcBlockLen));

    plainTextLen = 10;
    ctrCipherLen = crypto::fle2AeadCipherOutputLength(plainTextLen, crypto::aesMode::ctr);
    cbcCipherLen = crypto::fle2AeadCipherOutputLength(plainTextLen, crypto::aesMode::cbc);
    ASSERT_EQ(ctrCipherLen, (ivAndHmacLen + plainTextLen));
    ASSERT_EQ(cbcCipherLen, (ivAndHmacLen + cbcBlockLen));
}

TEST(AEADFLE2, Fle2AeadGetMaximumPlainTextLength) {
    size_t cipherTextLen = 234;
    auto plainTextLen = mongo::fle2AeadGetMaximumPlainTextLength(cipherTextLen);
    ASSERT_OK(plainTextLen);
    ASSERT_EQ(plainTextLen.getValue(), cipherTextLen - ivAndHmacLen);

    cipherTextLen = (cbcBlockLen * 3) + ivAndHmacLen;
    plainTextLen = mongo::fle2AeadGetMaximumPlainTextLength(cipherTextLen);
    ASSERT_OK(plainTextLen);
    ASSERT_EQ(plainTextLen.getValue(), cipherTextLen - ivAndHmacLen);

    cipherTextLen = ivAndHmacLen;
    plainTextLen = mongo::fle2AeadGetMaximumPlainTextLength(cipherTextLen);
    ASSERT_NOT_OK(plainTextLen);
}

TEST(EncryptFLE2, Fle2EncryptRandom) {
    mongo::PseudoRandom rnd(SecureRandom().nextInt64());

    std::vector<uint8_t> key(mongo::crypto::sym256KeySize);
    rnd.fill(&key[0], key.size());

    std::vector<uint8_t> plainText(static_cast<uint8_t>(rnd.nextInt32()) + 1);
    rnd.fill(&plainText[0], plainText.size());
    std::vector<uint8_t> encryptedBytes(plainText.size() + 16);
    DataRange encrypted(encryptedBytes);

    std::vector<uint8_t> blankIv;
    auto encryptStatus = mongo::crypto::fle2Encrypt({key}, {plainText}, {blankIv}, encrypted);
    ASSERT_OK(encryptStatus);

    auto planTextLength = mongo::fle2GetPlainTextLength(encrypted.length());
    ASSERT_OK(planTextLength);
    ASSERT_EQ(plainText.size(), planTextLength.getValue());

    std::vector<uint8_t> decryptedBytes(planTextLength.getValue());
    auto decryptStatus = mongo::crypto::fle2Decrypt({key}, encrypted, DataRange(decryptedBytes));
    ASSERT_OK(decryptStatus);
    ASSERT_TRUE(std::equal(
        decryptedBytes.begin(), decryptedBytes.end(), plainText.begin(), plainText.end()));
}

class Fle2AeadTestVectors : public unittest::Test {
public:
    struct TestVector {
        StringData ad;
        StringData c;
        StringData iv;
        StringData ke;
        StringData km;
        StringData m;
        StringData s;
        StringData t;
    };

    /*
     * Runs fle2AeadEncrypt with the given parameters, and with an output buffer
     * of correct length. Returns a StatusWith containing the resulting ciphertext.
     */
    StatusWith<std::vector<uint8_t>> doAeadEncryption(ConstDataRange key,
                                                      ConstDataRange iv,
                                                      ConstDataRange associatedData,
                                                      ConstDataRange plainText,
                                                      mongo::crypto::aesMode mode) {
        auto expectedCipherTextLen =
            mongo::crypto::fle2AeadCipherOutputLength(plainText.length(), mode);
        std::vector<uint8_t> encryptedBytes(expectedCipherTextLen);
        DataRange encrypted(encryptedBytes);

        auto encryptStatus =
            mongo::crypto::fle2AeadEncrypt(key, plainText, iv, associatedData, encrypted, mode);
        if (!encryptStatus.isOK()) {
            return encryptStatus;
        }
        return encryptedBytes;
    }

    StatusWith<std::vector<uint8_t>> doAeadDecryption(ConstDataRange key,
                                                      ConstDataRange iv,
                                                      ConstDataRange associatedData,
                                                      ConstDataRange cipherText,
                                                      mongo::crypto::aesMode mode) {
        auto plainTextLength = mongo::fle2AeadGetMaximumPlainTextLength(cipherText.length());
        ASSERT_OK(plainTextLength);
        ASSERT_EQ(plainTextLength.getValue(), cipherText.length() - ivAndHmacLen);

        std::vector<uint8_t> decryptedBytes(plainTextLength.getValue());
        auto decryptStatus = mongo::crypto::fle2AeadDecrypt(
            key, cipherText, associatedData, DataRange(decryptedBytes), mode);
        if (!decryptStatus.isOK()) {
            return decryptStatus.getStatus();
        }

        if (mode == crypto::aesMode::cbc) {
            decryptedBytes.resize(decryptStatus.getValue());
        }
        return decryptedBytes;
    }

    void roundTrip(ConstDataRange key,
                   ConstDataRange iv,
                   ConstDataRange associatedData,
                   ConstDataRange plainText,
                   boost::optional<ConstDataRange> expectedCipherText,
                   mongo::crypto::aesMode mode) {

        auto swEncryptedBytes = doAeadEncryption(key, iv, associatedData, plainText, mode);
        ASSERT_OK(swEncryptedBytes.getStatus());

        auto& encryptedBytes = swEncryptedBytes.getValue();
        DataRange encrypted(encryptedBytes);

        if (expectedCipherText) {
            ASSERT_EQ(expectedCipherText->length(), encrypted.length());
            ASSERT_TRUE(std::equal(
                encryptedBytes.begin(), encryptedBytes.end(), expectedCipherText->data<uint8_t>()));
        }

        auto swDecryptedBytes = doAeadDecryption(key, iv, associatedData, encrypted, mode);
        ASSERT_OK(swDecryptedBytes.getStatus());

        auto& decryptedBytes = swDecryptedBytes.getValue();
        ASSERT_EQ(plainText.length(), decryptedBytes.size());
        ASSERT_TRUE(
            std::equal(decryptedBytes.begin(), decryptedBytes.end(), plainText.data<uint8_t>()));
    }

    void evaluate(const TestVector& vector, crypto::aesMode mode) {
        std::string associatedData = hexblob::decode(vector.ad);
        std::string iv = hexblob::decode(vector.iv);
        std::string key = hexblob::decode(vector.ke) + hexblob::decode(vector.km);
        std::string plainText = hexblob::decode(vector.m);
        std::string expectedCipherText = hexblob::decode(vector.c);
        std::vector<uint8_t> blankIv;

        // Test with IV provided with test vector. Verify intermediate values
        roundTrip({key}, {iv}, {associatedData}, {plainText}, {{expectedCipherText}}, mode);

        // Test with random IV. Intermediate values are undetermined
        roundTrip({key}, {blankIv}, {associatedData}, {plainText}, boost::none, mode);
    }
};

TEST_F(Fle2AeadTestVectors, Fle2AeadRandom) {
    auto seed = SecureRandom().nextInt64();
    mongo::PseudoRandom rnd(seed);

    std::cout << "Fle2AeadRandom using seed: " << seed << std::endl;

    std::vector<uint8_t> key(mongo::crypto::kFieldLevelEncryption2KeySize);
    rnd.fill(&key[0], key.size());

    std::vector<uint8_t> associatedData(18);
    rnd.fill(&associatedData[0], associatedData.size());

    std::vector<uint8_t> plainText(956);
    rnd.fill(&plainText[0], plainText.size());

    std::vector<uint8_t> blankIv;

    roundTrip({key}, {blankIv}, {associatedData}, {plainText}, boost::none, crypto::aesMode::ctr);

    roundTrip({key}, {blankIv}, {associatedData}, {plainText}, boost::none, crypto::aesMode::cbc);
}

TEST_F(Fle2AeadTestVectors, Fle2AeadDetectTampering) {
    auto seed = SecureRandom().nextInt64();
    mongo::PseudoRandom rnd(seed);

    std::cout << "Fle2AeadDetectTampering using seed: " << seed << std::endl;

    std::vector<uint8_t> key(mongo::crypto::kFieldLevelEncryption2KeySize);
    rnd.fill(&key[0], key.size());

    std::vector<uint8_t> associatedData(18);
    rnd.fill(&associatedData[0], associatedData.size());

    std::vector<uint8_t> plainText(956);
    rnd.fill(&plainText[0], plainText.size());

    std::vector<uint8_t> blankIv;

    for (auto mode : {crypto::aesMode::ctr, crypto::aesMode::cbc}) {
        auto swEncryptedBytes =
            doAeadEncryption({key}, {blankIv}, {associatedData}, {plainText}, mode);
        ASSERT_OK(swEncryptedBytes.getStatus());

        auto& encrypted = swEncryptedBytes.getValue();

        // test decrypt works before tampering the ciphertext
        auto swDecryptedBytes =
            doAeadDecryption({key}, {blankIv}, {associatedData}, {encrypted}, mode);
        ASSERT_OK(swDecryptedBytes.getStatus());

        // test decrypt fails after tampering the ciphertext
        encrypted[rnd.nextInt32() % encrypted.size()]++;
        swDecryptedBytes = doAeadDecryption({key}, {blankIv}, {associatedData}, {encrypted}, mode);
        ASSERT_NOT_OK(swDecryptedBytes.getStatus());
    }
}

// echo -n "Old McDonald had a farm, ei-eio" | aead_encryption_fle2_test_vectors.sh
TEST_F(Fle2AeadTestVectors, Fle2AeadTestCaseMcdonald) {
    TestVector vector;
    // clang-format off
    vector.ad = "e08ef57279ca6a2078f7c68a4a5d8a26a5b0"_sd;
    vector.c = "75f08f498f607cda3437c4860bd33e0748dc4ad9108190bc13193cfd9dddbb5bd680989e4d73d53af4f9bcd5d8a49929cacaf3220359babeb9a8f66ff236683055e26ee21b0bb5a2dd45eabd8ecd63"_sd;
    vector.iv = "75f08f498f607cda3437c4860bd33e07"_sd;
    vector.ke = "7b0c5d51bf7ab19beb784bc89caa029d9b10479cccfbda4bf18f24372ffa60ba"_sd;
    vector.km = "0a5bb33598b8787f775f4fb823730a485aaff2a105b1ea3826b92e2bcce9164d"_sd;
    vector.m = "4f6c64204d63446f6e616c64206861642061206661726d2c2065692d65696f"_sd;
    vector.s = "48dc4ad9108190bc13193cfd9dddbb5bd680989e4d73d53af4f9bcd5d8a499"_sd;
    vector.t = "29cacaf3220359babeb9a8f66ff236683055e26ee21b0bb5a2dd45eabd8ecd63"_sd;
    // clang-format on
    evaluate(vector, crypto::aesMode::ctr);

    // clang-format off
    vector.ad = "69d1b6b5042ec1fc675cd1195434a0ac4720"_sd;
    vector.c = "0d52d4d1f12f7f9a4e2693b7271c631b4c7a679a7a046ade560ffa7dd3451acd513bdb1d0a7d217462854c6b93abb5f8dde8dbfc94293c8eb9577699096a29eaad227fd7a212604bedb0f03fddbd4167"_sd;
    vector.iv = "0d52d4d1f12f7f9a4e2693b7271c631b"_sd;
    vector.ke = "a3f3fb0451fb85d29bca5902539ecc5ce8970b8242c7dacac60f7bdfd0a555e0"_sd;
    vector.km = "7ce25815235a850e1c2a27efcfef05495fe0a1375889772cf080010ce58e6cbb"_sd;
    vector.m = "4f6c64204d63446f6e616c64206861642061206661726d2c2065692d65696f"_sd;
    vector.s = "4c7a679a7a046ade560ffa7dd3451acd513bdb1d0a7d217462854c6b93abb5f8"_sd;
    vector.t = "dde8dbfc94293c8eb9577699096a29eaad227fd7a212604bedb0f03fddbd4167"_sd;
    // clang-format on
    evaluate(vector, crypto::aesMode::cbc);
}

TEST_F(Fle2AeadTestVectors, Fle2AeadTestCaseUnusualAdLength) {
    TestVector vector;
    // clang-format off
    vector.ad = "63b71111983ecb3cdd5a66115fe843455c4ac07a142586ea97aa85f5cc506301cf652800ed2789d6b147150f3636594c9cc0abae44b7eb40"_sd;
    vector.c = "8f5a6fb0a8123797ca9be37821426b48d34bc190c9de5f2c5cbefcab085fd26322197184328028f408c85171caa7e00e2d9819a861562563f905857d0f9ca791a4de6028e1e1"_sd;
    vector.iv = "8f5a6fb0a8123797ca9be37821426b48"_sd;
    vector.ke = "8506b1c572965416cbb88a2a9eaa0eca4b4b17694c6c7bbd8eb466ec97703b8f"_sd;
    vector.km = "152354910877ecefccf3bbcb9fc268748f1cec9b4572d7d6c99d841ec8b2d939"_sd;
    vector.m = "8adf5d4744a17f1b4dddd4cbf4840aae4c4fa5c8a653"_sd;
    vector.s = "d34bc190c9de5f2c5cbefcab085fd263221971843280"_sd;
    vector.t = "28f408c85171caa7e00e2d9819a861562563f905857d0f9ca791a4de6028e1e1"_sd;
    // clang-format on
    evaluate(vector, crypto::aesMode::ctr);

    // clang-format off
    vector.ad = "600e4abf31e88262731dda95fbd6c10017435f1b31fad9fdae20e24bedd0027c7761e36dba373f7f88ffc60f54f39f2a1547e029cb95d481"_sd;
    vector.c = "a1a3c0bc4c17bafdffd04f320dea258ded1a349e4bb5d4836b508cced0f4529a1c8158c38991b04ef86b47c1b7f27a743b6e40a998922d7ac8773db09afa6c2abf41d8f9e7a2aa392bc438bcd103d262"_sd;
    vector.iv = "a1a3c0bc4c17bafdffd04f320dea258d"_sd;
    vector.ke = "507dfcec3aca6edf1403f4c3a0392003c59bedaa0b6dd7f363e7d5530ee3c070"_sd;
    vector.km = "d580befcfe9cf5940af1d175879fe0b8a19ddcd52d59944641c02fffee928fc4"_sd;
    vector.m = "8adf5d4744a17f1b4dddd4cbf4840aae4c4fa5c8a653"_sd;
    vector.s = "ed1a349e4bb5d4836b508cced0f4529a1c8158c38991b04ef86b47c1b7f27a74"_sd;
    vector.t = "3b6e40a998922d7ac8773db09afa6c2abf41d8f9e7a2aa392bc438bcd103d262"_sd;
    // clang-format on
    evaluate(vector, crypto::aesMode::cbc);
}

TEST_F(Fle2AeadTestVectors, Fle2AeadTestCaseOneByte) {
    TestVector vector;
    // clang-format off
    vector.ad = "2a10adb3bbcf625cb78d13cab1d824eea87d"_sd;
    vector.c = "369a26a9a346525024ae525e7975ae213e0bd168f97f6cd44bd3998a886ec23f495d352467e8b29b9babef2702eb7b3331"_sd;
    vector.iv = "369a26a9a346525024ae525e7975ae21"_sd;
    vector.ke = "9ffa9bb72c5aec8cdd41207d975bdf30b9973301db3174f5a013b561dd91e668"_sd;
    vector.km = "b0a7e6bcc4d41efff27d3fd3c7955caa33457980097107a9dd275811bdccba68"_sd;
    vector.m = "2e"_sd;
    vector.s = "3e"_sd;
    vector.t = "0bd168f97f6cd44bd3998a886ec23f495d352467e8b29b9babef2702eb7b3331"_sd;
    // clang-format on
    evaluate(vector, crypto::aesMode::ctr);

    // clang-format off
    vector.ad = "32f04022327d1f19874fa098c09ed0475ea4"_sd;
    vector.c = "05af2f9c948ad45da80907cd954816a0a954c997e8c648e6311a524af1a3f4d6a21f72fbf57df9f16d5bec2633662fbadab21fc71b117625ee4d790557721a29"_sd;
    vector.iv = "05af2f9c948ad45da80907cd954816a0"_sd;
    vector.ke = "86072c82a91986dca10bfc52fb230d745f3acccbf854536c6239e401861f4bee"_sd;
    vector.km = "c8296ced862246a219ea97e9e8795229237b5689ea3e78896cd122d0e99dfc98"_sd;
    vector.m = "2e"_sd;
    vector.s = "a954c997e8c648e6311a524af1a3f4d6"_sd;
    vector.t = "a21f72fbf57df9f16d5bec2633662fbadab21fc71b117625ee4d790557721a29"_sd;
    // clang-format on
    evaluate(vector, crypto::aesMode::cbc);
}

TEST_F(Fle2AeadTestVectors, Fle2AeadTestCaseShort) {
    TestVector vector;
    // clang-format off
    vector.ad = "20eb32f306a3d4c3f013ba2bd4aec890ed7e"_sd;
    vector.c = "c96f564f962cd43b9f588b14420f7cf32db4f752d184ee72a60b18f7a6f74a9ef699a08c99de75b613d2be1c07c3f80865a33940ce789a5674952896e72cde9e0cd1d4d90d8d02f774f484215a18c1d2"_sd;
    vector.iv = "c96f564f962cd43b9f588b14420f7cf3"_sd;
    vector.ke = "c361042bfcc41effbe5a7e6f1ceeff3ddc921213b37062e2993f563868617289"_sd;
    vector.km = "f20b19d3bf2102462cddd7603d4e86b2cdea1e08ba4b2a30def11a6ec6757db7"_sd;
    vector.m = "e920e9663f4058f0a15e2b15efe34d6fc75fbc32096c338a795068ff5e6b40b2"_sd;
    vector.s = "2db4f752d184ee72a60b18f7a6f74a9ef699a08c99de75b613d2be1c07c3f808"_sd;
    vector.t = "65a33940ce789a5674952896e72cde9e0cd1d4d90d8d02f774f484215a18c1d2"_sd;
    // clang-format on
    evaluate(vector, crypto::aesMode::ctr);

    // clang-format off
    vector.ad = "e222c8b73ffa3d223c7c67dc560d6e27c2dc"_sd;
    vector.c = "bc78351eebd86baab3e7f1d53994b157ae5323d5da814acaf196a93b372a78432c33fe5ddca455289e13b23b2075af28c703daf8e1578606a35f985edb1e22a813604724c10ea4b1149e079f912c01dc5bd79aa1c01c324d479f9733310ffddd"_sd;
    vector.iv = "bc78351eebd86baab3e7f1d53994b157"_sd;
    vector.ke = "1cad05cb3526e70dc4798f7c714db9b45a039892ff0fca5866cb7d8d7ef4795c"_sd;
    vector.km = "75b1aaab45c2b2df310a9108f89cb07764d660c02a010aafe9416482966c24ef"_sd;
    vector.m = "e920e9663f4058f0a15e2b15efe34d6fc75fbc32096c338a795068ff5e6b40b2"_sd;
    vector.s = "ae5323d5da814acaf196a93b372a78432c33fe5ddca455289e13b23b2075af28c703daf8e1578606a35f985edb1e22a8"_sd;
    vector.t = "13604724c10ea4b1149e079f912c01dc5bd79aa1c01c324d479f9733310ffddd"_sd;
    // clang-format on
    evaluate(vector, crypto::aesMode::cbc);
}

TEST_F(Fle2AeadTestVectors, Fle2AeadTestCaseMedium) {
    TestVector vector;
    // clang-format off
    vector.ad = "912fe39c2c59cec602b06e05defe00ab2a7a"_sd;
    vector.c = "d437935327442a5a98270940078e6208a9b52874937eb0bafb823c3f8091860e0401aa28f7323b53bcffc0394bad45530e186096ddc4769ed17d6c8e08c7cb3072e068633b5afd9d80443b9cdce96d027c0ba4c3102b2bbc0ea78f628c7dc2390176eb76e33d26530109d0582bcb8de8ad74aff5ee3f4457b8c9252d009e0f6f3e674cfaa3a4326ee105f0fe58489b1aefbea783baee2461debb6598f32cdfa0939fc0865534c31d6d8ce0b7bebf6fe060a482b5473a1cf5377063a039958bc4793b3604d3dfb6e3ad4fd54ec4276f9f7a414cae690ed048ae126ae97017299a67328f2afde3f62e969199e501c04c7359dbbb9b602f78c8fefc157ec8e8833265e40e3199ad77f291821c7874f1370955d0d67494565daccb"_sd;
    vector.iv = "d437935327442a5a98270940078e6208"_sd;
    vector.ke = "c7aa75ad698269d5eb50e831157f146399075dc0014106d2367758a43371c71f"_sd;
    vector.km = "21b3dc46eb674031d1d18fb74efeae6d3a49743d820668a141a841159afc74c0"_sd;
    vector.m = "e8062f1b86e3b1346d8400379afc518ae65c0ead8a349fc0d0fe3d85b88831475331b4cdf86ce953adfd69e2bea9e3e11f319757aa9bcf14eeb4b12c3835cd793f116f2d582e9680cc644cd69df73e3e8639fc2e500cf60959fd40fe1e5d782266e0d5ccba7d01bd05de371f179bf8ea901629742254d0950c790f845822570a6dcc817a78f35109879ca6b4c113f498574f0daca792d09b47e0d467b33d104b44274991a193e23d132eda51dbd6d235735187c9735421844970e09f573b76a9e850f1c2262920fbb2eb43443527fa36bb37c5f005ca33379dfb4a40d6dd5f7439de2a33c4ca0db0c1"_sd;
    vector.s = "a9b52874937eb0bafb823c3f8091860e0401aa28f7323b53bcffc0394bad45530e186096ddc4769ed17d6c8e08c7cb3072e068633b5afd9d80443b9cdce96d027c0ba4c3102b2bbc0ea78f628c7dc2390176eb76e33d26530109d0582bcb8de8ad74aff5ee3f4457b8c9252d009e0f6f3e674cfaa3a4326ee105f0fe58489b1aefbea783baee2461debb6598f32cdfa0939fc0865534c31d6d8ce0b7bebf6fe060a482b5473a1cf5377063a039958bc4793b3604d3dfb6e3ad4fd54ec4276f9f7a414cae690ed048ae126ae97017299a67328f2afde3f62e969199e501c04c7359dbbb9b602f78c8fe"_sd;
    vector.t = "fc157ec8e8833265e40e3199ad77f291821c7874f1370955d0d67494565daccb"_sd;
    // clang-format on
    evaluate(vector, crypto::aesMode::ctr);

    // clang-format off
    vector.ad = "94e50883e0bbd77b479a1735048665d60cfd"_sd;
    vector.c = "775ed609544ba9c7b31be9d9eaebb4b9f271e04fc0be0460cb1fd67037a75712a17fdd9cefb5f2f881c8cd27e157375b3c5855638592c7836a04aec5cd9615b35e2798f83620ec7533635efff40e999fbf95d21fe0f9c91c9d055e790236573554f082145fcce58a2e702d2ba359d4b7f57ddf1ddd5648ad149cc205616c68590e10b2386cc7271caa2d06acff851a48ebba82ed0e40a7a46fc92f0577a4c04fbecad48087582be9c724186a27868d4a1d193cd7d82914441053af1841b02b95950841a407bd00e51920362dc0e831447f3ebf1a3428a7ad1bf965f7970d1ef958a70600f3ffb9f4570e6845e8859321ba14a4d6887d3dfe842eb19e0a63431849db5648703d9bfbfa3b9bb0265fb141175f8af1d6c21ce1704d105c6168e08f"_sd;
    vector.iv = "775ed609544ba9c7b31be9d9eaebb4b9"_sd;
    vector.ke = "1804d2d1a106a80b0965b4ca6db1b8c6def116951c6cbe077056fce7390b90a8"_sd;
    vector.km = "ef7ae8f92b5ce8a23652174b46b6d85b5d68acbec7630df949d39b223de87e5a"_sd;
    vector.m = "e8062f1b86e3b1346d8400379afc518ae65c0ead8a349fc0d0fe3d85b88831475331b4cdf86ce953adfd69e2bea9e3e11f319757aa9bcf14eeb4b12c3835cd793f116f2d582e9680cc644cd69df73e3e8639fc2e500cf60959fd40fe1e5d782266e0d5ccba7d01bd05de371f179bf8ea901629742254d0950c790f845822570a6dcc817a78f35109879ca6b4c113f498574f0daca792d09b47e0d467b33d104b44274991a193e23d132eda51dbd6d235735187c9735421844970e09f573b76a9e850f1c2262920fbb2eb43443527fa36bb37c5f005ca33379dfb4a40d6dd5f7439de2a33c4ca0db0c1"_sd;
    vector.s = "f271e04fc0be0460cb1fd67037a75712a17fdd9cefb5f2f881c8cd27e157375b3c5855638592c7836a04aec5cd9615b35e2798f83620ec7533635efff40e999fbf95d21fe0f9c91c9d055e790236573554f082145fcce58a2e702d2ba359d4b7f57ddf1ddd5648ad149cc205616c68590e10b2386cc7271caa2d06acff851a48ebba82ed0e40a7a46fc92f0577a4c04fbecad48087582be9c724186a27868d4a1d193cd7d82914441053af1841b02b95950841a407bd00e51920362dc0e831447f3ebf1a3428a7ad1bf965f7970d1ef958a70600f3ffb9f4570e6845e8859321ba14a4d6887d3dfe842eb19e0a634318"_sd;
    vector.t = "49db5648703d9bfbfa3b9bb0265fb141175f8af1d6c21ce1704d105c6168e08f"_sd;
    // clang-format on
    evaluate(vector, crypto::aesMode::cbc);
}

TEST_F(Fle2AeadTestVectors, Fle2AeadTestCaseLonger) {
    TestVector vector;
    // clang-format off
    vector.ad = "d204244e61c18e221eaa36c6912999296d5c"_sd;
    vector.c = "8b3e094bae75d7766cc07d4db1106459c637b389df1f9357776a8b2030162e595f15b087d3414e81395479c5143cd0f32c6fa18688b9ed765f5b1087cef3bd4bf9e243bc4fea978ee69b6a1a768eb18643e998b9f32b4d1f2c196d5bd9eaf89f69b2b81b3e5fc90f21aa863ac4463f924a42772a73fb271a312fd2832f3ed9adfa1fb1b506aa47b92f5edb57dce9b0ea4c1410829c2229204c4e4dbad8f909ccacbc738151f091e791215a346d96f75e498b20f6cb891d3bbf8bf42e342f27df4cc6b80fa14a1ab47af6fba005e76d4b3364de37877d8c664e70792655444c910e094f55b8edd2e541f0bf2164fdd4b2497c7c914d6a1590bc2319541439151139a10e4f422b0fb227887edd75e191bd2b70483f16bde4ca25a17c6c5457a34348ef0601179e51aa83ee141d799377277a464b6c0b4616c1fa1612cf73077b2a3f5fd2f0c87e7d75d575c2e362ebe4b32ce307ff577ffc672a97effb59d236d6d5fc05a2262e9b26e7b366398135326ffa5efbe28df13c903c0aabd15001a86afca854ca3f4c92ba78d69708b12f9c299990c620d3c50b1e42934defb5df594bb66ad222838caa3061ee46df166bf019ce473d13f58ec826b88167dc43f71a074e6f05639501827203a7ba0aac1eb58d065536a99987a3e93d06d7e4c5f184adde5023ffd57704d313103a9dd41702d514f99f8f14561718c1b06891c979345b408d3ea65312b8b57eb8fd37d6633092571c9f833bd497bc8196b450c420f186a87f4c57e27280a1e0c3f4858a87efc73df01677fe8ce9df63ac7c54fa8858f2ae26f0bcc4fa28477545c349b0555d6efdd93f371c8a90de57c167d6e5edc6f8da57282c9a70a86241ed559b96b404cc06b2db9c10bcac612cf3d281cf12b06a962304fcd2bedf28ebc4afed700a6dce775a34b3bd6e874da5e19997cb818f2a1d49e19431eaf0ff6f308ae6fd8d8103778c252034282cb2f9587750ec53e46f91d06894a0c8007a8d2ed9f9942e373d27d0693e40925ddc4da48a17f63e2a3bbc20a6a91a3ff14b311523beec8442a9d4b27aa9419b645f14372b0942ad68a563ccb9baeeedff02f73ee30c1d1a67b910993cc15865451b8fa0d475a473eab98b74bdd3a63139375ea2e6edec58a2625328437922164b8735b4e0babe437edb3fe8a254fed52f750dd6e41af1891df9c2ec7a80b265f2fc21acaba658b57e669cd32ee5a502f71c9e510e953ac9248d06ee225d359234d907a16272efadbebd65830e5761232fc1fe3197f95b9127eacd024842022f2526a1af84fae825d7e86ab1f480783092b790b57e07a33dce7b7c3fc550d9c69e2dd5162dfc48fc7defc79e848a6a548656635f9948ee4dca6339afcf7261e4b665a40996570c04dd0f1e1bcb01c76f06eaf26d790dba34e34952a98ca82ade3eed318d7761db9e0506099b13a7cc2eef6aac7a2e90f8bd628eb34307378fbd48d23b44fd797d476569dcf9533bfd0c73bb436d10a0136aef77e3523ca26646a6dea3ae217bee87223b15097932d32411d991477c75db1099ec144520a1c57836e35ea34607f251b679293f31339b15e3d40f234db116c73981a13677d4037caeec632ecc234aa35eb76e01e08f5dc766f511a7841b9310411b769f4ff274be92e2f44a1d2b00b1914f3ac2a64fbb406398d20b4427bf5c238ecec63103ba288e6211c9b41f90b32ae951062d14b15b8b44f2e592e2cad38b66c0b7a30666063eebb1b35bfe8b792b758895843ec309b6e831667bace68dddd3874bc6b277277bf3570d0052af0656bf97878efc157ae098c277d83ef0d8948d893462c4d6e0e2918f460ce998de3b9af3dac1e730ba66332993e220eb210d95c50578b014ea8aa9c0bd7805fe95a708a29c6fda43549a967b9fd5053a93"_sd;
    vector.iv = "8b3e094bae75d7766cc07d4db1106459"_sd;
    vector.ke = "516c7f9226b3d3eb767bfb7d3c1b260a56ca2ff5f0e5b8aca481c3ff8e30aef6"_sd;
    vector.km = "7e095a6270d4209c35e8736104331049485ded64ddcabe5891cbb56b75dc3e00"_sd;
    vector.m = "186ba19e23fe2fd9a8bed3b667edf41dcaa505cab266390eca74dc659d690197c76d2a5e97d941a7bbab412f81d476a97259a9c29f2960c1c26821bbecf7f2bcb3dbdb1f1a8d5bf741e7e82fc419608ceeebf9013a85aef1bf9d886c0579ea9cb79773d441294bdd716ca08284d6e0c67ea084ceb18c1d8681f58d64e2c74ac32e9a6de68e79bab202bc97d89a3a41b5c8a52ed6ad47842871e19e9aceb2e67ffde02578619ef411e7138cfae934aa83334e17fa630f4531ef4eb9bb0c8cce858ce8afb37098ed3c7df70438fdb716b967696464b0c7176fce9cd8b43496fe1161a92a61400c110bcd30843d389749acf39c56a85f5eae31754aceee63f99ac6fcbd419d781d595bb616f2f6d95684ee163519707c70060880db598cb99688a07ba84ebd1634cb7164d8ffdaef9ca83a81bc7c24a83aab0ce92e4d919142194a9f5f78cefdaea283226090307680c747d4505ff917cab8350a98c5d172f096f9003cbd45bb0034a79b23b83b18f88aa396f7d9f6a7216f2dc7660662220060cf3ca9c573b86b8c9bdd466af9ff0deed4fdffa6e46aa551205923aa7bf356602a9996c0b7c8891517bb5fbfd312fbe360619312bb1b8b1c3a59ff5a3ff16cdfb06c5f3fc48eefee1e26b2dbcc19e40da4ead14d3dfe97ffbe6908a39d43b419400abd8c0dc96ad7df2e219582647adf04530e6690450a07ca264c17d67d07d0d76b20600388f74fbd577e61b72d5240abbd36a118ab1d34210f8a120bce9cda175ea71ef0dc80353118260bed34f039c602ffa2aa24e5cce83553895ec9944a6d3e706a302fa9b832f871a1807023f4ff467b20d695622d4915b6744711d35a82680188744a9fe88ed794fb31c1e27de25e57aa7d4a9805bced58014940067973fe929f1fe0d212f57356628d8e79db53d7254ea378eb21ad23a0c0626d56a7cae2dc29ac0ca8918d881e9ddae2ed5aa0ff7f956f010d87619b1a95f007715b4e5add2f4a3df81d4ffbd91b5120860bd41cf60fedae500fa89d1c9e49fb8b704052dfe963629cd34a7065d6e394bc1b3ae0644d3924efa06a3fa81888a695ec2c33a88528d559be9cb391b6abcba4d0e83f56e21c819ac2612ba3bc76f1441572832b0721fbbcc57bc1daee6016673d4f3662555ab60140e217024d6f18196ea94b271289171c691d8017a81a9b4a3679ed06b302e4de2d94d2e04caf20c6745eb0773560305aa87134fa27eccff9286da53c9848e204bf24e43b349edf37aa683306fcc3bc0fe793cca600ee08c5420c81398b9298a9c8f66f912f37bb917f29a785f567b124abeee5807365e92ddd86858d768957615098d68f52605e279a36481f7f6f019bb22bd540e8c6cfa1ba57f0f79a6fac968ef8dc59fe0578ce391a46d15550c21e3c07d52862d12c8e620bf9ad449308403ebffd02b1ab99eaaadc7442f668156f5cbc0cbfdbbc394b85e4f4224eec7a8a82771b90b9b3da7417d10e194ac71234829e138402763a73756986eb4b74f2f0b2a8b67441af66ed75c39c91ca42a88523ed70100b00f4ac123113c46d9d4a68bd0c8299f0235ef65271ea46774a4cc8847e9ad084bcfe40c3632012ec63cf23296eee7b485f8039fd51657fc7d12d171ba59bebf05b1e90cbbefdf5d615f607f15dd7d9bbad5295bbc6e345cc618ee6bf41f843528ae267a6d9283590e000c32a7199feb9eeae1fe70c38a0d4a50072bf46cb447ff0431dc57b472f2b775c30337dc9ea85cd96ec59c61680cba3323861e017c6226990ce4b20d59b572e49535199874fe3c3f70639d1bdee7e8b53e901bacfd320ce3e0666f68a8f355f8e2282e98e58d26f371f"_sd;
    vector.s = "c637b389df1f9357776a8b2030162e595f15b087d3414e81395479c5143cd0f32c6fa18688b9ed765f5b1087cef3bd4bf9e243bc4fea978ee69b6a1a768eb18643e998b9f32b4d1f2c196d5bd9eaf89f69b2b81b3e5fc90f21aa863ac4463f924a42772a73fb271a312fd2832f3ed9adfa1fb1b506aa47b92f5edb57dce9b0ea4c1410829c2229204c4e4dbad8f909ccacbc738151f091e791215a346d96f75e498b20f6cb891d3bbf8bf42e342f27df4cc6b80fa14a1ab47af6fba005e76d4b3364de37877d8c664e70792655444c910e094f55b8edd2e541f0bf2164fdd4b2497c7c914d6a1590bc2319541439151139a10e4f422b0fb227887edd75e191bd2b70483f16bde4ca25a17c6c5457a34348ef0601179e51aa83ee141d799377277a464b6c0b4616c1fa1612cf73077b2a3f5fd2f0c87e7d75d575c2e362ebe4b32ce307ff577ffc672a97effb59d236d6d5fc05a2262e9b26e7b366398135326ffa5efbe28df13c903c0aabd15001a86afca854ca3f4c92ba78d69708b12f9c299990c620d3c50b1e42934defb5df594bb66ad222838caa3061ee46df166bf019ce473d13f58ec826b88167dc43f71a074e6f05639501827203a7ba0aac1eb58d065536a99987a3e93d06d7e4c5f184adde5023ffd57704d313103a9dd41702d514f99f8f14561718c1b06891c979345b408d3ea65312b8b57eb8fd37d6633092571c9f833bd497bc8196b450c420f186a87f4c57e27280a1e0c3f4858a87efc73df01677fe8ce9df63ac7c54fa8858f2ae26f0bcc4fa28477545c349b0555d6efdd93f371c8a90de57c167d6e5edc6f8da57282c9a70a86241ed559b96b404cc06b2db9c10bcac612cf3d281cf12b06a962304fcd2bedf28ebc4afed700a6dce775a34b3bd6e874da5e19997cb818f2a1d49e19431eaf0ff6f308ae6fd8d8103778c252034282cb2f9587750ec53e46f91d06894a0c8007a8d2ed9f9942e373d27d0693e40925ddc4da48a17f63e2a3bbc20a6a91a3ff14b311523beec8442a9d4b27aa9419b645f14372b0942ad68a563ccb9baeeedff02f73ee30c1d1a67b910993cc15865451b8fa0d475a473eab98b74bdd3a63139375ea2e6edec58a2625328437922164b8735b4e0babe437edb3fe8a254fed52f750dd6e41af1891df9c2ec7a80b265f2fc21acaba658b57e669cd32ee5a502f71c9e510e953ac9248d06ee225d359234d907a16272efadbebd65830e5761232fc1fe3197f95b9127eacd024842022f2526a1af84fae825d7e86ab1f480783092b790b57e07a33dce7b7c3fc550d9c69e2dd5162dfc48fc7defc79e848a6a548656635f9948ee4dca6339afcf7261e4b665a40996570c04dd0f1e1bcb01c76f06eaf26d790dba34e34952a98ca82ade3eed318d7761db9e0506099b13a7cc2eef6aac7a2e90f8bd628eb34307378fbd48d23b44fd797d476569dcf9533bfd0c73bb436d10a0136aef77e3523ca26646a6dea3ae217bee87223b15097932d32411d991477c75db1099ec144520a1c57836e35ea34607f251b679293f31339b15e3d40f234db116c73981a13677d4037caeec632ecc234aa35eb76e01e08f5dc766f511a7841b9310411b769f4ff274be92e2f44a1d2b00b1914f3ac2a64fbb406398d20b4427bf5c238ecec63103ba288e6211c9b41f90b32ae951062d14b15b8b44f2e592e2cad38b66c0b7a30666063eebb1b35bfe8b792b758895843ec309b6e831667bace68dddd3874bc6b277277bf3570d0052af0656bf97878efc157ae098c277d83ef0d8948d893462c4d6e0e2918f460ce998de3b9af3dac1e730ba66332993e220eb21"_sd;
    vector.t = "0d95c50578b014ea8aa9c0bd7805fe95a708a29c6fda43549a967b9fd5053a93"_sd;
    // clang-format on
    evaluate(vector, crypto::aesMode::ctr);

    // clang-format off
    vector.ad = "af98046696b1399fe692c3ff5741b594eb19"_sd;
    vector.c = "c90302f87c3c9373cf32c98e390004b5cdffbdaede53fb178ab08158bc0c80cdf672de16ebd3d680cc6bd4d51a6a7f34556379ad4f3f586b0ba5d3d2d3a540796c8e29aaae7ea2aae905da410f476b4346da8b095da97cddacfdfe7e5ff9596ecaa49f7f0dad669a078c90b5d72d123069173df563bea932337f2b0a58619947edd2bafd8bb0a530da38106c1bd7629715bfed0b1824b42f556ee9c72e0c5c30c33896303c9cbc0323a02fc463800e42405eb1cc4d24d6789ce1d507434c55b8f996abbbbdddf68ac06c7271720ceff17911fbd5720f5836d405c2c9b4e9e789c7a6e4c9cf590f06df6382503c61148446f4e266a82aeebde5dc0a50e96e523b3208694f3e0c2d50e816f66919c4ae2a58c7db523e617bf86e3ef20752a68b5d30d4b07414181127a2967e967fa3f23fd465980b416a5ab1a97f5ce74d8efa6f4813a5c4a5aa116d8472db1de1af0deee97a1b37d872a1432f158169ec54922e82e944ac10f31ef0c9964fec5a1f4387d44e74df5d4030efa12dc3d616a609c22bef4231341147639a9b719ac78f567a06ab03ac664d10f2db9b6594cac80e484edcdca29bc5411f4d190ff33999edeceaaf395c73dcfd4c490005a01b10a3623cb46ae848c2d8d811347f9361233bc8fd9d53f1677b57ca41784a1e1e9634f0e1978eeee86db890395811af3752554baec32ae100377ccb3d365be3d764b4a9b74dc0c24f32c81a09bf3711096488897f956129e33dd4dd1b95a78eb5f7c58167fa7e2ef40ab6a15d70f042a9de38e9e5408fe941fa07a325892bd9cdc862ee5cbd2e3d011891dee5abddbdd342e93973f54dab2ea5406284fa1938cd0a4b0abcf01f7644841d84e87e5fb9e00f3e9c4936839d6a7bad15ae223c00ebd804d4df200477354b6514d065cfa5fa20e8516b4952591b440087a7735a283065f8a67448096e8cdf08213f21c523c8623ce0248620ff68e7ab430a1f3d10fba9b0eee195f1f71a0b52fafb43168977dbaf3a614de2867428e1c580feea8a59fc04bf80b80bbb2db60bb129e647021015507db8f9ce3e95e99ccb5659841dce3c631a5e79c47a1d9985030cbe00c7ff5122fd4dd7a95d518153cc6c1a185193c3e7e3f9dc86ca955c443b21f2225dc826540fce2a1ca49bbd1100f8cd356182340a19fc4d2ee56a486ddcb61e4d5f9236c7facf298399a0da41dad8c0ccd739d765952ffd211840b7fb6521e1f68d47dd6250095cac77856c943e146aabdad79e7a50e2f629a59a160c70954581e207c5744de5071f4fd0bedf478c746ca4d4fc48c2cbcd5c9f00638838cf84530c8466035c917eca86318f23d511e3a96caa7e4e3b432c6cfa3faffd3fbd7dd599c24ee7bba6377f7df5325e730302da7623288aebcc1503360b2ade9e05450a4629755a6a6ff5e237c61efbd1128c915bd335b2bf1a399fb64abba1ed6ea52409d4e5f2eefaba68158252eb40d9494282dfa4ea8b533fa0e17d829d80543ea76ea9e3f3e2164abb96477e0c0290d4c6039927d4d20455cb48f4a7e35cdab5fadf1197db9827deb6e0e7474d08b4ebfc07438039e47ed8e6c3ccbb0369da01fd05cd8e01b50e4e4fed75e672394f2ddf5bb9410db2d90108456c3977c3bc9337e6873df83bbaa43b2b2eaad61f4c85ac595284ac76b8eb38af2db5c006ed94f5fe6bc9d6c18691dc93d159755a33378ac13257c4d20ec4649d963df96dd4599bcd3496f45b8f4d79d2f26d29d5ba52a6ed935bac40880e0c227f58ee62c38b519f300d39fe204c41124219fe463ada2b1a916960d6e1a5107326982e8248d60dcc62dd42c1b1694a5d665efd69147d855545d4c6ee0f3fac087fde6ec49dac82107ec9c7589d97f9e168becb2de11c079587ac1ea7676cdf90ad43fe746967dc29ac966ec7"_sd;
    vector.iv = "c90302f87c3c9373cf32c98e390004b5"_sd;
    vector.ke = "98f06e7d50f044c23471139962355281eced5b04dac3e6cd9b908c0e883dccc9"_sd;
    vector.km = "5879d5f29b1ffb3102a7f5e99c9b0fa3160f74ca63f30b4f6d95f1a0edd3d7e5"_sd;
    vector.m = "186ba19e23fe2fd9a8bed3b667edf41dcaa505cab266390eca74dc659d690197c76d2a5e97d941a7bbab412f81d476a97259a9c29f2960c1c26821bbecf7f2bcb3dbdb1f1a8d5bf741e7e82fc419608ceeebf9013a85aef1bf9d886c0579ea9cb79773d441294bdd716ca08284d6e0c67ea084ceb18c1d8681f58d64e2c74ac32e9a6de68e79bab202bc97d89a3a41b5c8a52ed6ad47842871e19e9aceb2e67ffde02578619ef411e7138cfae934aa83334e17fa630f4531ef4eb9bb0c8cce858ce8afb37098ed3c7df70438fdb716b967696464b0c7176fce9cd8b43496fe1161a92a61400c110bcd30843d389749acf39c56a85f5eae31754aceee63f99ac6fcbd419d781d595bb616f2f6d95684ee163519707c70060880db598cb99688a07ba84ebd1634cb7164d8ffdaef9ca83a81bc7c24a83aab0ce92e4d919142194a9f5f78cefdaea283226090307680c747d4505ff917cab8350a98c5d172f096f9003cbd45bb0034a79b23b83b18f88aa396f7d9f6a7216f2dc7660662220060cf3ca9c573b86b8c9bdd466af9ff0deed4fdffa6e46aa551205923aa7bf356602a9996c0b7c8891517bb5fbfd312fbe360619312bb1b8b1c3a59ff5a3ff16cdfb06c5f3fc48eefee1e26b2dbcc19e40da4ead14d3dfe97ffbe6908a39d43b419400abd8c0dc96ad7df2e219582647adf04530e6690450a07ca264c17d67d07d0d76b20600388f74fbd577e61b72d5240abbd36a118ab1d34210f8a120bce9cda175ea71ef0dc80353118260bed34f039c602ffa2aa24e5cce83553895ec9944a6d3e706a302fa9b832f871a1807023f4ff467b20d695622d4915b6744711d35a82680188744a9fe88ed794fb31c1e27de25e57aa7d4a9805bced58014940067973fe929f1fe0d212f57356628d8e79db53d7254ea378eb21ad23a0c0626d56a7cae2dc29ac0ca8918d881e9ddae2ed5aa0ff7f956f010d87619b1a95f007715b4e5add2f4a3df81d4ffbd91b5120860bd41cf60fedae500fa89d1c9e49fb8b704052dfe963629cd34a7065d6e394bc1b3ae0644d3924efa06a3fa81888a695ec2c33a88528d559be9cb391b6abcba4d0e83f56e21c819ac2612ba3bc76f1441572832b0721fbbcc57bc1daee6016673d4f3662555ab60140e217024d6f18196ea94b271289171c691d8017a81a9b4a3679ed06b302e4de2d94d2e04caf20c6745eb0773560305aa87134fa27eccff9286da53c9848e204bf24e43b349edf37aa683306fcc3bc0fe793cca600ee08c5420c81398b9298a9c8f66f912f37bb917f29a785f567b124abeee5807365e92ddd86858d768957615098d68f52605e279a36481f7f6f019bb22bd540e8c6cfa1ba57f0f79a6fac968ef8dc59fe0578ce391a46d15550c21e3c07d52862d12c8e620bf9ad449308403ebffd02b1ab99eaaadc7442f668156f5cbc0cbfdbbc394b85e4f4224eec7a8a82771b90b9b3da7417d10e194ac71234829e138402763a73756986eb4b74f2f0b2a8b67441af66ed75c39c91ca42a88523ed70100b00f4ac123113c46d9d4a68bd0c8299f0235ef65271ea46774a4cc8847e9ad084bcfe40c3632012ec63cf23296eee7b485f8039fd51657fc7d12d171ba59bebf05b1e90cbbefdf5d615f607f15dd7d9bbad5295bbc6e345cc618ee6bf41f843528ae267a6d9283590e000c32a7199feb9eeae1fe70c38a0d4a50072bf46cb447ff0431dc57b472f2b775c30337dc9ea85cd96ec59c61680cba3323861e017c6226990ce4b20d59b572e49535199874fe3c3f70639d1bdee7e8b53e901bacfd320ce3e0666f68a8f355f8e2282e98e58d26f371f"_sd;
    vector.s = "cdffbdaede53fb178ab08158bc0c80cdf672de16ebd3d680cc6bd4d51a6a7f34556379ad4f3f586b0ba5d3d2d3a540796c8e29aaae7ea2aae905da410f476b4346da8b095da97cddacfdfe7e5ff9596ecaa49f7f0dad669a078c90b5d72d123069173df563bea932337f2b0a58619947edd2bafd8bb0a530da38106c1bd7629715bfed0b1824b42f556ee9c72e0c5c30c33896303c9cbc0323a02fc463800e42405eb1cc4d24d6789ce1d507434c55b8f996abbbbdddf68ac06c7271720ceff17911fbd5720f5836d405c2c9b4e9e789c7a6e4c9cf590f06df6382503c61148446f4e266a82aeebde5dc0a50e96e523b3208694f3e0c2d50e816f66919c4ae2a58c7db523e617bf86e3ef20752a68b5d30d4b07414181127a2967e967fa3f23fd465980b416a5ab1a97f5ce74d8efa6f4813a5c4a5aa116d8472db1de1af0deee97a1b37d872a1432f158169ec54922e82e944ac10f31ef0c9964fec5a1f4387d44e74df5d4030efa12dc3d616a609c22bef4231341147639a9b719ac78f567a06ab03ac664d10f2db9b6594cac80e484edcdca29bc5411f4d190ff33999edeceaaf395c73dcfd4c490005a01b10a3623cb46ae848c2d8d811347f9361233bc8fd9d53f1677b57ca41784a1e1e9634f0e1978eeee86db890395811af3752554baec32ae100377ccb3d365be3d764b4a9b74dc0c24f32c81a09bf3711096488897f956129e33dd4dd1b95a78eb5f7c58167fa7e2ef40ab6a15d70f042a9de38e9e5408fe941fa07a325892bd9cdc862ee5cbd2e3d011891dee5abddbdd342e93973f54dab2ea5406284fa1938cd0a4b0abcf01f7644841d84e87e5fb9e00f3e9c4936839d6a7bad15ae223c00ebd804d4df200477354b6514d065cfa5fa20e8516b4952591b440087a7735a283065f8a67448096e8cdf08213f21c523c8623ce0248620ff68e7ab430a1f3d10fba9b0eee195f1f71a0b52fafb43168977dbaf3a614de2867428e1c580feea8a59fc04bf80b80bbb2db60bb129e647021015507db8f9ce3e95e99ccb5659841dce3c631a5e79c47a1d9985030cbe00c7ff5122fd4dd7a95d518153cc6c1a185193c3e7e3f9dc86ca955c443b21f2225dc826540fce2a1ca49bbd1100f8cd356182340a19fc4d2ee56a486ddcb61e4d5f9236c7facf298399a0da41dad8c0ccd739d765952ffd211840b7fb6521e1f68d47dd6250095cac77856c943e146aabdad79e7a50e2f629a59a160c70954581e207c5744de5071f4fd0bedf478c746ca4d4fc48c2cbcd5c9f00638838cf84530c8466035c917eca86318f23d511e3a96caa7e4e3b432c6cfa3faffd3fbd7dd599c24ee7bba6377f7df5325e730302da7623288aebcc1503360b2ade9e05450a4629755a6a6ff5e237c61efbd1128c915bd335b2bf1a399fb64abba1ed6ea52409d4e5f2eefaba68158252eb40d9494282dfa4ea8b533fa0e17d829d80543ea76ea9e3f3e2164abb96477e0c0290d4c6039927d4d20455cb48f4a7e35cdab5fadf1197db9827deb6e0e7474d08b4ebfc07438039e47ed8e6c3ccbb0369da01fd05cd8e01b50e4e4fed75e672394f2ddf5bb9410db2d90108456c3977c3bc9337e6873df83bbaa43b2b2eaad61f4c85ac595284ac76b8eb38af2db5c006ed94f5fe6bc9d6c18691dc93d159755a33378ac13257c4d20ec4649d963df96dd4599bcd3496f45b8f4d79d2f26d29d5ba52a6ed935bac40880e0c227f58ee62c38b519f300d39fe204c41124219fe463ada2b1a916960d6e1a5107326982e8248d60dcc62dd42c1b1694a5d665efd69147d855545d4c6ee0f3fac087fde6ec49dac82107ec9c758"_sd;
    vector.t = "9d97f9e168becb2de11c079587ac1ea7676cdf90ad43fe746967dc29ac966ec7"_sd;
    // clang-format on
    evaluate(vector, crypto::aesMode::cbc);
}

class Fle2EncryptTestVectors : public unittest::Test {
public:
    struct TestVector {
        StringData d;
        StringData k;
        StringData iv;
        StringData c;
        StringData r;
    };

    void evaluate(const TestVector& vector) {
        std::string in = hexblob::decode(vector.d);
        std::string key = hexblob::decode(vector.k);
        std::string iv = hexblob::decode(vector.iv);
        std::string expect = hexblob::decode(vector.r);

        std::vector<uint8_t> encryptedBytes(in.length() + 16);
        DataRange encrypted(encryptedBytes);

        auto encryptStatus = mongo::crypto::fle2Encrypt({key}, {in}, {iv}, encrypted);
        ASSERT_OK(encryptStatus);

        ASSERT_EQ(expect.size(), encrypted.length());
        ASSERT_TRUE(std::equal(encryptedBytes.begin(),
                               encryptedBytes.end(),
                               reinterpret_cast<uint8_t*>(expect.data())));

        auto planTextLength = mongo::fle2GetPlainTextLength(encrypted.length());
        ASSERT_OK(planTextLength);
        ASSERT_EQ(in.length(), planTextLength.getValue());

        std::vector<uint8_t> decryptedBytes(planTextLength.getValue());
        auto decryptStatus =
            mongo::crypto::fle2Decrypt({key}, encrypted, DataRange(decryptedBytes));
        ASSERT_OK(decryptStatus);
        ASSERT_EQ(in.length(), decryptStatus.getValue());
        ASSERT_TRUE(std::equal(
            decryptedBytes.begin(), decryptedBytes.end(), reinterpret_cast<uint8_t*>(in.data())));
    }
};

TEST_F(Fle2EncryptTestVectors, Fle2AeadTestCaseMcdonald) {
    TestVector vector;
    // clang-format off
    vector.d = "4f6c64204d63446f6e616c64206861642061206661726d2c2065692d65696f"_sd;
    vector.k = "83cf8e8646fd42318a2113e1333d515163d5c0b21ddbfec374107b71ecbec165"_sd;
    vector.iv = "e41c6d484108219172de421a426bbe52"_sd;
    vector.c = "6fd63feca246e26f5f9b59380e6b35f07c572c9eb6207f00fbe1bf3d4c01f4"_sd;
    vector.r = "e41c6d484108219172de421a426bbe526fd63feca246e26f5f9b59380e6b35f07c572c9eb6207f00fbe1bf3d4c01f4"_sd;
    // clang-format on
    evaluate(vector);
}

TEST_F(Fle2EncryptTestVectors, Fle2AeadTestCaseOneByte) {
    TestVector vector;
    // clang-format off
    vector.d = "72"_sd;
    vector.k = "43e75cda73214f659c9b0372b3069a9c5954e07dc22ec339749def6482d40be8"_sd;
    vector.iv = "d61217b67b49ab2259fd901ad32e9d7d"_sd;
    vector.c = "96"_sd;
    vector.r = "d61217b67b49ab2259fd901ad32e9d7d96"_sd;
    // clang-format on
    evaluate(vector);
}

TEST_F(Fle2EncryptTestVectors, Fle2AeadTestCaseMedium) {
    TestVector vector;
    // clang-format off
    vector.d = "6cdfd57aa3d4fac4561a559542517e4ec5549c21531830c527d45d5ede5e5b9fe1a4fa6b54d0d7304aaf22217eac18a87a4581e0f9038a67c04c3e68639c7f0d8d1cb94487902efe8497c2bc46d70b8e61beaf1c24258ea4c2c3a8a1787bc28a869460"_sd;
    vector.k = "f2af73a805f2e94507d43a6898cd4bfa7883f2cdeef0d9f71a3c758fdf9e19e0"_sd;
    vector.iv = "0da1a042da8346fc46f1f1e7da0d4594"_sd;
    vector.c = "be3fe3a2a124046bcd1c378a36b6cb12111faf90ce066b9f6dd8e4f72089d900add8bc4cf35230f80cb1bba3d720877a5ac14a0601735a2ec482760d85e9009a71c8ffae85c37ad7b4aeaf63150cfef20b2fa82c27d7d722fb7f615b86dbc183624367"_sd;
    vector.r = "0da1a042da8346fc46f1f1e7da0d4594be3fe3a2a124046bcd1c378a36b6cb12111faf90ce066b9f6dd8e4f72089d900add8bc4cf35230f80cb1bba3d720877a5ac14a0601735a2ec482760d85e9009a71c8ffae85c37ad7b4aeaf63150cfef20b2fa82c27d7d722fb7f615b86dbc183624367"_sd;
    // clang-format on
    evaluate(vector);
}

TEST_F(Fle2EncryptTestVectors, Fle2AeadTestCaseLarge) {
    TestVector vector;
    // clang-format off
    vector.d = "04e5f1c0e0a0bad390a61e3bfe0a6c67ab99845c8177763ef8f2086f23500e880cb83f384614ee7af7eddadbeafc6a95145462a1c599a0c192858c57bbafe2df139859cf642d3bcf57c45fc29da11d93292cccf380f8b1bd8e7beff3bb63639e24c51939cffb5865aff85f7b2b8581bb3b6b3de50f71ea1c85f5fd2e1c58c9dd9c3ab05038b7312271423d9aa15ef2c1b5e4e9b4cc96096fe6cee50699cbbde7acd105cc067b641054fc929d88226951d125a6e600bb483092fc07434c76405f9ee3c3f5c46c99af636d53dc49df25e1a058d6b0e94216f29a6b18651c01a7b8f4080df902c1954ba18b120579da8245260ea13cee5a5ca4be780b7bd3ed6214f158f116ec4d98e343b26641a4d4856e957925943832bf30cfb84a44a34fc867ff4790da1f13a5ad8d56459d47ed3546c1cb2d660e22d773482da23a9362e7ce821dc341cf5951f9ad84627a06b34adbd85359ca72cd026713d39470373843f54b58020dd006a42ce78d93ba644516e9cd43d4660c172cf5d2d5a40de761a9d22b941ed34d5e039e10acae264b8fdddfc47e4b20ce0f7a3defeb592a166c30dcf3dec8f79a754eece5da19b847ce9b2bbb3e257a34bc16bf2c1da8c1ef6028444d51e9ff1fa0ba1e7fcf0b19412d5c1b869baf5207d695e2381401034ded0ac00c4c46ab6b06def25b8dba318bc756449a83796afd8af232719e1de8bd5d4d27d31d806b3aeb1e02cdea91feec5d77a8568195ce1beea1c38af1e1e81b56a9460db6173ad74958a5c09284e8dae56f50255f15b0ce1400d650c41f437d6c8ab1d9e58b5970ff924d50c8a95a243d78d1c3910cfd5f7b3beaf097591179e9731ac031387bead4b05b82335d138c8f23c34beaddbd97f5fa7f167c3ca789efd6743489f441206b19184c0664328c27766aad389c83c3a02203e2b362a2985be574fd6f7d7e250f84dbdfeede0a75cfdf447dec726f5a47de33665f7ec68911cd38fe8ef940ea7faa831392481cbb560491708d05a68c94841d8f709300bc2fe2a2192108b97e5e37581cda3580ac3f5883429bccda045b7c0beac21d75c401004a046b1afda702"_sd;
    vector.k = "78d4626904c0d5d76e32426a3db42c763533342884f89de5219f4e46cfab78e1"_sd;
    vector.iv = "4feca0f2860522033cf5c9451c0673f6"_sd;
    vector.c = "ede3594ed590195a45939f362d49475297b53cfe61d2247370c77e75b75a00ab93113c1b7b148efa6d73fbd9b4f5a19057eff8e15ff19ed3392ababcc894d172eb8fdae9c2713a7cb5e76fff1c8763655c0a16730af28498f12f4596c53499fcc25bfdb8c9fcbc90e1f5dbc717fa60c4f545c5f138347df2d9d2d3971dd8e8c0b0559587c660a7c53d21be2a640b77d46690aa9ab37f6b6ca79311bcc9a643d757dfcec74a2f5205b2f2cb1da2625a48bec3db101eee574562be0c348662461a656267be8c044ee548f2233dd6f9909d3d3d7f25403ba754e755ccc9a079054df828bebb8ba32806bdd717aa6f52f9906a58b3fe43f04f764ae2345dbd56c3afa56a29f2ecc030cd4fe4a6a27a557e67c0d854f5e2d418261d30c9ac41d71baf59e3dbcfebd3a72adbec9c44a99fd37a846bec1bf02555bd54482693b39a260f4e64a89c5a1e53f18cf20bc3c94d5a802b906f61c6c68a8577523c57f3edf64174472db1b165c09d95c787d234c12e5711bbadd56fbb0547a33d1f4f08e258855c0b57b52d2377820b8e4d5642da44730c64c7ac7a9de3c3923774504761dabf716db3b5ebeb4522fdd337361c309944539efb78699b589df015e922dc9a1501e90ab6535fbb1428e581f988502528fb447e3774cc4fc0ea1118bb7f4fb6fc8378f711e9c94a19ec3a9fd649fc0938aabf4fe7efc04f247e23d688b8b866cfbaffa5ae84fd29ba0338a341139c11aa317b9951e2071c706cbd63cf6f2d1096e3352d6662a8df510c746831b55c0b65d992fec1fe19eefbca960305ff2e52d1939bf57a5c4c8b17a01ed6a07df5d56db9abdf464b465d726f53714529a08ad132a0da043aefa837f2706b81fd02e8d969fb67940662da28f3337d49967986bd4947bb773fa245752aa8bf2a986d26e1476f73230e6e78d162d82e6393e1f0392d3157ec0a3f26545a51bf74b8fa05bbe67070fddb70473be84f95e0bcf64f66c091885f2291db4d9a52db461f110953a963573dec698a66d1a54003d9aa96937027740c14df2e32dd9ae40a486e3807e826e038be11d36a4a7d899875f7d6e75311f4826c1dde"_sd;
    vector.r = "4feca0f2860522033cf5c9451c0673f6ede3594ed590195a45939f362d49475297b53cfe61d2247370c77e75b75a00ab93113c1b7b148efa6d73fbd9b4f5a19057eff8e15ff19ed3392ababcc894d172eb8fdae9c2713a7cb5e76fff1c8763655c0a16730af28498f12f4596c53499fcc25bfdb8c9fcbc90e1f5dbc717fa60c4f545c5f138347df2d9d2d3971dd8e8c0b0559587c660a7c53d21be2a640b77d46690aa9ab37f6b6ca79311bcc9a643d757dfcec74a2f5205b2f2cb1da2625a48bec3db101eee574562be0c348662461a656267be8c044ee548f2233dd6f9909d3d3d7f25403ba754e755ccc9a079054df828bebb8ba32806bdd717aa6f52f9906a58b3fe43f04f764ae2345dbd56c3afa56a29f2ecc030cd4fe4a6a27a557e67c0d854f5e2d418261d30c9ac41d71baf59e3dbcfebd3a72adbec9c44a99fd37a846bec1bf02555bd54482693b39a260f4e64a89c5a1e53f18cf20bc3c94d5a802b906f61c6c68a8577523c57f3edf64174472db1b165c09d95c787d234c12e5711bbadd56fbb0547a33d1f4f08e258855c0b57b52d2377820b8e4d5642da44730c64c7ac7a9de3c3923774504761dabf716db3b5ebeb4522fdd337361c309944539efb78699b589df015e922dc9a1501e90ab6535fbb1428e581f988502528fb447e3774cc4fc0ea1118bb7f4fb6fc8378f711e9c94a19ec3a9fd649fc0938aabf4fe7efc04f247e23d688b8b866cfbaffa5ae84fd29ba0338a341139c11aa317b9951e2071c706cbd63cf6f2d1096e3352d6662a8df510c746831b55c0b65d992fec1fe19eefbca960305ff2e52d1939bf57a5c4c8b17a01ed6a07df5d56db9abdf464b465d726f53714529a08ad132a0da043aefa837f2706b81fd02e8d969fb67940662da28f3337d49967986bd4947bb773fa245752aa8bf2a986d26e1476f73230e6e78d162d82e6393e1f0392d3157ec0a3f26545a51bf74b8fa05bbe67070fddb70473be84f95e0bcf64f66c091885f2291db4d9a52db461f110953a963573dec698a66d1a54003d9aa96937027740c14df2e32dd9ae40a486e3807e826e038be11d36a4a7d899875f7d6e75311f4826c1dde"_sd;
    // clang-format on
    evaluate(vector);
}

}  // namespace
}  // namespace mongo
