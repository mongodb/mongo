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

#include "mongo/transport/backpressure_connection_metrics.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/decorable.h"

#include <array>
#include <cmath>
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

int32_t normalizeBackpressureVersion(const BSONElement& elem) {
    if (elem.eoo()) {
        return kNoBackpressureVersion;
    }
    switch (elem.type()) {
        case BSONType::boolean:
            return elem.boolean() ? 1 : kNoBackpressureVersion;
        case BSONType::numberDouble:
            if (std::isnan(elem.numberDouble()) || !std::isfinite(elem.numberDouble())) {
                return kOtherBackpressureVersion;
            }
            break;
        case BSONType::numberDecimal:
            if (elem.numberDecimal().isNaN()) {
                return kOtherBackpressureVersion;
            }
            break;
        case BSONType::numberInt:
        case BSONType::numberLong:
            break;
        default:
            return kOtherBackpressureVersion;
    }

    const auto v = elem.safeNumberInt();
    return clampBackpressureVersion(v);
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
    bool hasCurrent = false;
    bool hasTotalCreated = false;
    for (std::size_t i = 0; i < kBackpressureVersionBucketCount; ++i) {
        if (_counts[i].load() != 0) {
            hasCurrent = true;
        }
        if (_totalCreated[i].load() != 0) {
            hasTotalCreated = true;
        }
    }

    if (hasCurrent) {
        BSONObjBuilder activeBuilder(builder->subobjStart("activeCount"));
        for (std::size_t i = 0; i < kBackpressureVersionBucketCount; ++i) {
            const auto current = _counts[i].load();
            if (current != 0) {
                const auto version = (i == kOtherBackpressureVersionBucketIndex)
                    ? kOtherBackpressureVersion
                    : static_cast<Version>(i);
                activeBuilder.append(backpressureVersionLabel(version),
                                     static_cast<long long>(current));
            }
        }
    }

    if (hasTotalCreated) {
        BSONObjBuilder totalBuilder(builder->subobjStart("totalCount"));
        for (std::size_t i = 0; i < kBackpressureVersionBucketCount; ++i) {
            const auto created = _totalCreated[i].load();
            if (created != 0) {
                const auto version = (i == kOtherBackpressureVersionBucketIndex)
                    ? kOtherBackpressureVersion
                    : static_cast<Version>(i);
                totalBuilder.append(backpressureVersionLabel(version),
                                    static_cast<long long>(created));
            }
        }
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
