// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string_view>

#include <boost/optional.hpp>

namespace mongo::exec::expression::convert_utils {

/**
 * Parses the given JSON string into a Value and uasserts its type. Possible outcomes are an array,
 * object or a ConversionFailure. Used by $convert and the shorthand expressions $toArray and
 * $toObject.
 */
Value parseJson(std::string_view data, boost::optional<BSONType> expectedType);

/**
 * Data type for BinData vectors (per BinDataVector specification). See:
 * https://github.com/mongodb/specifications/blob/9d0d3f0042a8cf5faeb47ae7765716151bfca9ef/source/bson-binary-vector/bson-binary-vector.md#data-types-dtype.
 * More types may eventually be useful. Note that the order of these enum values is important,
 * because we do less-than/greater-than comparisons with enum values when determining what vector
 * type to convert to.
 */
enum class dType {
    PACKED_BIT,
    INT8,
    FLOAT32  // Note this will truncate doubles.
};

/**
 * Data type byte constants for BinData vectors.
 */
constexpr std::byte kPackedBitDataTypeByte{0x10};
constexpr std::byte kInt8DataTypeByte{0x03};
constexpr std::byte kFloat32DataTypeByte{0x27};

/**
 * Lightweight view over a BinData vector's raw bytes, avoiding Value allocations.
 * Returned by parseBinDataVector().
 */
struct BinDataVectorView {
    dType dtype;
    int padding;            // 0 for FLOAT32/INT8, 0-7 for PACKED_BIT
    const std::byte* data;  // pointer past the 2-byte header
    int dataLength;         // number of raw bytes of element data
    size_t elementCount;    // number of logical elements
};

/**
 * Parse a BinData vector's header and return a view over the raw data.
 * Returns boost::none for zero-length bindata (no header). Returns a view with
 * elementCount == 0 for header-only vectors (dtype + padding, no data bytes).
 */
boost::optional<BinDataVectorView> parseBinDataVector(const BSONBinData& binData);

/**
 * Convert a binData vector Value to a std::vector<Value>.
 * The input must be a binData with BinDataType::Vector subtype.
 */
std::vector<Value> convertBinDataVectorToArray(const Value& val, bool isLittleEndian = true);

}  // namespace mongo::exec::expression::convert_utils
