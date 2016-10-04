/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/base/data_range_cursor.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/platform/endian.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(DataRangeCursor, ConstDataRangeCursor) {
    char buf[14];

    DataView(buf).write<uint16_t>(1);
    DataView(buf).write<LittleEndian<uint32_t>>(2, sizeof(uint16_t));
    DataView(buf).write<BigEndian<uint64_t>>(3, sizeof(uint16_t) + sizeof(uint32_t));

    ConstDataRangeCursor cdrc(buf, buf + sizeof(buf));
    ConstDataRangeCursor backup(cdrc);

    ASSERT_EQUALS(static_cast<uint16_t>(1), cdrc.readAndAdvance<uint16_t>().getValue());
    ASSERT_EQUALS(static_cast<uint32_t>(2),
                  cdrc.readAndAdvance<LittleEndian<uint32_t>>().getValue());
    ASSERT_EQUALS(static_cast<uint64_t>(3), cdrc.readAndAdvance<BigEndian<uint64_t>>().getValue());
    ASSERT_EQUALS(false, cdrc.readAndAdvance<char>().isOK());

    // test skip()
    cdrc = backup;
    ASSERT_EQUALS(true, cdrc.skip<uint32_t>().isOK());
    ;
    ASSERT_EQUALS(true, cdrc.advance(10).isOK());
    ASSERT_EQUALS(false, cdrc.readAndAdvance<char>().isOK());
}

TEST(DataRangeCursor, ConstDataRangeCursorType) {
    char buf[] = "foo";

    ConstDataRangeCursor cdrc(buf, buf + sizeof(buf));

    ConstDataRangeCursor out(nullptr, nullptr);

    auto inner = cdrc.read(&out);

    ASSERT_OK(inner);
    ASSERT_EQUALS(buf, out.data());
}

TEST(DataRangeCursor, DataRangeCursor) {
    char buf[100] = {0};

    DataRangeCursor dc(buf, buf + 14);

    ASSERT_EQUALS(true, dc.writeAndAdvance<uint16_t>(1).isOK());
    ASSERT_EQUALS(true, dc.writeAndAdvance<LittleEndian<uint32_t>>(2).isOK());
    ASSERT_EQUALS(true, dc.writeAndAdvance<BigEndian<uint64_t>>(3).isOK());
    ASSERT_EQUALS(false, dc.writeAndAdvance<char>(1).isOK());

    ConstDataRangeCursor cdrc(buf, buf + sizeof(buf));

    ASSERT_EQUALS(static_cast<uint16_t>(1), cdrc.readAndAdvance<uint16_t>().getValue());
    ASSERT_EQUALS(static_cast<uint32_t>(2),
                  cdrc.readAndAdvance<LittleEndian<uint32_t>>().getValue());
    ASSERT_EQUALS(static_cast<uint64_t>(3), cdrc.readAndAdvance<BigEndian<uint64_t>>().getValue());
    ASSERT_EQUALS(static_cast<char>(0), cdrc.readAndAdvance<char>().getValue());
}

TEST(DataRangeCursor, DataRangeCursorType) {
    char buf[] = "foo";
    char buf2[] = "barZ";

    DataRangeCursor drc(buf, buf + sizeof(buf) + -1);

    DataRangeCursor out(nullptr, nullptr);

    Status status = drc.read(&out);

    ASSERT_OK(status);
    ASSERT_EQUALS(buf, out.data());

    drc = DataRangeCursor(buf2, buf2 + sizeof(buf2) + -1);
    status = drc.write(out);

    ASSERT_OK(status);
    ASSERT_EQUALS(std::string("fooZ"), buf2);
}
}  // namespace mongo
