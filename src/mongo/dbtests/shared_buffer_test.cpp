/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/base/string_data.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/shared_buffer_fragment.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using SharedBufferTest = unittest::Test;

TEST_F(SharedBufferTest, ReallocOrCopyNull) {
    SharedBuffer buf;
    ASSERT_EQ(buf.capacity(), 0u);
    ASSERT(!buf);
    ASSERT(!buf.isShared());
    buf.reallocOrCopy(10);
    ASSERT(buf);
    ASSERT(!buf.isShared());
    ASSERT_EQ(buf.capacity(), 10u);
}

TEST_F(SharedBufferTest, ReallocOrCopyNullShared) {
    // null SharedBuffers are never considered "shared", even when copied.
    SharedBuffer buf;
    const SharedBuffer sharer = buf;
    ASSERT_EQ(buf.capacity(), 0u);
    ASSERT(!buf);
    ASSERT(!buf.isShared());
    buf.reallocOrCopy(10);
    ASSERT(buf);
    ASSERT(!buf.isShared());
    ASSERT_EQ(buf.capacity(), 10u);
    ASSERT_EQ(sharer.capacity(), 0u);
}

SharedBuffer makeBuffer() {
    SharedBuffer buf = SharedBuffer::allocate(4);
    memcpy(buf.get(), "foo", 4);
    return buf;
}

TEST_F(SharedBufferTest, ReallocOrCopyGrow) {
    SharedBuffer buf = makeBuffer();
    ASSERT_EQ(buf.capacity(), 4u);
    ASSERT(buf);
    ASSERT(!buf.isShared());
    buf.reallocOrCopy(10);
    ASSERT(buf);
    ASSERT(!buf.isShared());
    ASSERT_EQ(buf.capacity(), 10u);
    ASSERT_EQ("foo"_sd, buf.get());
}

TEST_F(SharedBufferTest, ReallocOrCopyGrowShared) {
    SharedBuffer buf = makeBuffer();
    const SharedBuffer sharer = buf;
    ASSERT_EQ(buf.capacity(), 4u);
    ASSERT(buf);
    ASSERT(buf.isShared());
    buf.reallocOrCopy(10);
    ASSERT(buf);
    ASSERT(!buf.isShared());
    ASSERT_EQ(buf.capacity(), 10u);
    ASSERT_EQ(sharer.capacity(), 4u);
    ASSERT_EQ("foo"_sd, buf.get());
    ASSERT_EQ("foo"_sd, sharer.get());
    ASSERT_NE(buf.get(), sharer.get());
}

TEST_F(SharedBufferTest, ReallocOrCopyShrink) {
    SharedBuffer buf = makeBuffer();
    ASSERT_EQ(buf.capacity(), 4u);
    ASSERT(buf);
    ASSERT(!buf.isShared());
    // The buffer is already at least 1 byte.
    buf.reallocOrCopy(1);
    ASSERT(buf);
    // We copy it anyway.
    ASSERT(!buf.isShared());
    ASSERT_EQ(buf.capacity(), 1u);
    ASSERT_EQ('f', buf.get()[0]);
}

TEST_F(SharedBufferTest, ReallocOrCopyShrinkShared) {
    SharedBuffer buf = makeBuffer();
    const SharedBuffer sharer = buf;
    ASSERT_EQ(buf.capacity(), 4u);
    ASSERT(buf);
    ASSERT(buf.isShared());
    // The buffer is already at least 1 byte.
    buf.reallocOrCopy(1);
    ASSERT(buf);
    // We copy it anyway.
    ASSERT(!buf.isShared());
    ASSERT_EQ(buf.capacity(), 1u);
    ASSERT_EQ(sharer.capacity(), 4u);
    ASSERT_EQ('f', buf.get()[0]);
    ASSERT_EQ("foo"_sd, sharer.get());
    ASSERT_NE(buf.get(), sharer.get());
}

TEST_F(SharedBufferTest, SharedBufferFragmentBuilder) {
    constexpr size_t kBlockSize = 16;
    SharedBufferFragmentBuilder builder(kBlockSize);

    auto verifyFragment = [](const SharedBufferFragment& fragment, uint8_t expected) {
        for (size_t i = 0; i < fragment.size(); ++i)
            ASSERT(memcmp(fragment.get() + i, &expected, 1) == 0);
    };

    builder.start(kBlockSize / 2);
    ASSERT_EQ(builder.capacity(), kBlockSize);
    uint8_t one = 1;
    memset(builder.get(), one, kBlockSize / 2);
    auto fragment1 = builder.finish(kBlockSize / 2);
    ASSERT_EQ(fragment1.size(), kBlockSize / 2);
    verifyFragment(fragment1, one);

    builder.start(kBlockSize / 2);
    ASSERT_EQ(builder.capacity(), kBlockSize / 2);
    // We can use less than we ask for
    uint8_t two = 2;
    memset(builder.get(), two, kBlockSize / 4);
    auto fragment2 = builder.finish(kBlockSize / 4);
    ASSERT_EQ(fragment2.size(), kBlockSize / 4);
    // Buffers should not overlap and be next to each other
    ASSERT_EQ(fragment1.get() + fragment1.size(), fragment2.get());
    verifyFragment(fragment2, two);

    // Verify that anything written is transfered when we grow
    builder.start(builder.capacity());
    ASSERT_EQ(builder.capacity(), kBlockSize / 4);
    uint8_t three = 3;
    size_t written = kBlockSize / 4;
    // Write current capacity
    memset(builder.get(), three, written);
    builder.grow(kBlockSize);
    // Write the rest
    memset(builder.get() + written, three, builder.capacity() - written);
    auto fragment3 = builder.finish(kBlockSize);
    for (size_t i = 0; i < (kBlockSize / 4); ++i)
        ASSERT(memcmp(fragment3.get() + i, &three, 1) == 0);
    ASSERT_GTE(builder.capacity(), kBlockSize);
    verifyFragment(fragment3, three);

    builder.start(builder.capacity());
    auto ptr = builder.get();
    builder.discard();
    builder.start(builder.capacity());
    ASSERT_EQ(builder.get(), ptr);

    // No buffers should have been overwritten by others
    verifyFragment(fragment1, one);
    verifyFragment(fragment2, two);
    verifyFragment(fragment3, three);
}

}  // namespace
}  // namespace mongo
