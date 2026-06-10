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

#include "mongo/db/replicated_fast_count/size_count_checkpoint_buffer.h"

#include "mongo/util/assert_util.h"

namespace mongo::replicated_fast_count {

boost::optional<OplogScanResult> SizeCountCheckpointBuffer::checkoutForFlush() {
    std::lock_guard lk(_mutex);

    if (_inFlight) {
        return _inFlight;
    }

    if (!_pending.lastTimestamp) {
        return boost::none;
    }
    _inFlight = std::exchange(_pending, {});
    return _inFlight;
}

void SizeCountCheckpointBuffer::acknowledgeFlushSuccess() {
    std::lock_guard lk(_mutex);
    _inFlight.reset();
}

bool SizeCountCheckpointBuffer::hasInFlightWork() const {
    std::lock_guard lk(_mutex);
    return _inFlight.has_value();
}

bool SizeCountCheckpointBuffer::hasPendingWork() const {
    std::lock_guard lk(_mutex);
    return _pending.lastTimestamp.has_value();
}

void SizeCountCheckpointBuffer::mergeVisibleScan(OplogScanResult scanResult) {
    if (!scanResult.lastTimestamp) {
        return;
    }

    std::lock_guard lk(_mutex);

    if (!_pending.lastTimestamp || *_pending.lastTimestamp < *scanResult.lastTimestamp) {
        _pending.lastTimestamp = *scanResult.lastTimestamp;
    }

    for (auto& [uuid, incoming] : scanResult.deltas) {
        auto it = _pending.deltas.find(uuid);
        if (it == _pending.deltas.end()) {
            _pending.deltas.emplace(uuid, std::move(incoming));
            continue;
        }

        auto& existing = it->second;
        switch (incoming.state) {
            case DDLState::kNone:
                tassert(12650000,
                        "Unexpected size/count delta merged after drop without recreate",
                        existing.state != DDLState::kDropped);
                existing.sizeCount = existing.sizeCount + incoming.sizeCount;
                break;

            case DDLState::kCreated:
                if (existing.state == DDLState::kDropped) {
                    existing = SizeCountDelta{
                        .sizeCount = incoming.sizeCount,
                        .state = DDLState::kDroppedAndRecreated,
                    };
                } else {
                    existing = std::move(incoming);
                }
                break;

            case DDLState::kDropped:
                if (existing.state == DDLState::kCreated) {
                    _pending.deltas.erase(it);
                } else {
                    existing = SizeCountDelta{
                        .sizeCount = CollectionSizeCount{},
                        .state = DDLState::kDropped,
                    };
                }
                break;

            case DDLState::kDroppedAndRecreated:
                existing = std::move(incoming);
                break;
        }
    }
}

}  // namespace mongo::replicated_fast_count
