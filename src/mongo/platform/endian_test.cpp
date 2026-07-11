// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/endian.h"

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"

#include <cstring>

namespace mongo::endian {
namespace {

const bool kNativeLittle = (endian::Order::kNative == endian::Order::kLittle);

TEST(EndianTest, NativeToBig_uint16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    uint16_t le;
    uint16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(be, nativeToBig(le));
    } else {
        ASSERT_EQUALS(be, nativeToBig(be));
    }
}

TEST(EndianTest, NativeToBig_uint32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    uint32_t le;
    uint32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(be, nativeToBig(le));
    } else {
        ASSERT_EQUALS(be, nativeToBig(be));
    }
}

TEST(EndianTest, NativeToBig_uint64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    uint64_t le;
    uint64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(be, nativeToBig(le));
    } else {
        ASSERT_EQUALS(be, nativeToBig(be));
    }
}

TEST(EndianTest, NativeToBig_int16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    int16_t le;
    int16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(be, nativeToBig(le));
    } else {
        ASSERT_EQUALS(be, nativeToBig(be));
    }
}

TEST(EndianTest, NativeToBig_int32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    int32_t le;
    int32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(be, nativeToBig(le));
    } else {
        ASSERT_EQUALS(be, nativeToBig(be));
    }
}

TEST(EndianTest, NativeToBig_int64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    int64_t le;
    int64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(be, nativeToBig(le));
    } else {
        ASSERT_EQUALS(be, nativeToBig(be));
    }
}

TEST(EndianTest, NativeToBig_float) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    float le;
    float be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(be, nativeToBig(le));
    } else {
        ASSERT_EQUALS(be, nativeToBig(be));
    }
}

TEST(EndianTest, NativeToBig_double) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    double le;
    double be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(be, nativeToBig(le));
    } else {
        ASSERT_EQUALS(be, nativeToBig(be));
    }
}

TEST(EndianTest, NativeToLittle_uint16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    uint16_t le;
    uint16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, nativeToLittle(le));
    } else {
        ASSERT_EQUALS(le, nativeToLittle(be));
    }
}

TEST(EndianTest, NativeToLittle_uint32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    uint32_t le;
    uint32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, nativeToLittle(le));
    } else {
        ASSERT_EQUALS(le, nativeToLittle(be));
    }
}

TEST(EndianTest, NativeToLittle_uint64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    uint64_t le;
    uint64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, nativeToLittle(le));
    } else {
        ASSERT_EQUALS(le, nativeToLittle(be));
    }
}

TEST(EndianTest, NativeToLittle_int16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    int16_t le;
    int16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, nativeToLittle(le));
    } else {
        ASSERT_EQUALS(le, nativeToLittle(be));
    }
}

TEST(EndianTest, NativeToLittle_int32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    int32_t le;
    int32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, nativeToLittle(le));
    } else {
        ASSERT_EQUALS(le, nativeToLittle(be));
    }
}

TEST(EndianTest, NativeToLittle_int64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    int64_t le;
    int64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, nativeToLittle(le));
    } else {
        ASSERT_EQUALS(le, nativeToLittle(be));
    }
}

TEST(EndianTest, NativeToLittle_float) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    float le;
    float be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, nativeToLittle(le));
    } else {
        ASSERT_EQUALS(le, nativeToLittle(be));
    }
}

TEST(EndianTest, NativeToLittle_double) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    double le;
    double be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, nativeToLittle(le));
    } else {
        ASSERT_EQUALS(le, nativeToLittle(be));
    }
}

TEST(EndianTest, LittleToNative_uint16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    uint16_t le;
    uint16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, littleToNative(le));
    } else {
        ASSERT_EQUALS(be, littleToNative(le));
    }
}

TEST(EndianTest, LittleToNative_uint32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    uint32_t le;
    uint32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, littleToNative(le));
    } else {
        ASSERT_EQUALS(be, littleToNative(le));
    }
}

TEST(EndianTest, LittleToNative_uint64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    uint64_t le;
    uint64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, littleToNative(le));
    } else {
        ASSERT_EQUALS(be, littleToNative(le));
    }
}

TEST(EndianTest, LittleToNative_int16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    int16_t le;
    int16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, littleToNative(le));
    } else {
        ASSERT_EQUALS(be, littleToNative(le));
    }
}

TEST(EndianTest, LittleToNative_int32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    int32_t le;
    int32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, littleToNative(le));
    } else {
        ASSERT_EQUALS(be, littleToNative(le));
    }
}

TEST(EndianTest, LittleToNative_int64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    int64_t le;
    int64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, littleToNative(le));
    } else {
        ASSERT_EQUALS(be, littleToNative(le));
    }
}

TEST(EndianTest, LittleToNative_float) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    float le;
    float be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, littleToNative(le));
    } else {
        ASSERT_EQUALS(be, littleToNative(le));
    }
}

TEST(EndianTest, LittleToNative_double) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    double le;
    double be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, littleToNative(le));
    } else {
        ASSERT_EQUALS(be, littleToNative(le));
    }
}

TEST(EndianTest, BigToNative_uint16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    uint16_t le;
    uint16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, bigToNative(be));
    } else {
        ASSERT_EQUALS(be, bigToNative(be));
    }
}

TEST(EndianTest, BigToNative_uint32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    uint32_t le;
    uint32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, bigToNative(be));
    } else {
        ASSERT_EQUALS(be, bigToNative(be));
    }
}

TEST(EndianTest, BigToNative_uint64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    uint64_t le;
    uint64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, bigToNative(be));
    } else {
        ASSERT_EQUALS(be, bigToNative(be));
    }
}

TEST(EndianTest, BigToNative_int16_t) {
    uint8_t le_buf[] = {0x01, 0x02};
    uint8_t be_buf[] = {0x02, 0x01};
    int16_t le;
    int16_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, bigToNative(be));
    } else {
        ASSERT_EQUALS(be, bigToNative(be));
    }
}

TEST(EndianTest, BigToNative_int32_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    int32_t le;
    int32_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, bigToNative(be));
    } else {
        ASSERT_EQUALS(be, bigToNative(be));
    }
}

TEST(EndianTest, BigToNative_int64_t) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    int64_t le;
    int64_t be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, bigToNative(be));
    } else {
        ASSERT_EQUALS(be, bigToNative(be));
    }
}

TEST(EndianTest, BigToNative_float) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t be_buf[] = {0x04, 0x03, 0x02, 0x01};
    float le;
    float be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, bigToNative(be));
    } else {
        ASSERT_EQUALS(be, bigToNative(be));
    }
}

TEST(EndianTest, BigToNative_double) {
    uint8_t le_buf[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t be_buf[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    double le;
    double be;
    std::memcpy(&le, le_buf, sizeof(le));
    std::memcpy(&be, be_buf, sizeof(be));

    if (kNativeLittle) {
        ASSERT_EQUALS(le, bigToNative(be));
    } else {
        ASSERT_EQUALS(be, bigToNative(be));
    }
}

}  // namespace
}  // namespace mongo::endian
