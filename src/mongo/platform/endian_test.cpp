/*    Copyright 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/endian.h"

#include <cstring>

#include "mongo/config.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using namespace endian;

TEST(EndianTest, TestSlow16) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    uint16_t le;
    uint16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    ASSERT_EQUALS(be, endian::bswap_slow16(le));
    ASSERT_EQUALS(le, endian::bswap_slow16(be));
}

TEST(EndianTest, TestSlow32) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    uint32_t le;
    uint32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    ASSERT_EQUALS(be, endian::bswap_slow32(le));
    ASSERT_EQUALS(le, endian::bswap_slow32(be));
}

TEST(EndianTest, TestSlow64) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    uint64_t le;
    uint64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    ASSERT_EQUALS(be, endian::bswap_slow64(le));
    ASSERT_EQUALS(le, endian::bswap_slow64(be));
}

TEST(EndianTest, NativeToBig_uint16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    uint16_t le;
    uint16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(be, nativeToBig(le));
#else
    ASSERT_EQUALS(be, nativeToBig(be));
#endif
}

TEST(EndianTest, NativeToBig_uint32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    uint32_t le;
    uint32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(be, nativeToBig(le));
#else
    ASSERT_EQUALS(be, nativeToBig(be));
#endif
}

TEST(EndianTest, NativeToBig_uint64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    uint64_t le;
    uint64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(be, nativeToBig(le));
#else
    ASSERT_EQUALS(be, nativeToBig(be));
#endif
}

TEST(EndianTest, NativeToBig_int16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    int16_t le;
    int16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(be, nativeToBig(le));
#else
    ASSERT_EQUALS(be, nativeToBig(be));
#endif
}

TEST(EndianTest, NativeToBig_int32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    int32_t le;
    int32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(be, nativeToBig(le));
#else
    ASSERT_EQUALS(be, nativeToBig(be));
#endif
}

TEST(EndianTest, NativeToBig_int64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    int64_t le;
    int64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(be, nativeToBig(le));
#else
    ASSERT_EQUALS(be, nativeToBig(be));
#endif
}

TEST(EndianTest, NativeToBig_float) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    float le;
    float be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(be, nativeToBig(le));
#else
    ASSERT_EQUALS(be, nativeToBig(be));
#endif
}

TEST(EndianTest, NativeToBig_double) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    double le;
    double be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(be, nativeToBig(le));
#else
    ASSERT_EQUALS(be, nativeToBig(be));
#endif
}

TEST(EndianTest, NativeToLittle_uint16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    uint16_t le;
    uint16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, nativeToLittle(le));
#else
    ASSERT_EQUALS(le, nativeToLittle(be));
#endif
}

TEST(EndianTest, NativeToLittle_uint32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    uint32_t le;
    uint32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, nativeToLittle(le));
#else
    ASSERT_EQUALS(le, nativeToLittle(be));
#endif
}

TEST(EndianTest, NativeToLittle_uint64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    uint64_t le;
    uint64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, nativeToLittle(le));
#else
    ASSERT_EQUALS(le, nativeToLittle(be));
#endif
}

TEST(EndianTest, NativeToLittle_int16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    int16_t le;
    int16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, nativeToLittle(le));
#else
    ASSERT_EQUALS(le, nativeToLittle(be));
#endif
}

TEST(EndianTest, NativeToLittle_int32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    int32_t le;
    int32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, nativeToLittle(le));
#else
    ASSERT_EQUALS(le, nativeToLittle(be));
#endif
}

TEST(EndianTest, NativeToLittle_int64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    int64_t le;
    int64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, nativeToLittle(le));
#else
    ASSERT_EQUALS(le, nativeToLittle(be));
#endif
}

TEST(EndianTest, NativeToLittle_float) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    float le;
    float be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, nativeToLittle(le));
#else
    ASSERT_EQUALS(le, nativeToLittle(be));
#endif
}

TEST(EndianTest, NativeToLittle_double) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    double le;
    double be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, nativeToLittle(le));
#else
    ASSERT_EQUALS(le, nativeToLittle(be));
#endif
}

TEST(EndianTest, LittleToNative_uint16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    uint16_t le;
    uint16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, littleToNative(le));
#else
    ASSERT_EQUALS(be, littleToNative(le));
#endif
}

TEST(EndianTest, LittleToNative_uint32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    uint32_t le;
    uint32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, littleToNative(le));
#else
    ASSERT_EQUALS(be, littleToNative(le));
#endif
}

TEST(EndianTest, LittleToNative_uint64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    uint64_t le;
    uint64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, littleToNative(le));
#else
    ASSERT_EQUALS(be, littleToNative(le));
#endif
}

TEST(EndianTest, LittleToNative_int16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    int16_t le;
    int16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, littleToNative(le));
#else
    ASSERT_EQUALS(be, littleToNative(le));
#endif
}

TEST(EndianTest, LittleToNative_int32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    int32_t le;
    int32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, littleToNative(le));
#else
    ASSERT_EQUALS(be, littleToNative(le));
#endif
}

TEST(EndianTest, LittleToNative_int64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    int64_t le;
    int64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, littleToNative(le));
#else
    ASSERT_EQUALS(be, littleToNative(le));
#endif
}

TEST(EndianTest, LittleToNative_float) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    float le;
    float be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, littleToNative(le));
#else
    ASSERT_EQUALS(be, littleToNative(le));
#endif
}

TEST(EndianTest, LittleToNative_double) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    double le;
    double be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, littleToNative(le));
#else
    ASSERT_EQUALS(be, littleToNative(le));
#endif
}

TEST(EndianTest, BigToNative_uint16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    uint16_t le;
    uint16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, bigToNative(be));
#else
    ASSERT_EQUALS(be, bigToNative(be));
#endif
}

TEST(EndianTest, BigToNative_uint32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    uint32_t le;
    uint32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, bigToNative(be));
#else
    ASSERT_EQUALS(be, bigToNative(be));
#endif
}

TEST(EndianTest, BigToNative_uint64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    uint64_t le;
    uint64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, bigToNative(be));
#else
    ASSERT_EQUALS(be, bigToNative(be));
#endif
}

TEST(EndianTest, BigToNative_int16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    int16_t le;
    int16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, bigToNative(be));
#else
    ASSERT_EQUALS(be, bigToNative(be));
#endif
}

TEST(EndianTest, BigToNative_int32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    int32_t le;
    int32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, bigToNative(be));
#else
    ASSERT_EQUALS(be, bigToNative(be));
#endif
}

TEST(EndianTest, BigToNative_int64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    int64_t le;
    int64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, bigToNative(be));
#else
    ASSERT_EQUALS(be, bigToNative(be));
#endif
}

TEST(EndianTest, BigToNative_float) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    float le;
    float be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, bigToNative(be));
#else
    ASSERT_EQUALS(be, bigToNative(be));
#endif
}

TEST(EndianTest, BigToNative_double) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    double le;
    double be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

#if MONGO_CONFIG_BYTE_ORDER == 1234
    ASSERT_EQUALS(le, bigToNative(be));
#else
    ASSERT_EQUALS(be, bigToNative(be));
#endif
}

}  // namespace mongo
