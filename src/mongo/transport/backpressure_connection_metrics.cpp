// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/backpressure_connection_metrics.h"

#include "mongo/base/parse_number.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/decorable.h"

#include <array>
#include <string>
#include <string_view>

namespace mongo {
namespace {

const auto backpressureVersionMetricsDecoration =
    transport::Session::declareDecoration<BackpressureVersionMetrics>();

int32_t clampBackpressureVersion(int32_t version) {
    if (version < kNoBackpressureVersion) {
        return kOtherBackpressureVersion;
    }
    if (version > kMaxExplicitBackpressureVersion) {
        return kOtherBackpressureVersion;
    }
    return version;
}

bool isUnsignedDecimalDigits(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// Hello "backpressure": absent/false -> NoBackpressure; true -> 1; string "0"/"1"..256 -> version.
// Unsigned decimal strings only (no signs/whitespace); BSON numbers and other types -> Other.
int32_t normalizeBackpressureVersion(const BSONElement& elem) {
    if (elem.eoo()) {
        return kNoBackpressureVersion;
    }
    switch (elem.type()) {
        case BSONType::boolean:
            return elem.boolean() ? 1 : kNoBackpressureVersion;
        case BSONType::string: {
            const auto s = elem.valueStringData();
            if (!isUnsignedDecimalDigits(s)) {
                return kOtherBackpressureVersion;
            }
            int parsed = 0;
            if (!NumberParser{}.base(10)(s, &parsed).isOK()) {
                return kOtherBackpressureVersion;
            }
            return clampBackpressureVersion(parsed);
        }
        default:
            return kOtherBackpressureVersion;
    }
}

}  // namespace

std::string backpressureVersionLabel(int32_t version) {
    if (version == kNoBackpressureVersion) {
        return std::string{kNoBackpressureVersionLabel};
    }
    if (version < kNoBackpressureVersion || version > kMaxExplicitBackpressureVersion) {
        return std::string{kBackpressureOtherVersionLabel};
    }
    return std::to_string(version);
}

std::size_t BackpressureConnectionMetrics::_bucketIndex(Version version) {
    const auto clamped = clampBackpressureVersion(version);
    if (clamped == kOtherBackpressureVersion) {
        return kOtherBackpressureVersionBucketIndex;
    }
    return static_cast<std::size_t>(clamped);
}

Atomic<BackpressureConnectionMetrics::Count>& BackpressureConnectionMetrics::_counterFor(
    Version version) {
    return _counts[_bucketIndex(version)];
}

Atomic<BackpressureConnectionMetrics::Count>& BackpressureConnectionMetrics::_totalCreatedFor(
    Version version) {
    return _totalCreated[_bucketIndex(version)];
}

const Atomic<BackpressureConnectionMetrics::Count>& BackpressureConnectionMetrics::_counterFor(
    Version version) const {
    return _counts[_bucketIndex(version)];
}

const Atomic<BackpressureConnectionMetrics::Count>& BackpressureConnectionMetrics::_totalCreatedFor(
    Version version) const {
    return _totalCreated[_bucketIndex(version)];
}

BackpressureConnectionMetrics::BackpressureConnectionMetrics(
    BackpressureConnectionMetrics&& other) noexcept {
    for (std::size_t i = 0; i < kBackpressureVersionBucketCount; ++i) {
        _counts[i].store(other._counts[i].load());
        _totalCreated[i].store(other._totalCreated[i].load());
    }
}

BackpressureConnectionMetrics& BackpressureConnectionMetrics::operator=(
    BackpressureConnectionMetrics&& other) noexcept {
    if (this != &other) {
        for (std::size_t i = 0; i < kBackpressureVersionBucketCount; ++i) {
            _counts[i].store(other._counts[i].load());
            _totalCreated[i].store(other._totalCreated[i].load());
        }
    }
    return *this;
}

void BackpressureConnectionMetrics::increment(Version version) {
    _totalCreatedFor(version).fetchAndAdd(1);
    _counterFor(version).fetchAndAdd(1);
}

void BackpressureConnectionMetrics::decrement(Version version) {
    _counterFor(version).fetchAndSubtractRelaxed(1);
}

BackpressureConnectionMetrics::Count BackpressureConnectionMetrics::count(Version version) const {
    return _counterFor(version).load();
}

BackpressureConnectionMetrics::Count BackpressureConnectionMetrics::totalCreated(
    Version version) const {
    return _totalCreatedFor(version).load();
}

BackpressureConnectionMetrics& BackpressureConnectionMetrics::operator+=(
    const BackpressureConnectionMetrics& other) {
    for (std::size_t i = 0; i < kBackpressureVersionBucketCount; ++i) {
        const auto current = other._counts[i].load();
        if (current != 0) {
            _counts[i].fetchAndAddRelaxed(current);
        }
        const auto created = other._totalCreated[i].load();
        if (created != 0) {
            _totalCreated[i].fetchAndAddRelaxed(created);
        }
    }
    return *this;
}

void BackpressureConnectionMetrics::serialize(BSONObjBuilder* builder) const {
    // Always emit both subobjects. Keep keys with totalCreated > 0, including zeros.
    auto appendSeen = [&](BSONObjBuilder& sub, bool useActive) {
        for (std::size_t i = 0; i < kBackpressureVersionBucketCount; ++i) {
            const auto created = _totalCreated[i].load();
            if (created == 0) {
                continue;
            }
            const auto version = (i == kOtherBackpressureVersionBucketIndex)
                ? kOtherBackpressureVersion
                : static_cast<Version>(i);
            const auto val = useActive ? _counts[i].load() : created;
            sub.append(backpressureVersionLabel(version), static_cast<long long>(val));
        }
    };

    {
        BSONObjBuilder activeBuilder(builder->subobjStart("activeCount"));
        appendSeen(activeBuilder, true);
    }
    {
        BSONObjBuilder totalBuilder(builder->subobjStart("totalCount"));
        appendSeen(totalBuilder, false);
    }
}

BackpressureVersionMetrics* BackpressureVersionMetrics::get(transport::Session* session) {
    auto* deco = &backpressureVersionMetricsDecoration(session);
    if (!deco->_sessionManager) {
        if (auto* tl = session->getTransportLayer()) {
            deco->_sessionManager =
                std::weak_ptr<transport::SessionManager>(tl->getSharedSessionManager());
        }
    }
    return deco;
}

void BackpressureVersionMetrics::setVersion(BackpressureConnectionMetrics::Version version) {
    const auto newVersion = clampBackpressureVersion(version);
    auto expected = kUnset;
    if (!_version.compareAndSwap(&expected, newVersion)) {
        return;
    }
    if (auto sm = _sessionManager ? _sessionManager->lock() : nullptr) {
        sm->backpressureConnectionMetrics.increment(newVersion);
    }
}

void BackpressureVersionMetrics::setVersionFromHelloField(const BSONElement& backpressureField) {
    setVersion(normalizeBackpressureVersion(backpressureField));
}

BackpressureVersionMetrics::~BackpressureVersionMetrics() {
    const auto prev = _version.swap(kUnset);
    if (prev == kUnset || !_sessionManager) {
        return;
    }
    if (auto sm = _sessionManager->lock()) {
        sm->backpressureConnectionMetrics.decrement(prev);
    }
}

}  // namespace mongo
