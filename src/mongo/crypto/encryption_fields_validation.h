// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string_view>


namespace [[MONGO_MOD_PUBLIC]] mongo {
class EncryptedField;
class EncryptedFieldConfig;
class QueryTypeConfig;

constexpr int kFLERangeSparsityDefault = 2;
constexpr int kFLERangeTrimFactorDefault = 6;

/*
 * Value: Value to attempt to coerce to field's type.
 * BSONType: Type of the field being queryed against.
 * std::string_view: A string parameter to support more informative error messages (currently
 * expected to only be either "bounds" (min and max parameters) or "literal", with "literal" being
 * the default)
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

void validateRangeIndex(BSONType fieldType, std::string_view fieldPath, QueryTypeConfig& query);
uint32_t getNumberOfBitsInDomain(BSONType fieldType,
                                 const boost::optional<Value>& min,
                                 const boost::optional<Value>& max,
                                 const boost::optional<uint32_t>& precision);
uint32_t getNumberOfBitsInDomain(BSONType fieldType,
                                 const boost::optional<BSONElement>& min,
                                 const boost::optional<BSONElement>& max,
                                 const boost::optional<uint32_t>& precision);

void setRangeDefaults(BSONType fieldType, std::string_view fieldPath, QueryTypeConfig* query);

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
                             std::string_view fieldPath,
                             QueryTypeConfig& query,
                             boost::optional<bool> previousCaseSensitivity,
                             boost::optional<bool> previousDiacriticSensitivity,
                             boost::optional<int64_t> previousContention);
}  // namespace mongo
