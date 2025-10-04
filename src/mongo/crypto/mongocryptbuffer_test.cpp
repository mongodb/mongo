/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/crypto/mongocryptbuffer.h"

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

// CDR's operator== enforces identicality (same memory addresses),
// not just equality (same contents).
// Do a lightweight conversion to StrindData for ASSERT_EQ equality comparisons.
StringData toString(ConstDataRange cdr) {
    return {cdr.data(), cdr.length()};
}

TEST(MongoCryptBuffer, EmptyBuffer) {
    MongoCryptBuffer buffer;
    ASSERT_TRUE(buffer.empty());
    ASSERT(buffer.data() == nullptr);
    ASSERT_EQ(buffer.size(), 0);
}

constexpr auto kHelloWorld = "Hello World"_sd;
const ConstDataRange kHelloWorldCDR(kHelloWorld.data(), kHelloWorld.size());

TEST(MongoCryptBuffer, HelloBufferOwned) {
    auto buffer = MongoCryptBuffer::copy(kHelloWorldCDR);
    ASSERT_TRUE(buffer.isOwned());
    ASSERT_EQ(buffer.size(), kHelloWorld.size());
    ASSERT_EQ(kHelloWorld.size(), 11);
    ASSERT_EQ(toString(buffer.toCDR()), kHelloWorld);
    buffer.data()[0]++;  // Iello World?
    ASSERT_NE(toString(buffer.toCDR()), kHelloWorld);
}

TEST(MongoCryptBuffer, HelloBufferUnowned) {
    auto buffer = MongoCryptBuffer::borrow(kHelloWorldCDR);
    ASSERT_FALSE(buffer.isOwned());
    // We expect the start/end addresses to be the same,
    // since this is a borrow from the original CDR.
    ASSERT_EQ(buffer.toCDR(), kHelloWorldCDR);
}

TEST(MongoCryptBuffer, Duplicate) {
    auto buffer = MongoCryptBuffer::borrow(kHelloWorldCDR);
    ASSERT_FALSE(buffer.isOwned());

    auto writable = buffer.duplicate();
    ASSERT_TRUE(writable.isOwned());

    ASSERT_EQ(toString(writable.toCDR()), toString(buffer.toCDR()));
    writable.data()[0]++;
    ASSERT_NE(toString(writable.toCDR()), toString(buffer.toCDR()));
}

}  // namespace
}  // namespace mongo
