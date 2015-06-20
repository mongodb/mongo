/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/base/data_cursor.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/platform/endian.h"
#include "mongo/unittest/unittest.h"

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
