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

#pragma once

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/platform/decimal128.h"

#include <cstdint>


namespace mongo {
class EncryptedField;
class EncryptedFieldConfig;
class QueryTypeConfig;

constexpr int kFLERangeSparsityDefault = 2;
constexpr int kFLERangeTrimFactorDefault = 6;

/*
 * Value: Value to attempt to coerce to field's type.
 * BSONType: Type of the field being queryed against.
 * StringData: A string parameter to support more informative error messages (currently expected
 * to only be either "bounds" (min and max parameters) or "literal", with "literal" being the
 * default)
 *
 * First, checks that the Value's type is supported on a range index. Then, the Value is
 * coerced if applicable:
 * - Int32 value and Int64 field --> coerce value to Int64
 * - Double value Decimal 128 field --> coerce value to Decimal 128
 */
Value coerceValueToRangeIndexTypes(Value val, BSONType fieldType);

void validateEncryptedField(const EncryptedField* field);
void validateEncryptedFieldConfig(const EncryptedFieldConfig* config);

bool validateDoublePrecisionRange(double d, uint32_t precision);
bool validateDecimal128PrecisionRange(Decimal128& dec, uint32_t precision);

void validateRangeIndex(BSONType fieldType, StringData fieldPath, QueryTypeConfig& query);
uint32_t getNumberOfBitsInDomain(BSONType fieldType,
                                 const boost::optional<Value>& min,
                                 const boost::optional<Value>& max,
                                 const boost::optional<uint32_t>& precision);
uint32_t getNumberOfBitsInDomain(BSONType fieldType,
                                 const boost::optional<BSONElement>& min,
                                 const boost::optional<BSONElement>& max,
                                 const boost::optional<uint32_t>& precision);

void setRangeDefaults(BSONType fieldType, StringData fieldPath, QueryTypeConfig* query);

/**
 * Maximum number of edges  (aka tags) that fit in a bson document. The actual limit is a higher but
 * we use a lower limit do allow for other content in a bson document.
 */
constexpr uint32_t kMaxTagLimit = 300000;
constexpr uint32_t kMaxTagLimitLog2 = 19;  // ceil(log2(kMaxTagLimit))

/**
 * Validate for a range field that sparsity and trimfactor to not lead to queries that would require
 * more edges then can fit in a BSON document.
 */
void validateRangeBounds(BSONType fieldType,
                         const boost::optional<Value>& min,
                         const boost::optional<Value>& max,
                         uint32_t sparsity,
                         uint32_t trimFactor,
                         const boost::optional<uint32_t>& precision);
void validateRangeBoundsInt32(const boost::optional<int32_t>& min,
                              const boost::optional<int32_t>& max,
                              uint32_t sparsity,
                              uint32_t trimFactor);
void validateRangeBoundsInt64(const boost::optional<int64_t>& min,
                              const boost::optional<int64_t>& max,
                              uint32_t sparsity,
                              uint32_t trimFactor);
void validateRangeBoundsDouble(const boost::optional<double>& min,
                               const boost::optional<double>& max,
                               uint32_t sparsity,
                               uint32_t trimFactor,
                               const boost::optional<uint32_t>& precision);
void validateRangeBoundsDecimal128(const boost::optional<Decimal128>& min,
                                   const boost::optional<Decimal128>& max,
                                   uint32_t sparsity,
                                   uint32_t trimFactor,
                                   const boost::optional<uint32_t>& precision);

void validateTextSearchIndex(BSONType fieldType,
                             StringData fieldPath,
                             QueryTypeConfig& query,
                             boost::optional<bool> previousCaseSensitivity,
                             boost::optional<bool> previousDiacriticSensitivity,
                             boost::optional<int64_t> previousContention);
}  // namespace mongo
