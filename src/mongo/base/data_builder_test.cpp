// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_builder.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_type_terminated.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>

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

    ASSERT_EQUALS(static_cast<uint16_t>(1), cdrc.readAndAdvance<uint16_t>());
    ASSERT_EQUALS(static_cast<uint32_t>(2), cdrc.readAndAdvance<LittleEndian<uint32_t>>());
    ASSERT_EQUALS(static_cast<uint64_t>(3), cdrc.readAndAdvance<BigEndian<uint64_t>>());
    ASSERT_NOT_OK(cdrc.readAndAdvanceNoThrow<char>());
}

TEST(DataBuilder, ResizeDown) {
    DataBuilder db(1);

    ASSERT_EQUALS(true, db.writeAndAdvance<uint16_t>(1).isOK());
    ASSERT_EQUALS(true, db.writeAndAdvance<uint64_t>(2).isOK());

    db.resize(2u);
    ASSERT_EQUALS(2u, db.capacity());
    ASSERT_EQUALS(2u, db.size());

    ConstDataRangeCursor cdrc = db.getCursor();

    ASSERT_EQUALS(static_cast<uint16_t>(1), cdrc.readAndAdvance<uint16_t>());
    ASSERT_NOT_OK(cdrc.readAndAdvanceNoThrow<char>());
}

TEST(DataBuilder, ResizeUp) {
    DataBuilder db(1);

    ASSERT_EQUALS(true, db.writeAndAdvance<uint16_t>(1).isOK());
    ASSERT_EQUALS(true, db.writeAndAdvance<uint64_t>(2).isOK());

    db.resize(64u);
    ASSERT_EQUALS(64u, db.capacity());
    ASSERT_EQUALS(10u, db.size());

    ConstDataRangeCursor cdrc = db.getCursor();

    ASSERT_EQUALS(static_cast<uint16_t>(1), cdrc.readAndAdvance<uint16_t>());
    ASSERT_EQUALS(static_cast<uint64_t>(2), cdrc.readAndAdvance<uint64_t>());
    ASSERT_NOT_OK(cdrc.readAndAdvanceNoThrow<char>());
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
    ASSERT_NOT_OK(cdrc.readAndAdvanceNoThrow<char>());
}

TEST(DataBuilder, Move) {
    DataBuilder db(42);

    ASSERT_EQUALS(true, db.writeAndAdvance<uint16_t>(1).isOK());

    auto db2 = DataBuilder(std::move(db));

    ConstDataRangeCursor cdrc = db2.getCursor();

    ASSERT_EQUALS(static_cast<uint16_t>(1), cdrc.readAndAdvance<uint16_t>());
    ASSERT_EQUALS(42u, db2.capacity());
    ASSERT_EQUALS(2u, db2.size());

    ASSERT_EQUALS(0u, db.capacity());  // NOLINT(bugprone-use-after-move)
    ASSERT_EQUALS(0u, db.size());      // NOLINT(bugprone-use-after-move)
    ASSERT(!db.getCursor().data());    // NOLINT(bugprone-use-after-move)
}

TEST(DataBuilder, TerminatedStringDatas) {
    DataBuilder db{10};
    std::string_view sample{"abcdefgh"};

    auto status2 = db.writeAndAdvance<Terminated<'\0', std::string_view>>(sample);
    ASSERT_EQUALS(true, status2.isOK());

    auto status3 = db.writeAndAdvance<Terminated<'\0', std::string_view>>(sample);
    ASSERT_EQUALS(true, status3.isOK());
}

}  // namespace mongo
