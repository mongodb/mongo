/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <queue>

#include "mongo/crypto/block_packer.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/hex.h"

namespace mongo {
namespace crypto {

std::vector<uint8_t> generateByteSequence(uint8_t start, uint8_t count) {
    std::vector<uint8_t> result;
    for (uint8_t i = start; i < count; i++) {
        result.push_back(i);
    }
    return result;
}

class MockPackerCallback {
public:
    ~MockPackerCallback() {
        ASSERT(_invokations.empty());
    }

    StatusWith<size_t> operator()(ConstDataRange input) {
        ASSERT_GT(input.length(), 0);
        ASSERT(input.length() % 16 == 0);
        ASSERT(_invokations.size() > 0);

        uint8_t front = _invokations.front();
        _invokations.pop();
        ASSERT_EQ(front, input.length());
        auto expectedBuffer = generateByteSequence(index, input.length());
        index += input.length();
        ASSERT(std::equal(expectedBuffer.begin(), expectedBuffer.end(), input.data()));

        return 0;
    }

    void expects(std::initializer_list<uint8_t> expectations) {
        _invokations = std::queue<uint8_t>(std::move(expectations));
        ASSERT(_invokations.size() > 0);
    }

private:
    size_t index = 0;
    std::queue<uint8_t> _invokations;
};

// Test that a Packer with no input has no leftover bytes
TEST(BlockPacker, NoInput) {
    BlockPacker packer;
    auto leftovers = packer.getBlock();
    ASSERT(leftovers.empty());
}

// Test that all writes that fill up to a single block
// produce no callback invokations
TEST(BlockPacker, SingleWriteBlock) {
    for (int i = 0; i <= 16; i++) {
        MockPackerCallback callback;
        BlockPacker packer;

        auto buffer = generateByteSequence(0, i);
        ASSERT_OK(packer.pack(ConstDataRange(buffer), callback));
        auto leftovers = packer.getBlock();
        ASSERT_EQ(i, leftovers.length());
    }
}

// Test a single write which overflows a block
TEST(BlockPacker, SingleWriteBiggerThanBlock) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 17);
    callback.expects({16});
    ASSERT_OK(packer.pack(ConstDataRange(buffer), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(1, leftovers.length());
}

// Test a single write which perfectly fills multiple blocks
TEST(BlockPacker, SingleWriteMultiBlock) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 32);
    callback.expects({16});
    ASSERT_OK(packer.pack(ConstDataRange(buffer), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(16, leftovers.length());
}

// Test a single write which fills multiple blocks, and then overflows the last
TEST(BlockPacker, MultiBlockInputPlus) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 33);
    callback.expects({32});
    ASSERT_OK(packer.pack(ConstDataRange(buffer), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(1, leftovers.length());
}

// Given an initial partially filled block...
// ... Try writing insufficient data to fill the block
TEST(BlockPacker, PrepackedLessThanBlock) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 2);
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data(), buffer.data() + 1), callback));
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data() + 1, buffer.data() + 2), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(2, leftovers.length());
}

// ... Try writing just enough data to fill the block
TEST(BlockPacker, PrepackedFillBlock) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 16);
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data(), buffer.data() + 2), callback));
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data() + 2, buffer.data() + 16), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(16, leftovers.length());
}

// .. Try writing more data than is needed to fill the block
TEST(BlockPacker, PrepackedMoreThanBlock) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 20);
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data(), buffer.data() + 10), callback));
    callback.expects({16});
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data() + 10, buffer.data() + 20), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(4, leftovers.length());
}

// ... Try writing enough data to fill the block, and the next block.
TEST(BlockPacker, BlockAlignPrepackedBuffer) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 32);
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data(), buffer.data() + 6), callback));
    callback.expects({16});
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data() + 6, buffer.data() + 32), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(16, leftovers.length());
}

// ... Try writing enough data to fill the block, the next block, and then partial fill the next.
TEST(BlockPacker, OverflowNewBlockWithPrepackedBuffer) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 33);
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data(), buffer.data() + 6), callback));
    callback.expects({16, 16});
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data() + 6, buffer.data() + 33), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(1, leftovers.length());
}

// ... Try writing enough data to fill the block, then several more.
TEST(BlockPacker, OverflowMultipleBlocksWithPrepackedBuffer) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 64);
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data(), buffer.data() + 6), callback));
    callback.expects({16, 32});
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data() + 6, buffer.data() + 64), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(16, leftovers.length());
}

// Given an initial write which overflows a block...

