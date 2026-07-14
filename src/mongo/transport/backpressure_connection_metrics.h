/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
