// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/extension_version.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>

namespace mongo::extension {
namespace {

// ---------------------------------------------------------------------------
// Compile-time guarantees. load_extension.cpp stores CapabilityVersion in
// static constexpr objects, so these static_asserts lock in the constexpr
// contract the call sites depend on.
// ---------------------------------------------------------------------------

// Packing places the major version in the high 16 bits and the minor in the low 16 bits.
static_assert(static_cast<uint32_t>(CapabilityVersion(0, 0)) == 0x00000000u);
static_assert(static_cast<uint32_t>(CapabilityVersion(0, 0xFFFF)) == 0x0000FFFFu);
static_assert(static_cast<uint32_t>(CapabilityVersion(1, 0)) == 0x00010000u);
static_assert(static_cast<uint32_t>(CapabilityVersion(0xFFFF, 0xFFFF)) == 0xFFFFFFFFu);

// Getters unpack the packed representation.
static_assert(CapabilityVersion(1, 2).majorVersion() == 1u);
static_assert(CapabilityVersion(1, 2).minorVersion() == 2u);

// Packed comparison is lexicographic on (major, minor).
static_assert(CapabilityVersion(2, 0) >= CapabilityVersion(1, 0xFFFF));
static_assert(!(CapabilityVersion(1, 0xFFFF) >= CapabilityVersion(2, 0)));
static_assert(CapabilityVersion(1, 1) >= CapabilityVersion(1, 0));
static_assert(CapabilityVersion(1, 1) <= CapabilityVersion(1, 1));

// fromPackedValue round-trips through the packed representation.
static_assert(CapabilityVersion::fromPackedValue(0x00010002u).majorVersion() == 1u);
static_assert(CapabilityVersion::fromPackedValue(0x00010002u).minorVersion() == 2u);

// Static factories are usable in constant expressions.
static_assert(CapabilityVersion::zeroCapability() == CapabilityVersion(0, 0));
static_assert(CapabilityVersion::legacyCapability() == CapabilityVersion(1, 0));
static_assert(CapabilityVersion::latestCapability() <= CapabilityVersion(0xFFFF, 0xFFFF));

TEST(CapabilityVersionTest, StaticCapabilities) {
    ASSERT_EQ(CapabilityVersion::zeroCapability().majorVersion(), 0u);
    ASSERT_EQ(CapabilityVersion::zeroCapability().minorVersion(), 0u);

    ASSERT_EQ(CapabilityVersion::legacyCapability().majorVersion(), 1u);
    ASSERT_EQ(CapabilityVersion::legacyCapability().minorVersion(), 0u);

    // latestCapability() must track the version macros published in the public C API header.
    ASSERT_EQ(CapabilityVersion::latestCapability().majorVersion(),
              uint32_t{MONGODB_EXTENSION_API_MAJOR_VERSION});
    ASSERT_EQ(CapabilityVersion::latestCapability().minorVersion(),
              uint32_t{MONGODB_EXTENSION_API_MINOR_VERSION});
}

TEST(CapabilityVersionTest, PackingBoundaries) {
    // The maximum minor version representable in the packed 16-bit field is accepted.
    const CapabilityVersion maxMinor(0, CapabilityVersion::kCapabilityMask);
    ASSERT_EQ(maxMinor.minorVersion(), 0xFFFFu);
    ASSERT_EQ(static_cast<uint32_t>(maxMinor), 0x0000FFFFu);

    // The largest representable packed value.
    const CapabilityVersion maxBoth(0xFFFF, 0xFFFF);
    ASSERT_EQ(static_cast<uint32_t>(maxBoth), 0xFFFFFFFFu);

    // fromPackedValue inverts construction across the range.
    for (const uint32_t packed : {0x00000000u, 0x0000FFFFu, 0x00010000u, 0xFFFFFFFFu}) {
        const CapabilityVersion unpacked = CapabilityVersion::fromPackedValue(packed);
        ASSERT_EQ(static_cast<uint32_t>(unpacked), packed);
    }
}

TEST(CapabilityVersionTest, PackedOrderingIsLexicographic) {
    // A major bump dominates any minor of the previous major: (2,0) > (1,65535).
    const CapabilityVersion latestOfMajor1(1, 0xFFFF);
    const CapabilityVersion firstOfMajor2(2, 0);
    ASSERT_LT(latestOfMajor1, firstOfMajor2);
    ASSERT_GT(firstOfMajor2, latestOfMajor1);
    ASSERT_TRUE(latestOfMajor1 <= firstOfMajor2);
    ASSERT_FALSE(firstOfMajor2 <= latestOfMajor1);
    ASSERT_TRUE(firstOfMajor2 >= latestOfMajor1);
    ASSERT_FALSE(latestOfMajor1 >= firstOfMajor2);

    // Within a major, ordering follows the minor version.
    ASSERT_LT(CapabilityVersion(1, 1), CapabilityVersion(1, 2));
    ASSERT_GT(CapabilityVersion(1, 2), CapabilityVersion(1, 1));
    ASSERT_TRUE(CapabilityVersion(1, 1) <= CapabilityVersion(1, 2));
    ASSERT_FALSE(CapabilityVersion(1, 2) <= CapabilityVersion(1, 1));

    // Reflexivity.
    const CapabilityVersion cap(1, 1);
    ASSERT_TRUE(cap <= cap);
    ASSERT_TRUE(cap >= cap);
    ASSERT_FALSE(cap < cap);
    ASSERT_FALSE(cap > cap);
}

TEST(CapabilityVersionTest, Equality) {
    ASSERT_TRUE(CapabilityVersion(1, 1) == CapabilityVersion(1, 1));
    ASSERT_FALSE(CapabilityVersion(1, 1) == CapabilityVersion(1, 2));
    ASSERT_FALSE(CapabilityVersion(1, 1) == CapabilityVersion(2, 1));

    ASSERT_TRUE(CapabilityVersion(1, 1) != CapabilityVersion(1, 2));
    ASSERT_FALSE(CapabilityVersion(1, 1) != CapabilityVersion(1, 1));

    // Equality holds through a pack/unpack round-trip.
    const CapabilityVersion cap(1, 2);
    ASSERT_TRUE(CapabilityVersion::fromPackedValue(static_cast<uint32_t>(cap)) == cap);
}

TEST(CapabilityVersionTest, ToString) {
    ASSERT_EQ(CapabilityVersion::zeroCapability().toString(), "0.0");
    ASSERT_EQ(CapabilityVersion::legacyCapability().toString(), "1.0");
    ASSERT_EQ(CapabilityVersion(2, 15).toString(), "2.15");
    ASSERT_EQ(CapabilityVersion(0xFFFF, 0xFFFF).toString(), "65535.65535");
}

DEATH_TEST(CapabilityVersionDeathTest, MinorAtPackedWidthIsRejected, "exceeds packed width") {
    // 2^16 does not fit in the packed minor field: it would bleed into the major bits.
    CapabilityVersion(1, CapabilityVersion::kCapabilityMask + 1u);
}

DEATH_TEST(CapabilityVersionDeathTest, MaxUint32MinorIsRejected, "exceeds packed width") {
    CapabilityVersion(0, UINT32_MAX);
}

}  // namespace
}  // namespace mongo::extension