// ... Try writing insufficient data to align the next block
TEST(BlockPacker, OverflowedThenUnderfill) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 31);
    callback.expects({16});
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data(), buffer.data() + 17), callback));
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data() + 17, buffer.data() + 31), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(15, leftovers.length());
}

// ... Try writing enough data to align the next block
TEST(BlockPacker, OverflowedThenAlign) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 32);
    callback.expects({16});
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data(), buffer.data() + 17), callback));
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data() + 17, buffer.data() + 32), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(16, leftovers.length());
}

// ... Try writing enough data to overflow the next block
TEST(BlockPacker, OverflowedThenOverflow) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 33);
    callback.expects({16});
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data(), buffer.data() + 17), callback));
    callback.expects({16});
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data() + 17, buffer.data() + 33), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(1, leftovers.length());
}

// Given an initial write of one block...

// ... Try writing insufficient data to align the next block
TEST(BlockPacker, AlignedThenUnderfill) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 31);
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data(), buffer.data() + 16), callback));
    callback.expects({16});
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data() + 16, buffer.data() + 31), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(15, leftovers.length());
}

// ... Try writing enough data to align the next block
TEST(BlockPacker, AlignedThenAlign) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 32);
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data(), buffer.data() + 16), callback));
    callback.expects({16});
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data() + 16, buffer.data() + 32), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(16, leftovers.length());
}

// ... Try writing enough data to overfill the next block
TEST(BlockPacker, AlignedThenOverfill) {
    MockPackerCallback callback;
    BlockPacker packer;

    auto buffer = generateByteSequence(0, 33);
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data(), buffer.data() + 16), callback));
    callback.expects({16, 16});
    ASSERT_OK(packer.pack(ConstDataRange(buffer.data() + 16, buffer.data() + 33), callback));
    auto leftovers = packer.getBlock();
    ASSERT_EQ(1, leftovers.length());
}


// The following tests validate that SymmetricEncryptors function when called with inputs with
// varying block alignments.
TEST(SymmetricEncryptor, PaddingLogic) {
    SymmetricKey key = crypto::aesGenerate(crypto::sym256KeySize, "SymmetricEncryptorTest");
    const std::array<uint8_t, 16> iv = {};
    std::array<std::uint8_t, 1024> cryptoBuffer;
    DataRange cryptoRange(cryptoBuffer.data(), cryptoBuffer.size());
    // This array defines a series of different test cases. Each sub-array defines a series of calls
    // to encryptor->update(). Each number represents the number of bytes to pass to update in a
    // single call
    const std::vector<std::vector<uint8_t>> testData = {
        // Pre-condition: blockBuffer is empty
        {0x01, 0x10},
        {0x10},
        {0x20},
        {0x05, 0x30},
        {
            0x02,
            0x07,
        },
        {
            0x0B,
        },
        {
            0x15,
            0x26,
        },
    };

    // We will loop through all of the test cases, ensuring no fatal errors,
    // and ensuring correct encryption and decryption.
    for (auto& testCase : testData) {
        auto swEnc = crypto::SymmetricEncryptor::create(key, crypto::aesMode::cbc, iv);
        ASSERT_OK(swEnc.getStatus());
        auto encryptor = std::move(swEnc.getValue());
        DataRangeCursor cryptoCursor(cryptoRange);
        // Make subsequent calls to encryptor->update() as defined by the test data
        std::vector<uint8_t> accumulatedPlaintext;
        for (auto& updateBytes : testCase) {
            std::vector<uint8_t> plainText(updateBytes, updateBytes);
            std::copy(plainText.begin(), plainText.end(), std::back_inserter(accumulatedPlaintext));
            auto swSize = encryptor->update(plainText, cryptoCursor);
            ASSERT_OK(swSize);
            cryptoCursor.advance(swSize.getValue());
        }
        auto swSize = encryptor->finalize(cryptoCursor);
        ASSERT_OK(swSize);

        // finalize is guaranteed to output at least 16 bytes for the CBC blockmode
        ASSERT_GTE(swSize.getValue(), 16);
        cryptoCursor.advance(swSize.getValue());

        // Validate the length of the ciphertext is plausible for the plaintext
        auto totalSize = std::accumulate(testCase.begin(), testCase.end(), 0);
        auto totalBlocks = totalSize / 16 + 1;
        ASSERT_EQ(totalBlocks * 16, cryptoRange.length() - cryptoCursor.length());

        // Validate that the ciphertext can be decrypted
        auto swDec = crypto::SymmetricDecryptor::create(key, crypto::aesMode::cbc, iv);
        ASSERT_OK(swDec.getStatus());
        auto decryptor = std::move(swDec.getValue());
        std::array<uint8_t, 1024> decryptionBuffer;
        DataRangeCursor decryptionCursor(decryptionBuffer);
        auto swUpdateSize = decryptor->update(
            {cryptoRange.data(), cryptoRange.length() - cryptoCursor.length()}, decryptionCursor);
        ASSERT_OK(swUpdateSize.getStatus());
        decryptionCursor.advance(swUpdateSize.getValue());
        auto swFinalizeSize = decryptor->finalize(decryptionCursor);
        ASSERT_OK(swUpdateSize.getStatus());
        decryptionCursor.advance(swUpdateSize.getValue());
        ASSERT_EQ(totalSize, swUpdateSize.getValue() + swFinalizeSize.getValue());

        // Validate that the decrypted ciphertext matches the original plaintext
        ASSERT(std::equal(
            accumulatedPlaintext.begin(), accumulatedPlaintext.end(), decryptionBuffer.begin()));
    }
}

