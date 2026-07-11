// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension {
namespace {

TEST(ByteBufTest, EmptyCtorHasEmptyView) {
    auto buf = new ByteBuf();
    ExtensionByteBufHandle handle{buf};
    auto sv = handle->getStringView();
    ASSERT_EQ(sv.size(), 0U);
}

TEST(ByteBufTest, AssignFromRawBytesCopiesAndIsStable) {
    const uint8_t bytes[] = {1, 2, 3, 4, 5};
    auto buf = new ByteBuf();
    buf->assign(bytes, sizeof(bytes));

    ExtensionByteBufHandle handle{buf};
    auto sv = handle->getStringView();
    ASSERT_EQ(sv.size(), sizeof(bytes));
    ASSERT_EQ(std::memcmp(sv.data(), bytes, sizeof(bytes)), 0);
}

TEST(ByteBufTest, AssignToZeroClearsBuffer) {
    const uint8_t bytes[] = {9, 8, 7};
    auto buf = new ByteBuf(bytes, sizeof(bytes));
    buf->assign(nullptr, 0);

    ExtensionByteBufHandle handle{buf};
    ASSERT_TRUE(handle->getStringView().empty());
}

TEST(ByteBufTest, ConstructFromBSONCopiesBytesIndependentLifetime) {
    ExtensionByteBufHandle handle{nullptr};
    {
        // Build a BSONObj with a short lifetime for its owner.
        BSONObjBuilder bob;
        bob.append("x", 42);
        auto original = bob.obj();

        // Initialize the byte buffer from the BSONObj while 'original' is alive.
        ExtensionByteBufHandle tmp{new ByteBuf(original)};
        handle = std::move(tmp);
    }

    // 'original' and 'bob' have gone and out of scope and are destroyed here. Buffer must remain
    // valid after its source is gone. Reconstruct BSON from the buffer bytes and verify.
    auto roundTrip = bsonObjFromByteView(handle->getByteView());
    ASSERT_EQ(roundTrip.getIntField("x"), 42);
}

TEST(ByteBufTest, RoundTripBSONWorks) {
    const auto doc = BSON("a" << 1 << "b" << BSON("c" << true));
    auto buf = new ByteBuf(doc);
    ExtensionByteBufHandle handle{buf};

    auto from = bsonObjFromByteView(handle->getByteView());
    ASSERT_EQ(from.toString(), doc.toString());
}

DEATH_TEST(ByteBufDeathTest, AssignNullWithPositiveLenFails, "518") {
    ByteBuf buf;
    buf.assign(nullptr, 4);
}

DEATH_TEST(ExtensionByteBufVTableDeathTest, InvalidExtensionByteBufVTableFailsGetView, "517") {
    auto vtable = ByteBuf::getVTable();
    vtable.get_view = nullptr;
    ExtensionByteBufAPI::assertVTableConstraints(vtable);
}

}  // namespace
}  // namespace mongo::extension
