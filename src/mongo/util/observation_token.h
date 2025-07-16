/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/stdx/mutex.h"

#include <boost/optional/optional.hpp>

namespace mongo {

namespace observable_mutex_details {

/**
 * AcquisitionStats are the building blocks to keeping track of different sorts of acquisition
 * statistics, mainly exclusive and shared (as seen below).
 */
template <typename CounterType>
struct AcquisitionStats {
    CounterType total{0};        // Tracks the total number of acquisitions.
    CounterType contentions{0};  // Tracks the number of acquisitions that had to wait.
    CounterType waitCycles{0};   // Tracks the total wait time for contended acquisitions.
};

struct LockStats {
    AcquisitionStats<uint64_t> exclusiveAcquisitions{0, 0, 0};
    AcquisitionStats<uint64_t> sharedAcquisitions{0, 0, 0};
};

/**
 * Tokens are shared between the wrapper ObservableMutex and the ObservableMutexRegistry. A token is
 * used to enable tracking instances of ObservableMutex without changing their lifetime semantics.
 * The validity of the token tracks the liveness of its corresponding mutex object. The token also
 * enables collectors to safely acquire the latest contention statistics for each ObservableMutex
 * through the registry.
 */
class ObservationToken {
public:
    using AppendStatsCallback = std::function<void(LockStats&)>;

    explicit ObservationToken(AppendStatsCallback callback) : _callback(std::move(callback)) {}

    void invalidate() {
        stdx::lock_guard lk(_mutex);
        _isValid = false;
    }

    bool isValid() const {
        stdx::lock_guard lk(_mutex);
        return _isValid;
    }

    explicit operator bool() const {
        return isValid();
    }

    boost::optional<LockStats> getStats() const {
        stdx::lock_guard lk(_mutex);
        if (!_isValid) {
            return boost::none;
        }
        LockStats stats;
        _callback(stats);
        return stats;
    }

private:
    AppendStatsCallback _callback;
    mutable stdx::mutex _mutex;
    bool _isValid = true;
};

}  // namespace observable_mutex_details
}  // namespace mongo
