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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension {
namespace {

TEST(VecByteBufTest, EmptyCtorHasEmptyView) {
    auto buf = new VecByteBuf();
    ExtensionByteBufHandle handle{buf};
    auto sv = handle.getStringView();
    ASSERT_EQ(sv.size(), 0U);
}

TEST(VecByteBufTest, AssignFromRawBytesCopiesAndIsStable) {
    const uint8_t bytes[] = {1, 2, 3, 4, 5};
    auto buf = new VecByteBuf();
    buf->assign(bytes, sizeof(bytes));

    ExtensionByteBufHandle handle{buf};
    auto sv = handle.getStringView();
    ASSERT_EQ(sv.size(), sizeof(bytes));
    ASSERT_EQ(std::memcmp(sv.data(), bytes, sizeof(bytes)), 0);
}

TEST(VecByteBufTest, AssignToZeroClearsBuffer) {
    const uint8_t bytes[] = {9, 8, 7};
    auto buf = new VecByteBuf(bytes, sizeof(bytes));
    buf->assign(nullptr, 0);

    ExtensionByteBufHandle handle{buf};
    ASSERT_TRUE(handle.getStringView().empty());
}

TEST(VecByteBufTest, ConstructFromBSONCopiesBytesIndependentLifetime) {
    ExtensionByteBufHandle handle{nullptr};
    {
        // Build a BSONObj with a short lifetime for its owner.
        BSONObjBuilder bob;
        bob.append("x", 42);
        auto original = bob.obj();

        // Initialize the byte buffer from the BSONObj while 'original' is alive.
        ExtensionByteBufHandle tmp{new VecByteBuf(original)};
        handle = std::move(tmp);
    }

    // 'original' and 'bob' have gone and out of scope and are destroyed here. Buffer must remain
    // valid after its source is gone. Reconstruct BSON from the buffer bytes and verify.
    auto roundTrip = bsonObjFromByteView(handle.getByteView());
    ASSERT_EQ(roundTrip.getIntField("x"), 42);
}

TEST(VecByteBufTest, RoundTripBSONWorks) {
    const auto doc = BSON("a" << 1 << "b" << BSON("c" << true));
    auto buf = new VecByteBuf(doc);
    ExtensionByteBufHandle handle{buf};

    auto from = bsonObjFromByteView(handle.getByteView());
    ASSERT_EQ(from.toString(), doc.toString());
}

DEATH_TEST(VecByteBufDeathTest, AssignNullWithPositiveLenFails, "10806300") {
    VecByteBuf buf;
    buf.assign(nullptr, 4);
}

class ExtensionByteBufVTableTest : public unittest::Test {
public:
    // This special handle class is only used within this fixture so that we can unit test the
    // assertVTableConstraints functionality of the handle.
    class TestExtensionByteBufVTableHandle : public ExtensionByteBufHandle {
    public:
        TestExtensionByteBufVTableHandle(::MongoExtensionByteBuf* byteBufPtr)
            : ExtensionByteBufHandle(byteBufPtr) {};

        void assertVTableConstraints(const VTable_t& vtable) {
            _assertVTableConstraints(vtable);
        }
    };
};

DEATH_TEST_F(ExtensionByteBufVTableTest, InvalidExtensionByteBufVTableFailsGetView, "10806301") {
    auto buf = new VecByteBuf();
    auto handle = TestExtensionByteBufVTableHandle{buf};

    auto vtable = handle.vtable();
    vtable.get_view = nullptr;
    handle.assertVTableConstraints(vtable);
};

}  // namespace
}  // namespace mongo::extension
