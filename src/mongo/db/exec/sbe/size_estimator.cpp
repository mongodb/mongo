// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/size_estimator.h"


namespace mongo::sbe::size_estimator {

size_t estimate(const IndexBounds& indexBounds) {
    size_t size = estimate(indexBounds.startKey);
    size += estimate(indexBounds.endKey);
    size += estimate(indexBounds.fields);
    return size;
}

size_t estimate(const OrderedIntervalList& list) {
    size_t size = estimate(list.name);
    size += estimate(list.intervals);
    return size;
}

size_t estimate(const Interval& interval) {
    size_t size = estimate(interval._intervalData);
    size += estimate(interval.start);
    size += estimate(interval.end);
    return size;
}

size_t estimate(const IndexSeekPoint& indexSeekPoint) {
    size_t size = estimate(indexSeekPoint.keyPrefix);
    size += estimate(indexSeekPoint.keySuffix);
    return size;
}

size_t estimate(const IndexBoundsChecker& checker) {
    size_t size = sbe::size_estimator::estimate(checker._curInterval);
    size += sbe::size_estimator::estimate(checker._expectedDirection);
    size += sbe::size_estimator::estimate(checker._keyValues);
    return size;
}
}  // namespace mongo::sbe::size_estimator
