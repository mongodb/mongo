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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <queue>

#include "mongo/crypto/block_packer.h"
#include "mongo/unittest/unittest.h"

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
        auto swEnc =
            crypto::SymmetricEncryptor::create(key, crypto::aesMode::cbc, iv.data(), iv.size());
        ASSERT_OK(swEnc.getStatus());
        auto encryptor = std::move(swEnc.getValue());
        DataRangeCursor cryptoCursor(cryptoRange);
        // Make subsequent calls to encryptor->update() as defined by the test data
        std::vector<uint8_t> accumulatedPlaintext;
        for (auto& updateBytes : testCase) {
            std::vector<uint8_t> plainText(updateBytes, updateBytes);
            std::copy(plainText.begin(), plainText.end(), std::back_inserter(accumulatedPlaintext));
            auto swSize = encryptor->update(plainText.data(),
                                            plainText.size(),
                                            const_cast<uint8_t*>(cryptoCursor.data<uint8_t>()),
                                            cryptoCursor.length());
            ASSERT_OK(swSize);
            cryptoCursor.advance(swSize.getValue());
        }
        auto swSize = encryptor->finalize(const_cast<uint8_t*>(cryptoCursor.data<uint8_t>()),
                                          cryptoCursor.length());
        ASSERT_OK(swSize);

        // finalize is guaranteed to output at least 16 bytes for the CBC blockmode
        ASSERT_GTE(swSize.getValue(), 16);
        cryptoCursor.advance(swSize.getValue());

        // Validate the length of the ciphertext is plausible for the plaintext
        auto totalSize = std::accumulate(testCase.begin(), testCase.end(), 0);
        auto totalBlocks = totalSize / 16 + 1;
        ASSERT_EQ(totalBlocks * 16, cryptoRange.length() - cryptoCursor.length());

        // Validate that the ciphertext can be decrypted
        auto swDec =
            crypto::SymmetricDecryptor::create(key, crypto::aesMode::cbc, iv.data(), iv.size());
        ASSERT_OK(swDec.getStatus());
        auto decryptor = std::move(swDec.getValue());
        std::array<uint8_t, 1024> decryptionBuffer;
        DataRangeCursor decryptionCursor(decryptionBuffer);
        auto swUpdateSize =
            decryptor->update(cryptoRange.data<uint8_t>(),
                              cryptoRange.length() - cryptoCursor.length(),
                              const_cast<uint8_t*>(decryptionCursor.data<uint8_t>()),
                              decryptionCursor.length());
        ASSERT_OK(swUpdateSize.getStatus());
        decryptionCursor.advance(swUpdateSize.getValue());
        auto swFinalizeSize = decryptor->finalize(
            const_cast<uint8_t*>(decryptionCursor.data<uint8_t>()), decryptionCursor.length());
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
const std::uint8_t* asUint8(const char* str) {
    return reinterpret_cast<const std::uint8_t*>(str);
};

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

    auto encryptor =
        uassertStatusOK(crypto::SymmetricEncryptor::create(key, mode, iv.data(), iv.size()));

    constexpr auto kAAD = "Hello World"_sd;
    ASSERT_OK(encryptor->addAuthenticatedData(asUint8(kAAD.rawData()), kAAD.size()));

    constexpr auto kPlaintextMessage = "01234567012345670123456701234567"_sd;
    constexpr auto kBufferSize = kPlaintextMessage.size() + (2 * crypto::aesBlockSize);
    std::array<std::uint8_t, kBufferSize> cipherText;
    auto cipherLen = uassertStatusOK(encryptor->update(asUint8(kPlaintextMessage.rawData()),
                                                       kPlaintextMessage.size(),
                                                       cipherText.data(),
                                                       cipherText.size()));
    cipherLen += uassertStatusOK(
        encryptor->finalize(cipherText.data() + cipherLen, cipherText.size() - cipherLen));

    constexpr auto kExpectedCipherText =
        "\xF1\x87\x38\x92\xA3\x0E\x77\x27\x92\xB1\x3B\xA6\x27\xB5\xF5\x2B"
        "\xA0\x16\xCC\xB8\x88\x54\xC0\x06\x6E\x36\xCF\x3B\xB0\x8B\xF5\x11";
    ASSERT_EQ(StringData(asChar(cipherText.data()), cipherLen), kExpectedCipherText);

    std::array<std::uint8_t, 12> tag;
    const auto taglen = uassertStatusOK(encryptor->finalizeTag(tag.data(), tag.size()));

    constexpr auto kExpectedTag = "\xF9\xD6\xF9\x63\x21\x93\xE8\x5C\x42\xAA\x5E\x02"_sd;
    ASSERT_EQ(StringData(asChar(tag.data()), taglen), kExpectedTag);

    auto decryptor =
        uassertStatusOK(crypto::SymmetricDecryptor::create(key, mode, iv.data(), iv.size()));
    ASSERT_OK(decryptor->addAuthenticatedData(asUint8(kAAD.rawData()), kAAD.size()));

    std::array<std::uint8_t, kBufferSize> plainText;
    auto plainLen = uassertStatusOK(
        decryptor->update(cipherText.data(), cipherLen, plainText.data(), plainText.size()));

    if (!succeed) {
        // Corrupt the authenticated tag, which should cause a failure below.
        ++tag[0];
    }

    ASSERT_OK(decryptor->updateTag(tag.data(), tag.size()));
    auto swFinalize = decryptor->finalize(plainText.data() + plainLen, plainText.size() - plainLen);

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

}  // namespace crypto
}  // namespace mongo