SymmetricKey aesGeneratePredictableKey256(StringData stringKey, StringData keyId) {
    const size_t keySize = crypto::sym256KeySize;
    ASSERT_EQ(keySize, stringKey.size());

    SecureVector<uint8_t> key(keySize);
    std::copy(stringKey.begin(), stringKey.end(), key->begin());

    return SymmetricKey(std::move(key), crypto::aesAlgorithm, keyId.toString());
}

// Convenience wrappers to avoid line-wraps later.
const char* asChar(const std::uint8_t* data) {
    return reinterpret_cast<const char*>(data);
};

// Positive/Negative test for additional authenticated data GCM encryption.
// Setup encryptor/decryptor with fixed key/iv/aad in order to produce predictable results.
// Check roundtrip and that tag violation triggers failure.
void GCMAdditionalAuthenticatedDataHelper(bool succeed) {
    const auto mode = crypto::aesMode::gcm;
    if (!crypto::getSupportedSymmetricAlgorithms().count(getStringFromCipherMode(mode))) {
        return;
    }

    constexpr auto kKey = "abcdefghijklmnopABCDEFGHIJKLMNOP"_sd;
    SymmetricKey key = aesGeneratePredictableKey256(kKey, "testID");

    constexpr auto kIV = "FOOBARbazqux"_sd;
    std::array<std::uint8_t, 12> iv;
    std::copy(kIV.begin(), kIV.end(), iv.begin());

    auto encryptor = uassertStatusOK(crypto::SymmetricEncryptor::create(key, mode, iv));

    constexpr auto kAAD = "Hello World"_sd;
    ASSERT_OK(encryptor->addAuthenticatedData({kAAD.rawData(), kAAD.size()}));

    constexpr auto kPlaintextMessage = "01234567012345670123456701234567"_sd;
    constexpr auto kBufferSize = kPlaintextMessage.size() + (2 * crypto::aesBlockSize);
    std::array<std::uint8_t, kBufferSize> cipherText;
    std::size_t cipherLen = 0;
    {
        DataRangeCursor cipherTextCursor(cipherText);
        cipherLen = uassertStatusOK(encryptor->update(
            {kPlaintextMessage.rawData(), kPlaintextMessage.size()}, cipherTextCursor));
        cipherTextCursor.advance(cipherLen);
        cipherLen += uassertStatusOK(encryptor->finalize(cipherTextCursor));
    }

    constexpr auto kExpectedCipherText =
        "\xF1\x87\x38\x92\xA3\x0E\x77\x27\x92\xB1\x3B\xA6\x27\xB5\xF5\x2B"
        "\xA0\x16\xCC\xB8\x88\x54\xC0\x06\x6E\x36\xCF\x3B\xB0\x8B\xF5\x11";
    ASSERT_EQ(StringData(asChar(cipherText.data()), cipherLen), kExpectedCipherText);

    std::array<std::uint8_t, 12> tag;
    const auto taglen = uassertStatusOK(encryptor->finalizeTag({tag}));

    constexpr auto kExpectedTag = "\xF9\xD6\xF9\x63\x21\x93\xE8\x5C\x42\xAA\x5E\x02"_sd;
    ASSERT_EQ(StringData(asChar(tag.data()), taglen), kExpectedTag);

    auto decryptor = uassertStatusOK(crypto::SymmetricDecryptor::create(key, mode, iv));
    ASSERT_OK(decryptor->addAuthenticatedData({kAAD.rawData(), kAAD.size()}));

    std::array<std::uint8_t, kBufferSize> plainText;
    auto plainLen = uassertStatusOK(decryptor->update({cipherText.data(), cipherLen}, plainText));

    if (!succeed) {
        // Corrupt the authenticated tag, which should cause a failure below.
        ++tag[0];
    }

    ASSERT_OK(decryptor->updateTag(tag));
    auto swFinalize =
        decryptor->finalize({plainText.data() + plainLen, plainText.size() - plainLen});

    if (!succeed) {
        ASSERT_NOT_OK(swFinalize.getStatus());
        return;
    }

    ASSERT_OK(swFinalize.getStatus());
    plainLen += swFinalize.getValue();

    ASSERT_EQ(StringData(asChar(plainText.data()), plainLen), kPlaintextMessage);
}

