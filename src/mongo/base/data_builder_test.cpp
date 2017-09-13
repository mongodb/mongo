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

#include "mongo/base/data_builder.h"

#include "mongo/platform/endian.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {
/**
 * Helper type for writeAndAdvance in tests
 */
template <std::size_t bytes>
struct NByteStruct {
    NByteStruct() = default;
    char buf[bytes] = {};
};

}  // namespace

TEST(DataBuilder, Basic) {
    DataBuilder db(1);

    ASSERT_EQUALS(true, db.writeAndAdvance<uint16_t>(1).isOK());
    ASSERT_EQUALS(true, db.writeAndAdvance<LittleEndian<uint32_t>>(2).isOK());
    ASSERT_EQUALS(true, db.writeAndAdvance<BigEndian<uint64_t>>(3).isOK());

    ASSERT_EQUALS(18u, db.capacity());
    ASSERT_EQUALS(14u, db.size());

    db.resize(14u);
    ASSERT_EQUALS(14u, db.capacity());
    ASSERT_EQUALS(14u, db.size());

    db.reserve(2u);
    ASSERT_EQUALS(21u, db.capacity());
    ASSERT_EQUALS(14u, db.size());

    ConstDataRangeCursor cdrc = db.getCursor();

    ASSERT_EQUALS(static_cast<uint16_t>(1), cdrc.readAndAdvance<uint16_t>().getValue());
    ASSERT_EQUALS(static_cast<uint32_t>(2),
                  cdrc.readAndAdvance<LittleEndian<uint32_t>>().getValue());
    ASSERT_EQUALS(static_cast<uint64_t>(3), cdrc.readAndAdvance<BigEndian<uint64_t>>().getValue());
    ASSERT_EQUALS(false, cdrc.readAndAdvance<char>().isOK());
}

TEST(DataBuilder, ResizeDown) {
    DataBuilder db(1);

    ASSERT_EQUALS(true, db.writeAndAdvance<uint16_t>(1).isOK());
    ASSERT_EQUALS(true, db.writeAndAdvance<uint64_t>(2).isOK());

    db.resize(2u);
    ASSERT_EQUALS(2u, db.capacity());
    ASSERT_EQUALS(2u, db.size());

    ConstDataRangeCursor cdrc = db.getCursor();

    ASSERT_EQUALS(static_cast<uint16_t>(1), cdrc.readAndAdvance<uint16_t>().getValue());
    ASSERT_EQUALS(false, cdrc.readAndAdvance<char>().isOK());
}

TEST(DataBuilder, ResizeUp) {
    DataBuilder db(1);

    ASSERT_EQUALS(true, db.writeAndAdvance<uint16_t>(1).isOK());
    ASSERT_EQUALS(true, db.writeAndAdvance<uint64_t>(2).isOK());

    db.resize(64u);
    ASSERT_EQUALS(64u, db.capacity());
    ASSERT_EQUALS(10u, db.size());

    ConstDataRangeCursor cdrc = db.getCursor();

    ASSERT_EQUALS(static_cast<uint16_t>(1), cdrc.readAndAdvance<uint16_t>().getValue());
    ASSERT_EQUALS(static_cast<uint64_t>(2), cdrc.readAndAdvance<uint64_t>().getValue());
    ASSERT_EQUALS(false, cdrc.readAndAdvance<char>().isOK());
}

TEST(DataBuilder, Reserve) {
    DataBuilder db;

    ASSERT_EQUALS(0u, db.capacity());
    ASSERT_EQUALS(0u, db.size());

    // first step up is to 64
    db.reserve(10);
    ASSERT_EQUALS(64u, db.capacity());
    ASSERT_EQUALS(0u, db.size());

    // reserving less than we have doesn't change anything
    db.reserve(1);
    ASSERT_EQUALS(64u, db.capacity());
    ASSERT_EQUALS(0u, db.size());

    // next actual step up goes up by x1.5
    db.reserve(65);
    ASSERT_EQUALS(96u, db.capacity());
    ASSERT_EQUALS(0u, db.size());

    ASSERT_EQUALS(true, db.writeAndAdvance(NByteStruct<90>()).isOK());
    ASSERT_EQUALS(96u, db.capacity());
    ASSERT_EQUALS(90u, db.size());

    // partially satisfiable reserve works
    db.reserve(7);
    ASSERT_EQUALS(144u, db.capacity());
    ASSERT_EQUALS(90u, db.size());
}

TEST(DataBuilder, Clear) {
    DataBuilder db(1);

    ASSERT_EQUALS(true, db.writeAndAdvance<uint16_t>(1).isOK());

    db.clear();
    ASSERT_EQUALS(2u, db.capacity());
    ASSERT_EQUALS(0u, db.size());

    ConstDataRangeCursor cdrc = db.getCursor();
    ASSERT_EQUALS(false, cdrc.readAndAdvance<char>().isOK());
}

TEST(DataBuilder, Move) {
    DataBuilder db(1);

    ASSERT_EQUALS(true, db.writeAndAdvance<uint16_t>(1).isOK());

    auto db2 = DataBuilder(std::move(db));

    ConstDataRangeCursor cdrc = db2.getCursor();

    ASSERT_EQUALS(static_cast<uint16_t>(1), cdrc.readAndAdvance<uint16_t>().getValue());
    ASSERT_EQUALS(2u, db2.capacity());
    ASSERT_EQUALS(2u, db2.size());

    ASSERT_EQUALS(0u, db.capacity());
    ASSERT_EQUALS(0u, db.size());
    ASSERT(!db.getCursor().data());
}

}  // namespace mongo
