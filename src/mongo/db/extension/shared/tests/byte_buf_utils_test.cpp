// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/shared/byte_buf_utils.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

namespace mongo::extension {
namespace {

std::vector<char> generateBuffer(int32_t size) {
    std::vector<char> buffer(size);
    DataRange bufferRange(&buffer.front(), &buffer.back());
    ASSERT_OK(bufferRange.writeNoThrow(LittleEndian<int32_t>(size)));

    return buffer;
}

TEST(ByteBufUtilsTest, BsonObjFromByteViewValidInput) {
    // Create a valid BSON object in byte array form
    const auto validBSON = BSON("field" << "value");

    ASSERT_TRUE(validBSON.objsize() >= 0);
    MongoExtensionByteView view{reinterpret_cast<const uint8_t*>(validBSON.objdata()),
                                static_cast<size_t>(validBSON.objsize())};
    auto bsonObj = bsonObjFromByteView(view);
    ASSERT_TRUE(bsonObj.isValid());  // Check BSON is logically valid
    ASSERT_EQUALS(std::string(bsonObj.getStringField("field")), "value");  // Verify field content
}

DEATH_TEST(ByteBufUtilsTestDeathTest, BsonObjFromByteView_InvalidInput_InsufficientLength, "518") {
    MongoExtensionByteView view{nullptr, BSONObj::kMinBSONLength - 1};
    [[maybe_unused]] auto bsonObj = bsonObjFromByteView(view);
}

DEATH_TEST(ByteBufUtilsTestDeathTest, BsonObjFromByteView_InvalidInput_MalformedLength, "518") {
    const uint8_t malformedBsonData[] = {
        0xFF,
        0xFF,
        0xFF,
        0xFF,  // Invalid length (negative or oversized)
    };
    MongoExtensionByteView view{malformedBsonData, 4};
    [[maybe_unused]] auto bsonObj = bsonObjFromByteView(view);
}

DEATH_TEST(ByteBufUtilsTestDeathTest,
           BsonObjFromByteView_InvalidInput_LengthExceedsBufferSize,
           "518") {
    const auto validBSON = BSON("field" << "value");
    const auto objSize = validBSON.objsize();
    DataRange bufferRange(const_cast<char*>(validBSON.objdata()), objSize);
    ASSERT_TRUE(objSize >= 0);
    // Mutate document to report incorrect size of 32.
    ASSERT_OK(bufferRange.writeNoThrow(LittleEndian<int32_t>(32)));
    MongoExtensionByteView view{
        reinterpret_cast<const uint8_t*>(validBSON.objdata()),
        static_cast<size_t>(
            objSize)};  // Only 16 bytes in buffer, while document length states 32 bytes.

    [[maybe_unused]] auto bsonObj = bsonObjFromByteView(view);
}

TEST(ByteBufUtilsTest, BsonObjFromByteView_ValidInput_MinimumLength) {
    auto smallBsonBuffer = generateBuffer(BSONObj::kMinBSONLength);
    MongoExtensionByteView view{reinterpret_cast<const uint8_t*>(smallBsonBuffer.data()),
                                BSONObj::kMinBSONLength};

    auto bsonObj = bsonObjFromByteView(view);
    ASSERT_TRUE(bsonObj.isValid());  // Check BSON is logically valid.
}

DEATH_TEST(ByteBufUtilsTestDeathTest, BsonObjFromByteView_InvalidInput_Empty, "518") {
    MongoExtensionByteView view{nullptr, 0};
    [[maybe_unused]] auto bsonObj = bsonObjFromByteView(view);
}

TEST(ByteBufUtilsTest, BsonObjFromByteView_ValidInput_LargeDocument) {
    const int32_t largeBsonSize = 15 * 1024 * 1024;
    auto largeBsonBuffer = generateBuffer(largeBsonSize);

    MongoExtensionByteView view{reinterpret_cast<const uint8_t*>(largeBsonBuffer.data()),
                                largeBsonSize};

    auto bsonObj = bsonObjFromByteView(view);
    ASSERT_TRUE(bsonObj.isValid());
}

}  // namespace
}  // namespace mongo::extension