TEST(AES, GCMAdditionalAuthenticatedData) {
    GCMAdditionalAuthenticatedDataHelper(true);
    GCMAdditionalAuthenticatedDataHelper(false);
}

class AESTestVectors : public unittest::Test {
public:
    class GCMTestVector {
    public:
        GCMTestVector(StringData key,
                      StringData plaintext,
                      StringData a,
                      StringData iv,
                      StringData ciphertext,
                      StringData tag) {
            this->key = hexblob::decode(key);
            this->plaintext = hexblob::decode(plaintext);
            this->a = hexblob::decode(a);
            this->iv = hexblob::decode(iv);
            this->ciphertext = hexblob::decode(ciphertext);
            this->tag = hexblob::decode(tag);
        }

        std::string key;
        std::string plaintext;
        std::string a;
        std::string iv;
        std::string ciphertext;
        std::string tag;
    };

    class CTRTestVector {
    public:
        CTRTestVector(StringData key, StringData plaintext, StringData iv, StringData ciphertext) {
            this->key = hexblob::decode(key);
            this->plaintext = hexblob::decode(plaintext);
            this->iv = hexblob::decode(iv);
            this->ciphertext = hexblob::decode(ciphertext);
        }

        std::string key;
        std::string plaintext;
        std::string iv;
        std::string ciphertext;
    };

    void evaluate(GCMTestVector test) {
        constexpr auto mode = crypto::aesMode::gcm;

        if (getSupportedSymmetricAlgorithms().count(getStringFromCipherMode(mode)) == 0) {
            return;
        }

        SymmetricKey key = aesGeneratePredictableKey256(test.key, "testID");

        // Validate encryption
        auto encryptor = uassertStatusOK(crypto::SymmetricEncryptor::create(key, mode, test.iv));

        ASSERT_OK(encryptor->addAuthenticatedData(test.a));

        const size_t kBufferSize = test.plaintext.size();
        {
            // Validate encryption
            std::vector<uint8_t> encryptionResult(kBufferSize);
            auto cipherLen = uassertStatusOK(encryptor->update(test.plaintext, encryptionResult));
            cipherLen += uassertStatusOK(encryptor->finalize(
                {encryptionResult.data() + cipherLen, encryptionResult.size() - cipherLen}));

            ASSERT_EQ(test.ciphertext.size(), cipherLen);
            ASSERT_EQ(hexblob::encode(test.ciphertext),
                      hexblob::encode(
                          StringData(asChar(encryptionResult.data()), encryptionResult.size())));

            // The symmetric crypto framework uses 12 byte GCM tags. The tags used in NIST test
            // vectors can be larger than 12 bytes, but may be truncated.

            std::array<std::uint8_t, 12> tag;
            const auto taglen = uassertStatusOK(encryptor->finalizeTag(tag));
            ASSERT_EQ(tag.size(), taglen);
            ASSERT_EQ(hexblob::encode(StringData(test.tag.data(), test.tag.size()))
                          .substr(0, aesGCMTagSize * 2),
                      hexblob::encode(StringData(asChar(tag.data()), tag.size())));
        }
        {
            // Validate decryption
            auto decryptor =
                uassertStatusOK(crypto::SymmetricDecryptor::create(key, mode, test.iv));
            uassertStatusOK(decryptor->updateTag({test.tag.data(), aesGCMTagSize}));

            ASSERT_OK(decryptor->addAuthenticatedData(test.a));
            std::vector<uint8_t> decryptionResult(kBufferSize);
            auto decipherLen =
                uassertStatusOK(decryptor->update(test.ciphertext, decryptionResult));
            decipherLen += uassertStatusOK(decryptor->finalize(
                {decryptionResult.data() + decipherLen, decryptionResult.size() - decipherLen}));

            ASSERT_EQ(test.plaintext.size(), decipherLen);
            ASSERT_EQ(hexblob::encode(StringData(test.plaintext.data(), test.plaintext.size())),
                      hexblob::encode(
                          StringData(asChar(decryptionResult.data()), decryptionResult.size())));
        }
        {
            // Validate that decryption with incorrect tag does not succeed
            auto decryptor =
                uassertStatusOK(crypto::SymmetricDecryptor::create(key, mode, test.iv));
            auto tag = test.tag;
            tag[0]++;
            uassertStatusOK(decryptor->updateTag({tag.data(), aesGCMTagSize}));

            ASSERT_OK(decryptor->addAuthenticatedData(test.a));
            std::vector<uint8_t> decryptionResult(kBufferSize);
            DataRangeCursor decryptionResultCursor(decryptionResult);
            auto decipherLen =
                uassertStatusOK(decryptor->update(test.ciphertext, decryptionResultCursor));
            decryptionResultCursor.advance(decipherLen);
            ASSERT_NOT_OK(decryptor->finalize(decryptionResultCursor));
        }
    }

