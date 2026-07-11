// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/crypto/mongocryptbuffer.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

// CDR's operator== enforces identicality (same memory addresses),
// not just equality (same contents).
// Do a lightweight conversion to StrindData for ASSERT_EQ equality comparisons.
std::string_view toString(ConstDataRange cdr) {
    return {cdr.data(), cdr.length()};
}

TEST(MongoCryptBuffer, EmptyBuffer) {
    MongoCryptBuffer buffer;
    ASSERT_TRUE(buffer.empty());
    ASSERT(buffer.data() == nullptr);
    ASSERT_EQ(buffer.size(), 0);
}

constexpr auto kHelloWorld = "Hello World"sv;
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
