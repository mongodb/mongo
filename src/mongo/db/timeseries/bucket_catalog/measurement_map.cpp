// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/bucket_catalog/measurement_map.h"

#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/testing_proctor.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::timeseries::bucket_catalog {

MeasurementMap::MeasurementMap(tracking::Context& trackingContext)
    : _trackingContext(trackingContext),
      _builders(tracking::makeStringMap<BuilderWithCount>(_trackingContext)) {}

void MeasurementMap::initBuilders(BSONObj bucketDataDocWithCompressedBuilders,
                                  size_t numMeasurements) {
    for (auto&& [key, columnValue] : bucketDataDocWithCompressedBuilders) {
        str::stream errMsg;
        errMsg << "Compressed bucket contains uncompressed data field: " << key;
        massert(8830600, errMsg, columnValue.isBinData(BinDataType::Column));

        int binLength = 0;
        const char* binData = columnValue.binData(binLength);

        _compressedSize += binLength;
        _builders.try_emplace(tracking::make_string(_trackingContext, key.data(), key.size()),
                              BSONColumnBuilder<tracking::Allocator<void>>(
                                  binData, binLength, _trackingContext.get().makeAllocator<void>()),
                              numMeasurements);
    }
    _measurementCount = numMeasurements;
    if (TestingProctor::instance().isEnabled()) {
        for (auto&& [key, columnValue] : bucketDataDocWithCompressedBuilders) {
            int binLength = 0;
            const char* binData = columnValue.binData(binLength);
            tracking::Context trackingContext;
            BSONColumnBuilder<tracking::Allocator<void>> builderToCompareTo{
                trackingContext.makeAllocator<void>()};
            BSONColumn c(binData, binLength);
            for (auto&& elem : c) {
                builderToCompareTo.append(elem);
            }
            [[maybe_unused]] auto diff = builderToCompareTo.intermediate();
            auto it = _builders.find(key);
            bool isInternalStateCorrect =
                it->second.builder.isInternalStateIdentical(builderToCompareTo);
            if (!isInternalStateCorrect) {
                LOGV2_OPTIONS(
                    10402,
                    logv2::LogTruncation::Disabled,
                    "Detected incorrect internal state when reopening from following binary: ",
                    "binary"_attr = base64::encode(std::string_view(binData, binLength)));
            }
            invariant(isInternalStateCorrect);
        }
    }
}

std::vector<std::pair<std::string_view, BSONColumnBuilder<tracking::Allocator<void>>::BinaryDiff>>
MeasurementMap::intermediate(int32_t& compressedSizeDelta) {
    int32_t previousCompressedSize = _compressedSize;
    _compressedSize = 0;

    std::vector<
        std::pair<std::string_view, BSONColumnBuilder<tracking::Allocator<void>>::BinaryDiff>>
        intermediates;
    for (auto& entry : _builders) {
        auto& builder = entry.second.builder;
        auto diff = builder.intermediate();

        _compressedSize += (diff.offset() + diff.size());
        intermediates.push_back(
            {std::string_view(entry.first.c_str(), entry.first.size()), std::move(diff)});
    }

    compressedSizeDelta = _compressedSize - previousCompressedSize;
    return intermediates;
}

void MeasurementMap::_insertNewKey(std::string_view key, const BSONElement& elem, size_t count) {
    BSONColumnBuilder<tracking::Allocator<void>> columnBuilder(
        count, _trackingContext.get().makeAllocator<void>());
    columnBuilder.append(elem);
    _builders.try_emplace(tracking::make_string(_trackingContext, key.data(), key.size()),
                          std::move(columnBuilder),
                          count + 1 /* account for the append above */);
}

void MeasurementMap::insertOne(const BSONObj& measurement,
                               boost::optional<std::string_view> metaField) {
    // First pass to append all elements present in this measurement.
    for (const auto& elem : measurement) {
        std::string_view key = elem.fieldNameStringData();
        // Skip the meta field values because they aren't stored in a BSONColumn.
        if (key == metaField) {
            continue;
        }

        auto builderIt = _builders.find(key);
        if (builderIt == _builders.end()) {
            _insertNewKey(key, elem, _measurementCount);
        } else {
            builderIt->second.builder.append(elem);

            uassert(12602102,
                    "Measurements with duplicate field names cannot be stored in timeseries "
                    "collections",
                    builderIt->second.count++ <= _measurementCount);
        }
    }

    // Increment our total measurement count. Needs to be after the first pass above so that
    // builders for new keys are initialized with the correct count.
    ++_measurementCount;

    // Perform a second pass over our builders and perform a skip for the ones that did not get an
    // element appended to them in the first pass above.
    for (auto&& entry : _builders) {
        if (entry.second.count < _measurementCount) {
            entry.second.builder.skip();
            ++entry.second.count;
        }
    }
}

Date_t MeasurementMap::timeOfLastMeasurement(std::string_view key) const {
    const auto it = _builders.find(key);
    invariant(it != _builders.end());
    return it->second.builder.last().date();
}

}  // namespace mongo::timeseries::bucket_catalog
