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

#include <algorithm>

#include "mongo/base/data_range.h"

#include "mongo/platform/random.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/hex.h"

#include "aead_encryption.h"

namespace mongo {
namespace {

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

TEST(AEADFLE2, Fle2AeadRandom) {
    mongo::PseudoRandom rnd(SecureRandom().nextInt64());

    std::vector<uint8_t> key(mongo::crypto::kFieldLevelEncryption2KeySize);
    rnd.fill(&key[0], key.size());

    std::vector<uint8_t> associatedData(18);
    rnd.fill(&associatedData[0], associatedData.size());

    std::vector<uint8_t> plainText(956);
    rnd.fill(&plainText[0], plainText.size());
    std::vector<uint8_t> encryptedBytes(plainText.size() + 16 + 32);
    DataRange encrypted(encryptedBytes);

    std::vector<uint8_t> blankIv;
    auto encryptStatus =
        mongo::crypto::fle2AeadEncrypt({key}, {plainText}, {blankIv}, {associatedData}, encrypted);
    ASSERT_OK(encryptStatus);

    auto planTextLength = mongo::fle2AeadGetPlainTextLength(encrypted.length());
    ASSERT_OK(planTextLength);
    ASSERT_EQ(plainText.size(), planTextLength.getValue());

    std::vector<uint8_t> decryptedBytes(planTextLength.getValue());
    auto decryptStatus =
        mongo::crypto::fle2AeadDecrypt({key}, encrypted, associatedData, DataRange(decryptedBytes));
    ASSERT_OK(decryptStatus);
    ASSERT_TRUE(std::equal(
        decryptedBytes.begin(), decryptedBytes.end(), plainText.begin(), plainText.end()));
}

TEST(AEADFLE2, Fle2AeadDetectTampering) {
    mongo::PseudoRandom rnd(SecureRandom().nextInt64());

    std::vector<uint8_t> key(mongo::crypto::kFieldLevelEncryption2KeySize);
    rnd.fill(&key[0], key.size());

    std::vector<uint8_t> associatedData(18);
    rnd.fill(&associatedData[0], associatedData.size());

    std::vector<uint8_t> plainText(956);
    rnd.fill(&plainText[0], plainText.size());
    std::vector<uint8_t> encryptedBytes(plainText.size() + 16 + 32);
    DataRange encrypted(encryptedBytes);

    std::vector<uint8_t> blankIv;
    auto encryptStatus =
        mongo::crypto::fle2AeadEncrypt({key}, {plainText}, {blankIv}, {associatedData}, encrypted);
    ASSERT_OK(encryptStatus);

    auto planTextLength = mongo::fle2AeadGetPlainTextLength(encrypted.length());
    ASSERT_OK(planTextLength);
    ASSERT_EQ(plainText.size(), planTextLength.getValue());

    std::vector<uint8_t> decryptedBytes(planTextLength.getValue());
    auto decryptStatus1 =
        mongo::crypto::fle2AeadDecrypt({key}, encrypted, associatedData, DataRange(decryptedBytes));
    ASSERT_OK(decryptStatus1);

    encryptedBytes[rnd.nextInt32() % encryptedBytes.size()]++;
    auto decryptStatus2 =
        mongo::crypto::fle2AeadDecrypt({key}, encrypted, associatedData, DataRange(decryptedBytes));
    ASSERT_NOT_OK(decryptStatus2);
}

class Fle2AeadTestVectors : public unittest::Test {
public:
    struct TestVector {
        StringData ad;
        StringData c;
        StringData cc;
        StringData iv;
        StringData ke;
        StringData km;
        StringData m;
        StringData s;
        StringData t;
    };

    void evaluate(const TestVector& vector) {
        std::string associatedData = hexblob::decode(vector.ad);
        std::string in = hexblob::decode(vector.m);
        std::string iv = hexblob::decode(vector.iv);
        std::string key = hexblob::decode(vector.ke) + hexblob::decode(vector.km);
        std::string plainText = hexblob::decode(vector.m);

        {
            // Test with IV provided with test vector. Verify intermediate values
            std::vector<uint8_t> encryptedBytes(in.length() ? in.length() + 16 + 32 : 0);
            DataRange encrypted(encryptedBytes);

            auto encryptStatus =
                mongo::crypto::fle2AeadEncrypt({key}, {in}, {iv}, {associatedData}, encrypted);
            ASSERT_OK(encryptStatus);

            std::string expect = hexblob::decode(vector.c);
            ASSERT_EQ(expect.size(), encrypted.length());
            ASSERT_TRUE(std::equal(encryptedBytes.begin(),
                                   encryptedBytes.end(),
                                   reinterpret_cast<uint8_t*>(expect.data())));

            auto planTextLength = mongo::fle2AeadGetPlainTextLength(encrypted.length());
            ASSERT_OK(planTextLength);
            ASSERT_EQ(plainText.length(), planTextLength.getValue());

            std::vector<uint8_t> decryptedBytes(planTextLength.getValue());
            auto decryptStatus = mongo::crypto::fle2AeadDecrypt(
                {key}, encrypted, associatedData, DataRange(decryptedBytes));
            ASSERT_OK(decryptStatus);
            ASSERT_EQ(plainText.length(), decryptStatus.getValue());
            ASSERT_TRUE(std::equal(decryptedBytes.begin(),
                                   decryptedBytes.end(),
                                   reinterpret_cast<uint8_t*>(plainText.data())));
        }
        {
            // Test with random IV. Intermediate values are undetermined
            std::vector<uint8_t> encryptedBytes(in.length() ? in.length() + 16 + 32 : 0);
            DataRange encrypted(encryptedBytes);

            std::vector<uint8_t> blankIv;
            auto encryptStatus =
                mongo::crypto::fle2AeadEncrypt({key}, {in}, {blankIv}, {associatedData}, encrypted);
            ASSERT_OK(encryptStatus);

            auto planTextLength = mongo::fle2AeadGetPlainTextLength(encrypted.length());
            ASSERT_OK(planTextLength);
            ASSERT_EQ(plainText.length(), planTextLength.getValue());

            std::vector<uint8_t> decryptedBytes(planTextLength.getValue());
            auto decryptStatus = mongo::crypto::fle2AeadDecrypt(
                {key}, encrypted, associatedData, DataRange(decryptedBytes));
            ASSERT_OK(decryptStatus);
            ASSERT_EQ(plainText.length(), decryptStatus.getValue());
            ASSERT_TRUE(std::equal(decryptedBytes.begin(),
                                   decryptedBytes.end(),
                                   reinterpret_cast<uint8_t*>(plainText.data())));
        }
    }
};

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
    evaluate(vector);
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
    evaluate(vector);
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
    evaluate(vector);
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
    evaluate(vector);
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
    evaluate(vector);
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
    evaluate(vector);
}

}  // namespace
}  // namespace mongo
