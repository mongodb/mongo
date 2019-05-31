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

#include "mongo/util/shared_buffer.h"

#include "mongo/base/string_data.h"

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

}  // namespace
}  // namespace mongo
