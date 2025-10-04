/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/size_estimator.h"

#include "mongo/bson/bsonelement.h"

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
