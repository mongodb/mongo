// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_cursor.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>

namespace mongo {

TEST(DataCursor, ConstDataCursor) {
    char buf[100];

    DataView(buf).write<uint16_t>(1);
    DataView(buf).write<LittleEndian<uint32_t>>(2, sizeof(uint16_t));
    DataView(buf).write<BigEndian<uint64_t>>(3, sizeof(uint16_t) + sizeof(uint32_t));

    ConstDataCursor cdc(buf);

    ASSERT_EQUALS(static_cast<uint16_t>(1), cdc.readAndAdvance<uint16_t>());
    ASSERT_EQUALS(static_cast<uint32_t>(2), cdc.readAndAdvance<LittleEndian<uint32_t>>());
    ASSERT_EQUALS(static_cast<uint64_t>(3), cdc.readAndAdvance<BigEndian<uint64_t>>());

    // test skip()
    cdc = buf;
    cdc.skip<uint32_t>();
    ASSERT_EQUALS(buf + sizeof(uint32_t), cdc.view());

    // test x +
    cdc = buf;
    ASSERT_EQUALS(buf + sizeof(uint32_t), (cdc + sizeof(uint32_t)).view());

    // test x -
    cdc = buf + sizeof(uint32_t);
    ASSERT_EQUALS(buf, (cdc - sizeof(uint32_t)).view());

    // test x += and x -=
    cdc = buf;
    cdc += sizeof(uint32_t);
    ASSERT_EQUALS(buf + sizeof(uint32_t), cdc.view());
    cdc -= sizeof(uint16_t);
    ASSERT_EQUALS(buf + sizeof(uint16_t), cdc.view());

    // test ++x
    cdc = buf;
    ASSERT_EQUALS(buf + sizeof(uint8_t), (++cdc).view());
    ASSERT_EQUALS(buf + sizeof(uint8_t), cdc.view());

    // test x++
    cdc = buf;
    ASSERT_EQUALS(buf, (cdc++).view());
    ASSERT_EQUALS(buf + sizeof(uint8_t), cdc.view());

    // test --x
    cdc = buf + sizeof(uint8_t);
    ASSERT_EQUALS(buf, (--cdc).view());
    ASSERT_EQUALS(buf, cdc.view());

    // test x--
    cdc = buf + sizeof(uint8_t);
    ASSERT_EQUALS(buf + sizeof(uint8_t), (cdc--).view());
    ASSERT_EQUALS(buf, cdc.view());
}

TEST(DataCursor, DataCursor) {
    char buf[100];

    DataCursor dc(buf);

    dc.writeAndAdvance<uint16_t>(1);
    dc.writeAndAdvance<LittleEndian<uint32_t>>(2);
    dc.writeAndAdvance<BigEndian<uint64_t>>(3);

    ConstDataCursor cdc(buf);

    ASSERT_EQUALS(static_cast<uint16_t>(1), cdc.readAndAdvance<uint16_t>());
    ASSERT_EQUALS(static_cast<uint32_t>(2), cdc.readAndAdvance<LittleEndian<uint32_t>>());
    ASSERT_EQUALS(static_cast<uint64_t>(3), cdc.readAndAdvance<BigEndian<uint64_t>>());
}

}  // namespace mongo
