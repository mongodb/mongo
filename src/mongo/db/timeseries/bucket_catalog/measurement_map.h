/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/column/bsoncolumnbuilder.h"
#include "mongo/util/tracking/string_map.h"

#include <utility>
#include <vector>

namespace mongo::timeseries::bucket_catalog {

/**
 * A map that stores measurements by field names to compressed column builders, and fills in skips
 * for missing data fields.
 */
class MeasurementMap {
public:
    explicit MeasurementMap(tracking::Context& trackingContext);

    /**
     * Inserts one measurement.
     * Will not insert data fields with 'metaField' key.
     * Will account for skips:
     * - A new data field is added that wasn't in the map before - adds a number of skips equal to
     * the number of existing measurements in all builders prior to the insert into the builder of
     * the new data field.
     * - An existing data field is missing in this measurement - adds a skip to the builder of the
     * missing data field.
     */
    void insertOne(const BSONObj& measurement, boost::optional<StringData> metaField);

    /**
     * Sets internal state of builders to that of pre-existing compressed builders.
     * numMeasurements should be equal to the number of measurements in every data field in the
     * bucket.
     */
    void initBuilders(BSONObj bucketDataDocWithCompressedBuilders, size_t numMeasurements);

    /**
     * Calls BSONColumnBuilder::intermediate() for all builders. Updates the compressed size both
     * internally as well as the one passed in.
     */
    std::vector<std::pair<StringData, BSONColumnBuilder<tracking::Allocator<void>>::BinaryDiff>>
    intermediate(int32_t& compressedSizeDelta);

    /**
     * Returns the timestamp of the last measurement in the time column.
     */
    Timestamp timeOfLastMeasurement(StringData timeField) const;

    size_t numFields() const {
        return _builders.size();
    }

private:
    /**
     * Inserts skips where needed to all builders. Must be called after inserting one measurement.
     * Cannot call this after multiple measurements have been inserted.
     */
    void _fillSkipsInMissingFields(const std::set<StringData>& fieldsSeen);

    void _insertNewKey(StringData key,
                       const BSONElement& elem,
                       BSONColumnBuilder<tracking::Allocator<void>> builder);

    std::reference_wrapper<tracking::Context> _trackingContext;
    tracking::StringMap<BSONColumnBuilder<tracking::Allocator<void>>> _builders;
    size_t _measurementCount{0};

    // The size of the compressed binary data across all builders since the last call to
    // intermediate().
    int32_t _compressedSize{0};
};

}  // namespace mongo::timeseries::bucket_catalog
