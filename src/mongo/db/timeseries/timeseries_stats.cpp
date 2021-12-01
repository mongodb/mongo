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

#include "mongo/db/timeseries/timeseries_stats.h"

#include "mongo/db/catalog/collection.h"

namespace mongo {
namespace {

const auto getTimeseriesStatsDecoration =
    SharedCollectionDecorations::declareDecoration<TimeseriesStats>();

}  // namespace

const TimeseriesStats& TimeseriesStats::get(const Collection* coll) {
    return getTimeseriesStatsDecoration(coll->getSharedDecorations());
}

void TimeseriesStats::onBucketClosed(int uncompressedSize,
                                     const CompressedBucketInfo& compressed) const {
    _uncompressedSize.fetchAndAddRelaxed(uncompressedSize);
    if (compressed.result.isOK() && compressed.size > 0) {
        _compressedSize.fetchAndAddRelaxed(compressed.size);
        _compressedSubObjRestart.fetchAndAddRelaxed(compressed.numInterleaveRestarts);
        _numCompressedBuckets.fetchAndAddRelaxed(1);
    } else {
        _compressedSize.fetchAndAddRelaxed(uncompressedSize);
        _numUncompressedBuckets.fetchAndAddRelaxed(1);

        if (compressed.decompressionFailed) {
            _numFailedDecompressBuckets.fetchAndAddRelaxed(1);
        }
    }
}

void TimeseriesStats::append(BSONObjBuilder* builder) const {
    builder->appendNumber("numBytesUncompressed", _uncompressedSize.load());
    builder->appendNumber("numBytesCompressed", _compressedSize.load());
    builder->appendNumber("numSubObjCompressionRestart", _compressedSubObjRestart.load());
    builder->appendNumber("numCompressedBuckets", _numCompressedBuckets.load());
    builder->appendNumber("numUncompressedBuckets", _numUncompressedBuckets.load());
    builder->appendNumber("numFailedDecompressBuckets", _numFailedDecompressBuckets.load());
}

}  // namespace mongo