    void evaluate(CTRTestVector test) {
        constexpr auto mode = crypto::aesMode::ctr;

        if (getSupportedSymmetricAlgorithms().count(getStringFromCipherMode(mode)) == 0) {
            return;
        }

        SymmetricKey key = aesGeneratePredictableKey256(test.key, "testID");

        // Validate encryption
        auto encryptor = uassertStatusOK(crypto::SymmetricEncryptor::create(key, mode, test.iv));

        const size_t kBufferSize = test.plaintext.size();
        {
            // Validate encryption
            std::vector<uint8_t> encryptionResult(kBufferSize);
            auto cipherLen = uassertStatusOK(encryptor->update(test.plaintext, encryptionResult));
            cipherLen += uassertStatusOK(encryptor->finalize(
                {encryptionResult.data() + cipherLen, encryptionResult.size() - cipherLen}));

            ASSERT_EQ(test.ciphertext.size(), cipherLen);
            ASSERT_EQ(hexblob::encode(test.ciphertext),
                      hexblob::encode(
                          StringData(asChar(encryptionResult.data()), encryptionResult.size())));
        }
        {
            // Validate decryption
            auto decryptor =
                uassertStatusOK(crypto::SymmetricDecryptor::create(key, mode, test.iv));

            std::vector<uint8_t> decryptionResult(kBufferSize);
            auto decipherLen =
                uassertStatusOK(decryptor->update(test.ciphertext, decryptionResult));
            decipherLen += uassertStatusOK(decryptor->finalize(
                {decryptionResult.data() + decipherLen, decryptionResult.size() - decipherLen}));

            ASSERT_EQ(test.plaintext.size(), decipherLen);
            ASSERT_EQ(hexblob::encode(StringData(test.plaintext.data(), test.plaintext.size())),
                      hexblob::encode(
                          StringData(asChar(decryptionResult.data()), decryptionResult.size())));
        }
    }
};

/** Test vectors drawn from
 *  https://csrc.nist.rip/groups/ST/toolkit/BCM/documents/proposedmodes/gcm/gcm-spec.pdf
 */
TEST_F(AESTestVectors, GCMTestCase13) {
    evaluate(
        GCMTestVector("00000000000000000000000000000000"
                      "00000000000000000000000000000000"_sd,
                      ""_sd,
                      ""_sd,
                      "000000000000000000000000"_sd,
                      ""_sd,
                      "530f8afbc74536b9a963b4f1c4cb738b"_sd));
}

TEST_F(AESTestVectors, GCMTestCase14) {
    evaluate(
        GCMTestVector("00000000000000000000000000000000"
                      "00000000000000000000000000000000"_sd,
                      "00000000000000000000000000000000"_sd,
                      ""_sd,
                      "000000000000000000000000"_sd,
                      "cea7403d4d606b6e074ec5d3baf39d18"_sd,
                      "d0d1c8a799996bf0265b98b5d48ab919"_sd));
}

