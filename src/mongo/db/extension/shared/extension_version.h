// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/db/extension/public/api.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <compare>
#include <string>

namespace mongo::extension {

class CapabilityVersion {
public:
    static constexpr uint32_t kCapabilityShift = 16;
    static constexpr uint32_t kCapabilityMask = (1u << kCapabilityShift) - 1u;

    constexpr CapabilityVersion(uint32_t majorVersion, uint32_t minorVersion)
        : _capability((majorVersion << kCapabilityShift) | minorVersion) {
        tassert(13693503,
                fmt::format(
                    "Negotiated major {} exceeds packed width 0-{}", majorVersion, kCapabilityMask),
                isValidVersionComponent(majorVersion));
        tassert(13539200,
                fmt::format(
                    "Negotiated minor {} exceeds packed width 0-{}", minorVersion, kCapabilityMask),
                isValidVersionComponent(minorVersion));
    }

    static constexpr CapabilityVersion legacyCapability() {
        return CapabilityVersion(1, 0);
    }

    static constexpr CapabilityVersion zeroCapability() {
        return CapabilityVersion(0, 0);
    }

    static constexpr CapabilityVersion latestCapability() {
        return CapabilityVersion(MONGODB_EXTENSION_API_MAJOR_VERSION,
                                 MONGODB_EXTENSION_API_MINOR_VERSION);
    }

    static constexpr CapabilityVersion fromPackedValue(uint32_t packedValue) {
        return CapabilityVersion(packedValue);
    }

    // Both components pack into 16-bit halves of _capability (minor in the low bits, major
    // shifted into the high bits), so the same bound validates either one. Without this check
    // a large major would silently wrap on the shift: 0x10000 packs as major 0, and 0x10001
    // would alias to major 1, smuggling an unsupported major into the supported range.
    static constexpr bool isValidVersionComponent(uint32_t component) {
        return component <= kCapabilityMask;
    }

    static constexpr bool isValidVersion(uint32_t majorVersion, uint32_t minorVersion) {
        return isValidVersionComponent(majorVersion) && isValidVersionComponent(minorVersion);
    }

    constexpr uint32_t majorVersion() const {
        return _capability >> kCapabilityShift;
    }

    constexpr uint32_t minorVersion() const {
        return _capability & kCapabilityMask;
    }

    constexpr bool operator==(const CapabilityVersion& o) const = default;

    constexpr auto operator<=>(const CapabilityVersion& o) const = default;

    constexpr explicit operator uint32_t() const {
        return _capability;
    }

    std::string toString() const {
        return fmt::format("{}.{}", majorVersion(), minorVersion());
    }

    ::MongoExtensionAPIVersion toExtensionAPIVersion() const {
        return {majorVersion(), minorVersion()};
    }

private:
    constexpr CapabilityVersion(uint32_t packedValue) : _capability(packedValue) {}
    uint32_t _capability;
};

class CapabilityBounds {
public:
    constexpr CapabilityBounds(CapabilityVersion minVersion, CapabilityVersion maxVersion)
        : _min(minVersion), _max(maxVersion) {}

    constexpr bool isSupported(const CapabilityVersion& version) const {
        return version >= _min && version <= _max;
    }

private:
    CapabilityVersion _min;
    CapabilityVersion _max;
};
}  // namespace mongo::extension
