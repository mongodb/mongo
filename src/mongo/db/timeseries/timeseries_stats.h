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

#pragma once

#include <boost/optional.hpp>

#include "mongo/db/catalog/collection.h"

namespace mongo {
/**
 * Timeseries statistics at Collection-level. Decorates 'SharedCollectionDecorations' and is not
 * versioned per Collection instance.
 *
 * Used for timeseries statistics the BucketCatalog is unable to keep track of.
 */
class TimeseriesStats {
public:
    static const TimeseriesStats& get(const Collection* coll);

    struct CompressedBucketInfo {
        Status result = Status::OK();
        int size = 0;
        int numInterleaveRestarts = 0;
        bool decompressionFailed = false;
    };

    /**
     * Records stats for a closed time-series bucket. 'boost::none' for compressed means
     * compression failed for any reason.
     */
    void onBucketClosed(int uncompressedSize, const CompressedBucketInfo& compressed) const;

    /**
     * Appends current stats to the given BSONObjBuilder.
     */
    void append(BSONObjBuilder* builder) const;

private:
    // We need to be able to record stats on a time-series bucket collection without requiring a
    // non-const Collection (which requires MODE_X collection lock).
    mutable AtomicWord<long long> _uncompressedSize;
    mutable AtomicWord<long long> _compressedSize;
    mutable AtomicWord<long long> _compressedSubObjRestart;
    mutable AtomicWord<long long> _numCompressedBuckets;
    mutable AtomicWord<long long> _numUncompressedBuckets;
    mutable AtomicWord<long long> _numFailedDecompressBuckets;
};
}  // namespace mongo
