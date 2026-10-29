// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/column/bsoncolumnbuilder.h"
#include "mongo/util/modules.h"
#include "mongo/util/tracking/string_map.h"

#include <string_view>
#include <utility>
#include <vector>

[[MONGO_MOD_PARENT_PRIVATE]];
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
    void insertOne(const BSONObj& measurement, boost::optional<std::string_view> metaField);

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
    std::vector<
        std::pair<std::string_view, BSONColumnBuilder<tracking::Allocator<void>>::BinaryDiff>>
    intermediate(int32_t& compressedSizeDelta);

    /**
     * Returns the date of the last measurement in the time column.
     */
    Date_t timeOfLastMeasurement(std::string_view timeField) const;

    size_t numFields() const {
        return _builders.size();
    }

private:
    void _insertNewKey(std::string_view key, const BSONElement& elem, size_t count);

    std::reference_wrapper<tracking::Context> _trackingContext;

    // BSONColumnBuilder with a count of how many elements appended/skipped has been added to it.
    struct BuilderWithCount {
        BSONColumnBuilder<tracking::Allocator<void>> builder;
        size_t count;
    };
    // Maps user measurement field names to BSONColumnBuilders with counts.
    tracking::StringMap<BuilderWithCount> _builders;
    size_t _measurementCount{0};

    // The size of the compressed binary data across all builders since the last call to
    // intermediate().
    int32_t _compressedSize{0};
};

}  // namespace mongo::timeseries::bucket_catalog
