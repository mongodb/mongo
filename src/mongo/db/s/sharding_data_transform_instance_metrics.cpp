/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/sharding_data_transform_instance_metrics.h"
#include "mongo/db/s/sharding_data_transform_metrics_observer.h"

namespace {
constexpr int64_t kPlaceholderTimestampForTesting = 0;
}

namespace mongo {

ShardingDataTransformInstanceMetrics::ShardingDataTransformInstanceMetrics(
    ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
    : ShardingDataTransformInstanceMetrics{
          cumulativeMetrics, std::make_unique<ShardingDataTransformMetricsObserver>(this)} {}

ShardingDataTransformInstanceMetrics::ShardingDataTransformInstanceMetrics(
    ShardingDataTransformCumulativeMetrics* cumulativeMetrics, ObserverPtr observer)
    : _observer{std::move(observer)},
      _cumulativeMetrics{cumulativeMetrics},
      _deregister{_cumulativeMetrics->registerInstanceMetrics(_observer.get())},
      _placeholderUuidForTesting(UUID::gen()) {}

ShardingDataTransformInstanceMetrics::~ShardingDataTransformInstanceMetrics() {
    if (_deregister) {
        _deregister();
    }
}

int64_t ShardingDataTransformInstanceMetrics::getRemainingTimeMillis() const {
    return _observer->getRemainingTimeMillis();
}

int64_t ShardingDataTransformInstanceMetrics::getStartTimestamp() const {
    return kPlaceholderTimestampForTesting;
}

const UUID& ShardingDataTransformInstanceMetrics::getUuid() const {
    return _placeholderUuidForTesting;
}

}  // namespace mongo
