// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/atomic.h"
#include "mongo/transport/session.h"
#include "mongo/util/modules.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {
class ServiceContext;

namespace transport {
class SessionManager;
}  // namespace transport

/**
 * Max version tracked explicitly. Higher values and invalid inputs use
 * kOtherBackpressureVersion. Sized to bound FTDC/serverStatus growth while allowing many
 * protocol revisions over a major release.
 */
[[MONGO_MOD_PUBLIC]] inline constexpr int32_t kMaxExplicitBackpressureVersion = 256;

/** Client explicitly reports no backpressure support (absent, false, or 0). */
[[MONGO_MOD_PUBLIC]] inline constexpr int32_t kNoBackpressureVersion = 0;

/**
 * Sentinel for overflow/invalid versions. Fixed well above kMaxExplicitBackpressureVersion so
 * the explicit range can grow without renumbering this value.
 */
[[MONGO_MOD_PUBLIC]] inline constexpr int32_t kOtherBackpressureVersion = 10'000;

/** Compact array index for the overflow bucket (not equal to kOtherBackpressureVersion). */
inline constexpr std::size_t kOtherBackpressureVersionBucketIndex =
    static_cast<std::size_t>(kMaxExplicitBackpressureVersion) + 1;

/** Number of per-version counter buckets (0..kMaxExplicit + other). */
inline constexpr std::size_t kBackpressureVersionBucketCount =
    kOtherBackpressureVersionBucketIndex + 1;

/** serverStatus / OTel label for kNoBackpressureVersion. */
[[MONGO_MOD_PUBLIC]] inline constexpr std::string_view kNoBackpressureVersionLabel =
    "NoBackpressure";

/** serverStatus / OTel label for kOtherBackpressureVersion. */
[[MONGO_MOD_PUBLIC]] inline constexpr std::string_view kBackpressureOtherVersionLabel = "Other";

/**
 * serverStatus / OTel field name for a clamped version:
 * kNoBackpressureVersion -> "NoBackpressure"; kOtherBackpressureVersion (and > max) -> "Other";
 * 1..kMaxExplicitBackpressureVersion -> decimal string.
 */
[[MONGO_MOD_PUBLIC]] std::string backpressureVersionLabel(int32_t version);

/**
 * Per-version open and totalCreated ingress connection counts.
 * Owned by SessionManager; recorded after initial hello only.
 */
class [[MONGO_MOD_PUBLIC]] BackpressureConnectionMetrics {
public:
    using Version = int32_t;
    using Count = int64_t;

    BackpressureConnectionMetrics() = default;

    BackpressureConnectionMetrics(const BackpressureConnectionMetrics&) = delete;
    BackpressureConnectionMetrics& operator=(const BackpressureConnectionMetrics&) = delete;
    BackpressureConnectionMetrics(BackpressureConnectionMetrics&& other) noexcept;
    BackpressureConnectionMetrics& operator=(BackpressureConnectionMetrics&& other) noexcept;

    /** Bumps current and totalCreated for the clamped version. */
    void increment(Version version);

    /** Decrements current for the clamped version. */
    void decrement(Version version);

    /** Current open count for the clamped version. */
    Count count(Version version) const;

    /** Lifetime created count for the clamped version. */
    Count totalCreated(Version version) const;

    /** Merges another's counters into this instance. */
    BackpressureConnectionMetrics& operator+=(const BackpressureConnectionMetrics& other);

    /**
     * Appends per-version activeCount and totalCount objects.
     */
    void serialize(BSONObjBuilder* builder) const;

    /** Sums metrics across SessionManagers for serverStatus and OTel. */
    static BackpressureConnectionMetrics collect(ServiceContext* svcCtx);

private:
    static std::size_t _bucketIndex(Version version);
    Atomic<Count>& _counterFor(Version version);
    Atomic<Count>& _totalCreatedFor(Version version);
    const Atomic<Count>& _counterFor(Version version) const;
    const Atomic<Count>& _totalCreatedFor(Version version) const;

    std::array<Atomic<Count>, kBackpressureVersionBucketCount> _counts{};
    std::array<Atomic<Count>, kBackpressureVersionBucketCount> _totalCreated{};
};

/**
 * Session decoration that records the client's backpressure protocol version from the
 * initial hello. Increments SessionManager metrics on setVersion and decrements on destruction.
 */
class [[MONGO_MOD_PUBLIC]] BackpressureVersionMetrics {
public:
    BackpressureVersionMetrics() = default;

    BackpressureVersionMetrics(const BackpressureVersionMetrics&) = delete;
    BackpressureVersionMetrics& operator=(const BackpressureVersionMetrics&) = delete;
    BackpressureVersionMetrics(BackpressureVersionMetrics&&) = delete;
    BackpressureVersionMetrics& operator=(BackpressureVersionMetrics&&) = delete;

    ~BackpressureVersionMetrics();

    static BackpressureVersionMetrics* get(transport::Session* session);

    /** Records version once and increments metrics; later calls ignored. */
    void setVersion(BackpressureConnectionMetrics::Version version);

    /** Parses hello "backpressure" and records the normalized version once. */
    void setVersionFromHelloField(const BSONElement& backpressureField);

    BackpressureConnectionMetrics::Version version() const {
        return _version.load();
    }

    static constexpr BackpressureConnectionMetrics::Version kUnset = -1;

private:
    Atomic<BackpressureConnectionMetrics::Version> _version{kUnset};
    // Stashed for the destructor; Session is no longer valid then (InExhaustHello).
    boost::optional<std::weak_ptr<transport::SessionManager>> _sessionManager;
};

}  // namespace mongo
