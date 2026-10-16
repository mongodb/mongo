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
        tassert(13539200,
                fmt::format(
                    "Negotiated minor {} exceeds packed width 0-{}", minorVersion, kCapabilityMask),
                minorVersion <= kCapabilityMask);
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

private:
    constexpr CapabilityVersion(uint32_t packedValue) : _capability(packedValue) {}
    uint32_t _capability;
};
}  // namespace mongo::extension
