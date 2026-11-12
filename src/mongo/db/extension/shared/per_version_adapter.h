// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/shared/extension_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace mongo::extension {

/**
 * The total number of instances vended per negotiable (major, minor), i.e. minor+1 per supported
 * major. Takes the maxima as a parameter so tests can exercise multi-major arithmetic before a
 * second major exists.
 */
constexpr size_t totalNegotiableVersions(const auto& supportedVersionMaxima) {
    size_t total = 0;
    for (const auto& rowMax : supportedVersionMaxima) {
        total += rowMax.minorVersion() + 1;
    }
    return total;
}

/**
 * The row-major start offset of each supported major's row in a per-version dispatch table: prefix
 * sums of the row sizes. Takes the maxima as a parameter so tests can exercise multi-major
 * arithmetic before a second major exists.
 */
constexpr auto negotiableVersionRowOffsets(const auto& supportedVersionMaxima) {
    std::array<size_t, std::tuple_size_v<std::remove_cvref_t<decltype(supportedVersionMaxima)>>>
        offsets{};
    size_t offset = 0;
    for (size_t i = 0; i < offsets.size(); ++i) {
        offsets[i] = offset;
        offset += supportedVersionMaxima[i].minorVersion() + 1;
    }
    return offsets;
}

/**
 * Vends the per-negotiated-version instance of a type that bakes its capability into its own type
 * parameter.
 *
 * Several C API vtables are plain function pointers that receive no 'self' argument, so an
 * instance cannot carry state its own vtable entries could read. Those types instead instantiate
 * once per negotiable version, making the capability a compile-time constant visible to each
 * instantiation's statics. This is the shared lookup mapping a runtime capability back to the
 * right instantiation, used by ExtensionStatusOK and by the host's HostServicesAdapter and
 * LoggerAdapter.
 *
 * 'InstanceT' must be a class template over a packed CapabilityVersion exposing a static
 * 'getInstancePtr()' returning 'CApiT*'. That accessor is expected to be private, with this
 * function befriended, so instances cannot be obtained without a negotiated capability. Note that
 * a befriending class in a nested namespace must qualify the declaration
 * ('friend CApiT* mongo::extension::getNegotiatedVersionInstance(...)'), or it will declare an
 * unrelated template in its own namespace instead.
 *
 * Instances are lazily constructed on first negotiation of their version, and are shared by every
 * caller that negotiated the same version.
 */
template <template <uint32_t> class InstanceT, typename CApiT>
CApiT* getNegotiatedVersionInstance(const CapabilityVersion& capability) {
    // One instantiation per negotiable (major, minor), in a row-major table with one row per
    // supported major and one column per negotiable minor of that major. The table holds
    // factories so each instance is constructed lazily, on first negotiation of its version.
    static const auto kTable = []<size_t... M>(std::index_sequence<M...>) {
        constexpr size_t kNumInstances = totalNegotiableVersions(kSupportedVersionMaxima);
        std::array<CApiT* (*)(), kNumInstances> table{};
        size_t offset = 0;
        const auto fillRow = [&]<size_t kRow>(std::integral_constant<size_t, kRow>) {
            constexpr uint32_t kMajor = kSupportedVersionMaxima[kRow].majorVersion();
            constexpr size_t kRowSize = kSupportedVersionMaxima[kRow].minorVersion() + 1;
            [&]<size_t... I>(std::index_sequence<I...>) {
                ((table[offset + I] =
                      &InstanceT<static_cast<uint32_t>(
                          (kMajor << CapabilityVersion::kCapabilityShift) | I)>::getInstancePtr),
                 ...);
            }(std::make_index_sequence<kRowSize>{});
            offset += kRowSize;
        };
        (fillRow(std::integral_constant<size_t, M>{}), ...);
        return table;
    }(std::make_index_sequence<kSupportedVersionMaxima.size()>{});

    const uint32_t major = capability.majorVersion();
    const uint32_t minor = capability.minorVersion();

    // kSupportedVersionMaxima holds contiguous majors in ascending order starting from 1, so a
    // major's row is directly indexable; the row's start offset comes from prefix sums
    // precomputed at compile time. No search needed.
    static_assert([] {
        for (size_t i = 0; i < kSupportedVersionMaxima.size(); ++i) {
            if (kSupportedVersionMaxima[i].majorVersion() != i + 1) {
                return false;
            }
        }
        return true;
    }());
    tassert(13539201,
            fmt::format("Negotiated major version {} is not supported", major),
            major >= 1 && major <= kSupportedVersionMaxima.size());
    const auto& rowMax = kSupportedVersionMaxima[major - 1];
    tassert(13539202,
            fmt::format("Negotiated minor version {} exceeds the max negotiable minor {} for "
                        "major {}",
                        minor,
                        rowMax.minorVersion(),
                        major),
            minor <= rowMax.minorVersion());

    static constexpr auto kRowOffsets = negotiableVersionRowOffsets(kSupportedVersionMaxima);
    return kTable[kRowOffsets[major - 1] + minor]();
}

}  // namespace mongo::extension