TEST_F(AESTestVectors, GCMTestCase15) {
    evaluate(
        GCMTestVector("feffe9928665731c6d6a8f9467308308"
                      "feffe9928665731c6d6a8f9467308308"_sd,
                      "d9313225f88406e5a55909c5aff5269a"
                      "86a7a9531534f7da2e4c303d8a318a72"
                      "1c3c0c95956809532fcf0e2449a6b525"
                      "b16aedf5aa0de657ba637b391aafd255"_sd,
                      ""_sd,
                      "cafebabefacedbaddecaf888"_sd,
                      "522dc1f099567d07f47f37a32a84427d"
                      "643a8cdcbfe5c0c97598a2bd2555d1aa"
                      "8cb08e48590dbb3da7b08b1056828838"
                      "c5f61e6393ba7a0abcc9f662898015ad"_sd,
                      "b094dac5d93471bdec1a502270e3cc6c"_sd));
}

TEST_F(AESTestVectors, GCMTestCase16) {
    evaluate(
        GCMTestVector("feffe9928665731c6d6a8f9467308308"
                      "feffe9928665731c6d6a8f9467308308"_sd,
                      "d9313225f88406e5a55909c5aff5269a"
                      "86a7a9531534f7da2e4c303d8a318a72"
                      "1c3c0c95956809532fcf0e2449a6b525"
                      "b16aedf5aa0de657ba637b39"_sd,
                      "feedfacedeadbeeffeedfacedeadbeef"
                      "abaddad2"_sd,
                      "cafebabefacedbaddecaf888"_sd,
                      "522dc1f099567d07f47f37a32a84427d"
                      "643a8cdcbfe5c0c97598a2bd2555d1aa"
                      "8cb08e48590dbb3da7b08b1056828838"
                      "c5f61e6393ba7a0abcc9f662"_sd,
                      "76fc6ece0f4e1768cddf8853bb2d551b"_sd));
}

// AES-CTR test vectors are obtained here:
// https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38a.pdf

TEST_F(AESTestVectors, CTRTestCase1) {
    evaluate(
        CTRTestVector("603deb1015ca71be2b73aef0857d7781"
                      "1f352c073b6108d72d9810a30914dff4",
                      "6bc1bee22e409f96e93d7e117393172a",
                      "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
                      "601ec313775789a5b7a7f504bbf3d228"));
}

TEST_F(AESTestVectors, CTRTestCase2) {
    evaluate(
        CTRTestVector("603deb1015ca71be2b73aef0857d7781"
                      "1f352c073b6108d72d9810a30914dff4",
                      "ae2d8a571e03ac9c9eb76fac45af8e51",
                      "f0f1f2f3f4f5f6f7f8f9fafbfcfdff00",
                      "f443e3ca4d62b59aca84e990cacaf5c5"));
}

TEST_F(AESTestVectors, CTRTestCase3) {
    evaluate(
        CTRTestVector("603deb1015ca71be2b73aef0857d7781"
                      "1f352c073b6108d72d9810a30914dff4",
                      "30c81c46a35ce411e5fbc1191a0a52ef",
                      "f0f1f2f3f4f5f6f7f8f9fafbfcfdff01",
                      "2b0930daa23de94ce87017ba2d84988d"));
}

TEST_F(AESTestVectors, CTRTestCase4) {
    evaluate(
        CTRTestVector("603deb1015ca71be2b73aef0857d7781"
                      "1f352c073b6108d72d9810a30914dff4",
                      "f69f2445df4f9b17ad2b417be66c3710",
                      "f0f1f2f3f4f5f6f7f8f9fafbfcfdff02",
                      "dfc9c58db67aada613c2dd08457941a6"));
}

TEST_F(AESTestVectors, CTRTestCase1234) {
    evaluate(
        CTRTestVector("603deb1015ca71be2b73aef0857d7781"
                      "1f352c073b6108d72d9810a30914dff4",

                      "6bc1bee22e409f96e93d7e117393172a"
                      "ae2d8a571e03ac9c9eb76fac45af8e51"
                      "30c81c46a35ce411e5fbc1191a0a52ef"
                      "f69f2445df4f9b17ad2b417be66c3710",

                      "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",

                      "601ec313775789a5b7a7f504bbf3d228"
                      "f443e3ca4d62b59aca84e990cacaf5c5"
                      "2b0930daa23de94ce87017ba2d84988d"
                      "dfc9c58db67aada613c2dd08457941a6"));
}

// The tests vectors below are generated using random data. Since they do not contain logic,
// we will have them in a separate file so that they do not overtake the code space
#include "symmetric_crypto_tests.gen"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


}  // namespace crypto
}  // namespace mongo
