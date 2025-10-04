/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/extension/sdk/byte_buf_utils.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

namespace mongo::extension::sdk {
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

DEATH_TEST(ByteBufUtilsTest, BsonObjFromByteView_InvalidInput_InsufficientLength, "10596405") {
    MongoExtensionByteView view{nullptr, BSONObj::kMinBSONLength - 1};
    [[maybe_unused]] auto bsonObj = bsonObjFromByteView(view);
}

DEATH_TEST(ByteBufUtilsTest, BsonObjFromByteView_InvalidInput_MalformedLength, "10596405") {
    const uint8_t malformedBsonData[] = {
        0xFF, 0xFF, 0xFF, 0xFF,  // Invalid length (negative or oversized)
    };
    MongoExtensionByteView view{malformedBsonData, 4};
    [[maybe_unused]] auto bsonObj = bsonObjFromByteView(view);
}

DEATH_TEST(ByteBufUtilsTest, BsonObjFromByteView_InvalidInput_LengthExceedsBufferSize, "10596405") {
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

DEATH_TEST(ByteBufUtilsTest, BsonObjFromByteView_InvalidInput_Empty, "10596405") {
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
}  // namespace mongo::extension::sdk
