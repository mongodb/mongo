/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/modules.h"

#include <cstddef>

#include <boost/optional.hpp>

namespace mongo::exec::expression::convert_utils {

/**
 * Parses the given JSON string into a Value and uasserts its type. Possible outcomes are an array,
 * object or a ConversionFailure. Used by $convert and the shorthand expressions $toArray and
 * $toObject.
 */
Value parseJson(StringData data, boost::optional<BSONType> expectedType);

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
 * Convert a binData vector Value to a std::vector<Value>.
 * The input must be a binData with BinDataType::Vector subtype.
 */
std::vector<Value> convertBinDataVectorToArray(const Value& val, bool isLittleEndian = true);

}  // namespace mongo::exec::expression::convert_utils
