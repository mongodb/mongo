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

#include "encryption_fields_validation.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/container/small_vector.hpp>
#include <boost/cstdint.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/fle_numeric.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/namespace_string.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <boost/move/utility_core.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {


Value coerceValueToRangeIndexTypes(Value val, BSONType fieldType) {
    BSONType valType = val.getType();

    if (valType == fieldType)
        return val;

    if (valType == BSONType::date || fieldType == BSONType::date) {
        uassert(6720002,
                "If the value type is a date, the type of the index must also be date (and vice "
                "versa). ",
                valType == fieldType);
        return val;
    }

    uassert(6742000,
            str::stream() << "type" << valType
                          << " type isn't supported for the range encrypted index. ",
            isNumericBSONType(valType));

    // If we get to this point, we've already established that valType and fieldType are NOT the
    // same type, so if either of them is a double or a decimal we can't coerce.
    if (valType == BSONType::numberDecimal || valType == BSONType::numberDouble ||
        fieldType == BSONType::numberDecimal || fieldType == BSONType::numberDouble) {
        uasserted(
            6742002,
            str::stream() << "If the value type and the field type are not the same type and one "
                             "or both of them is a double or a decimal, coercion of the value to "
                             "field type is not supported, due to possible loss of precision.");
    }

    switch (fieldType) {
        case BSONType::numberInt:
            return Value(val.coerceToInt());
        case BSONType::numberLong:
            return Value(val.coerceToLong());
        default:
            MONGO_UNREACHABLE;
    }
}

namespace {
int32_t bsonToInt(const BSONElement& e) {
    return e.Int();
}
int64_t bsonToLong(const BSONElement& e) {
    return e.Long();
}
int64_t bsonToDateToLong(const BSONElement& e) {
    return e.Date().toMillisSinceEpoch();
}
double bsonToDouble(const BSONElement& e) {
    return e.Double();
}
Decimal128 bsonToDecimal(const BSONElement& e) {
    return e.Decimal();
}
int32_t valToInt(const Value& e) {
    return e.coerceToInt();
}
int64_t valToLong(const Value& e) {
    return e.coerceToLong();
}
int64_t valToDateToLong(const Value& e) {
    return e.coerceToDate().toMillisSinceEpoch();
}
double valToDouble(const Value& e) {
    return e.coerceToDouble();
}
Decimal128 valToDecimal(const Value& e) {
    return e.coerceToDecimal();
}

uint32_t getNumberOfBitsInDomain(const boost::optional<int32_t>& min,
                                 const boost::optional<int32_t>& max) {
    auto info = getTypeInfo32(min.value_or(0), min, max);
    return 64 - countLeadingZeros64(info.max);
}

uint32_t getNumberOfBitsInDomain(const boost::optional<int64_t>& min,
                                 const boost::optional<int64_t>& max) {
    auto info = getTypeInfo64(min.value_or(0), min, max);
    return 64 - countLeadingZeros64(info.max);
}

uint32_t getNumberOfBitsInDomain(const boost::optional<double>& min,
                                 const boost::optional<double>& max,
                                 const boost::optional<uint32_t>& precision) {
    auto info = getTypeInfoDouble(min.value_or(0), min, max, precision);
    return 64 - countLeadingZeros64(info.max);
}

uint32_t getNumberOfBitsInDomain(const boost::optional<Decimal128>& min,
                                 const boost::optional<Decimal128>& max,
                                 const boost::optional<uint32_t>& precision) {
    auto info =
        getTypeInfoDecimal128(min.value_or(Decimal128::kNormalizedZero), min, max, precision);
    try {
        return boost::multiprecision::msb(info.max) + 1;
    } catch (const std::domain_error&) {
        // no bits set case
        return 0;
    }
}

std::pair<mongo::Value, mongo::Value> getRangeMinMaxDefaults(BSONType fieldType) {
    switch (fieldType) {
        case BSONType::numberDouble:
            return {mongo::Value(std::numeric_limits<double>::lowest()),
                    mongo::Value(std::numeric_limits<double>::max())};
        case BSONType::numberDecimal:
            return {mongo::Value(Decimal128::kLargestNegative),
                    mongo::Value(Decimal128::kLargestPositive)};
        case BSONType::numberInt:
            return {mongo::Value(std::numeric_limits<int>::min()),
                    mongo::Value(std::numeric_limits<int>::max())};
        case BSONType::numberLong:
            return {mongo::Value(std::numeric_limits<long long>::min()),
                    mongo::Value(std::numeric_limits<long long>::max())};
        case BSONType::date:
            return {mongo::Value(Date_t::min()), mongo::Value(Date_t::max())};
        default:
            uasserted(7018202, "Range index only supports numeric types and the Date type.");
    }
    MONGO_UNREACHABLE;
}

uint64_t exp2UInt64(uint32_t exp) {
    uassert(9203501, "Exponent out of bounds for uint64", exp < 64);

    return 1ULL << exp;
}

// Validates that this assertion is true:
//
//            sp-1      tf
// min ( n, 2^     * (2^   + 2 log2(n) - 1 ) )  < CBSON
//
void validateRangeBoundsBase(double domainSizeLog2, uint32_t sparsity, uint32_t trimFactor) {
    uassert(
        9203502, "domainSizeLog2 is out of bounds", domainSizeLog2 > 0 && domainSizeLog2 <= 128);

    // Before we do the formula, sanity check that 2^tf * 2^{sp-1} = 2^{tf + sp - 1} <
    // ceil(log2(CBSON)) is sane
    uassert(9203504,
            "Sparsity and trimFactor together are too large and could create queries that exceed "
            "the BSON size limit",

            (trimFactor + sparsity - 1) < kMaxTagLimitLog2);

    // Since we now know that {tf + sp - 1} < ceil(log2(CBSON)), which means that the remainder of
    // the formula:  2log2(N) -1 cannot cause us to overflow a double.

    // trimfactor = 1 .. log2(bits) so 2^tf should be less then max_size of a type but we bounds
    // check anyway
    uint64_t tf_exp = exp2UInt64(trimFactor);

    // sparsity = 1 .. 4 so 2^{sp-1} should be less then max_size of a type but we bounds check
    // anyway
    uint64_t sp_exp = exp2UInt64(sparsity);

    // Compute the number of bits we need
    // domainSizeLog2 is <= 128 at this point

    double log_part2 = 2 * domainSizeLog2 - 1;
    // log_part2 is <= 256 at this point

    double log_part3 = log_part2 + tf_exp;

    double total = sp_exp * log_part3;

    uassert(9203508,
            "Sparsity, trimFactor, min, and max together are too large and could create queries "
            "that exceed the BSON size limit",
            total < kMaxTagLimit);
}

template <typename T>
void validateRangeBoundsInt(T typeInfo, uint32_t sparsity, uint32_t trimFactor) {

    if (typeInfo.max < kMaxTagLimit) {
        return;
    }

    double domainSizeLog2 = sizeof(typeInfo.max) * 8;

    if (typeInfo.max < std::numeric_limits<decltype(typeInfo.max)>::max()) {
        // +1 since we want the number of values between min and max, inclusive
        domainSizeLog2 = log2(typeInfo.max - typeInfo.min + 1);
    }

    // Compute the number of bits we need
    // domainSizeLog2 is <= 128 at this point
    validateRangeBoundsBase(domainSizeLog2, sparsity, trimFactor);
}

bool isTextSearchQueryType(QueryTypeEnum e) {
    switch (e) {
        case QueryTypeEnum::SubstringPreview:
        case QueryTypeEnum::SuffixPreview:
        case QueryTypeEnum::PrefixPreview:
            return true;
        default:
            return false;
    }
}

}  // namespace

uint32_t getNumberOfBitsInDomain(BSONType fieldType,
                                 const boost::optional<BSONElement>& min,
                                 const boost::optional<BSONElement>& max,
                                 const boost::optional<uint32_t>& precision) {
    uassert(8574112,
            "Precision may only be set when type is double or decimal",
            !precision || fieldType == BSONType::numberDouble ||
                fieldType == BSONType::numberDecimal);
    switch (fieldType) {
        case BSONType::numberInt:
            return getNumberOfBitsInDomain(min.map(bsonToInt), max.map(bsonToInt));
        case BSONType::numberLong:
            return getNumberOfBitsInDomain(min.map(bsonToLong), max.map(bsonToLong));
        case BSONType::date:
            return getNumberOfBitsInDomain(min.map(bsonToDateToLong), max.map(bsonToDateToLong));
        case BSONType::numberDouble:
            return getNumberOfBitsInDomain(min.map(bsonToDouble), max.map(bsonToDouble), precision);
        case BSONType::numberDecimal:
            return getNumberOfBitsInDomain(
                min.map(bsonToDecimal), max.map(bsonToDecimal), precision);
        default:
            uasserted(8574107,
                      "Field type is invalid; must be one of int, long, date, double, or decimal");
    }
}


uint32_t getNumberOfBitsInDomain(BSONType fieldType,
                                 const boost::optional<Value>& min,
                                 const boost::optional<Value>& max,
                                 const boost::optional<uint32_t>& precision) {
    uassert(8574113,
            "Precision may only be set when type is double or decimal",
            !precision || fieldType == BSONType::numberDouble ||
                fieldType == BSONType::numberDecimal);
    switch (fieldType) {
        case BSONType::numberInt:
            return getNumberOfBitsInDomain(min.map(valToInt), max.map(valToInt));
        case BSONType::numberLong:
            return getNumberOfBitsInDomain(min.map(valToLong), max.map(valToLong));
        case BSONType::date:
            return getNumberOfBitsInDomain(min.map(valToDateToLong), max.map(valToDateToLong));
        case BSONType::numberDouble:
            return getNumberOfBitsInDomain(min.map(valToDouble), max.map(valToDouble), precision);
        case BSONType::numberDecimal:
            return getNumberOfBitsInDomain(min.map(valToDecimal), max.map(valToDecimal), precision);
        default:
            uasserted(8574114,
                      "Field type is invalid; must be one of int, long, date, double, or decimal");
    }
}

void validateRangeIndex(BSONType fieldType, StringData fieldPath, QueryTypeConfig& query) {
    uassert(6775201,
            fmt::format("Type '{}' is not a supported range indexed type", typeName(fieldType)),
            isFLE2RangeIndexedSupportedType(fieldType));

    // Text search fields are not allowed.
    uassert(10774906,
            "The field 'strMaxLength' is not allowed for range index but is present",
            !query.getStrMaxLength().has_value());
    uassert(10774907,
            "The field 'strMinQueryLength' is not allowed for range index but is present",
            !query.getStrMinQueryLength().has_value());
    uassert(10774908,
            "The field 'strMaxQueryLength' is not allowed for range index but is present",
            !query.getStrMaxQueryLength().has_value());
    uassert(10774909,
            "The field 'caseSensitive' is not allowed for range index but is present",
            !query.getCaseSensitive().has_value());
    uassert(10774910,
            "The field 'diacriticSensitive' is not allowed for range index but is present",
            !query.getDiacriticSensitive().has_value());

    auto& indexMin = query.getMin();
    auto& indexMax = query.getMax();

    if (query.getSparsity().has_value()) {
        uassert(6775214,
                "The field 'sparsity' must be between 1 and 8",
                query.getSparsity().value() >= 1 && query.getSparsity().value() <= 8);
    }

    if (indexMin) {
        uassert(7018200,
                fmt::format("Range field type '{}' does not match the min value type '{}'",
                            typeName(fieldType),
                            typeName(indexMin->getType())),
                fieldType == indexMin->getType());
    }
    if (indexMax) {
        uassert(7018201,
                fmt::format("Range field type '{}' does not match the max value type '{}'",
                            typeName(fieldType),
                            typeName(indexMax->getType())),
                fieldType == indexMax->getType());
    }
    if (indexMin && indexMax) {
        uassert(6720005,
                "Min must be less than max.",
                Value::compare(*indexMin, *indexMax, nullptr) < 0);
    }

    if (fieldType == BSONType::numberDouble || fieldType == BSONType::numberDecimal) {
        if (!((indexMin.has_value() == indexMax.has_value()) &&
              (indexMin.has_value() == query.getPrecision().has_value()))) {
            uasserted(6967100,
                      str::stream() << "Precision, min, and max must all be specified "
                                    << "together for floating point fields");
        }
        if (query.getPrecision().has_value()) {
            uint32_t precision = query.getPrecision().value();
            if (fieldType == BSONType::numberDouble) {
                auto min = query.getMin()->coerceToDouble();
                uassert(6966805,
                        fmt::format("The number of decimal digits for minimum value of field '{}' "
                                    "must be less than or equal to precision",
                                    fieldPath),
                        validateDoublePrecisionRange(min, precision));
                auto max = query.getMax()->coerceToDouble();
                uassert(6966806,
                        fmt::format("The number of decimal digits for maximum value of field '{}' "
                                    "must be less than or equal to precision",
                                    fieldPath),
                        validateDoublePrecisionRange(max, precision));
                uassert(9157100,
                        fmt::format(
                            "The domain of double values specified by the min, max, and precision "
                            "for field '{}' cannot be represented in fewer than 64 bits",
                            fieldPath),
                        query.getQueryType() == QueryTypeEnum::RangePreviewDeprecated ||
                            canUsePrecisionMode(min, max, precision));
            } else {
                auto minDecimal = query.getMin()->coerceToDecimal();
                uassert(6966807,
                        fmt::format("The number of decimal digits for minimum value of field '{}' "
                                    "must be less than or equal to precision",
                                    fieldPath),
                        validateDecimal128PrecisionRange(minDecimal, precision));
                auto maxDecimal = query.getMax()->coerceToDecimal();
                uassert(6966808,
                        fmt::format("The number of decimal digits for maximum value of field '{}' "
                                    "must be less than or equal to precision",
                                    fieldPath),
                        validateDecimal128PrecisionRange(maxDecimal, precision));
                uassert(9157101,
                        fmt::format(
                            "The domain of decimal values specified by the min, max, and precision "
                            "for field '{}' cannot be represented in fewer than 128 bits",
                            fieldPath),
                        query.getQueryType() == QueryTypeEnum::RangePreviewDeprecated ||
                            canUsePrecisionMode(minDecimal, maxDecimal, precision));
            }
        }
    }

    if (query.getTrimFactor().has_value()) {
        uint32_t tf = query.getTrimFactor().value();
        auto precision = query.getPrecision().map([](int32_t i) { return (uint32_t)(i); });

        auto [defMin, defMax] = getRangeMinMaxDefaults(fieldType);
        uint32_t bits = getNumberOfBitsInDomain(
            fieldType, query.getMin().value_or(defMin), query.getMax().value_or(defMax), precision);

        // We allow the case where #bits = TF = 0.
        uassert(8574000,
                fmt::format("The field 'trimFactor' must be >= 0 and less than the total "
                            "number of bits needed to represent elements in the domain ({})",
                            bits),
                tf == 0 || tf < bits);

        validateRangeBounds(fieldType,
                            query.getMin().value_or(defMin),
                            query.getMax().value_or(defMax),
                            query.getSparsity().value_or(kFLERangeSparsityDefault),
                            tf,
                            precision);
    }
}

void validateTextSearchIndex(BSONType fieldType,
                             StringData fieldPath,
                             QueryTypeConfig& query,
                             boost::optional<bool> previousCaseSensitivity,
                             boost::optional<bool> previousDiacriticSensitivity,
                             boost::optional<std::int64_t> previousContention) {
    uassert(
        9783400,
        fmt::format("Type '{}' is not a supported type for text search indexed encrypted field {}",
                    typeName(fieldType),
                    fieldPath),
        fieldType == BSONType::string);

    // Range search fields not allowed.
    uassert(10774911,
            "The field 'sparsity' is not allowed for text-based index but is present",
            !query.getSparsity().has_value());
    uassert(10774912,
            "The field 'min' is not allowed for text-based index but is present",
            !query.getMin().has_value());
    uassert(10774913,
            "The field 'max' is not allowed for text-based index but is present",
            !query.getMax().has_value());
    uassert(10774914,
            "The field 'trimFactor' is not allowed for text-based index but is present",
            !query.getTrimFactor().has_value());
    uassert(10774915,
            "The field 'precision' is not allowed for text-based index but is present",
            !query.getPrecision().has_value());
    auto qTypeStr = QueryType_serializer(query.getQueryType());

    uassert(9783401,
            "Query type is not a text search query type",
            isTextSearchQueryType(query.getQueryType()));

    uassert(9783402,
            fmt::format("strMinQueryLength parameter is required for {} query type of field {}",
                        qTypeStr,
                        fieldPath),
            query.getStrMinQueryLength().has_value());
    uassert(9783403,
            fmt::format("strMaxQueryLength parameter is required for {} query type of field {}",
                        qTypeStr,
                        fieldPath),
            query.getStrMaxQueryLength().has_value());
    uassert(9783404,
            fmt::format("caseSensitive parameter is required for {} query type of field {}",
                        qTypeStr,
                        fieldPath),
            query.getCaseSensitive().has_value());
    uassert(9783405,
            fmt::format("diacriticSensitive parameter is required for {} query type of field {}",
                        qTypeStr,
                        fieldPath),
            query.getDiacriticSensitive().has_value());
    uassert(9783406,
            "strMinQueryLength cannot be greater than strMaxQueryLength",
            query.getStrMinQueryLength().value() <= query.getStrMaxQueryLength().value());

    if (query.getQueryType() == QueryTypeEnum::SubstringPreview) {
        uassert(9783407,
                fmt::format("strMaxLength parameter is required for {} query type of field {}",
                            qTypeStr,
                            fieldPath),
                query.getStrMaxLength().has_value());
        uassert(9783408,
                "strMaxQueryLength cannot be greater than strMaxLength",
                query.getStrMaxQueryLength().value() <= query.getStrMaxLength().value());
    }

    if (previousCaseSensitivity.has_value() &&
        query.getCaseSensitive().value() != *previousCaseSensitivity) {
        uasserted(
            9783409,
            fmt::format("caseSensitive parameter must be the same for all query types of field {}",
                        fieldPath));
    }
    if (previousDiacriticSensitivity.has_value() &&
        query.getDiacriticSensitive().value() != *previousDiacriticSensitivity) {
        uasserted(
            9783410,
            fmt::format(
                "diacriticSensitive parameter must be the same for all query types of field {}",
                fieldPath));
    }
    if (previousContention.has_value() && query.getContention() != *previousContention) {
        uasserted(
            9783411,
            fmt::format("contention parameter must be the same for all query types of field {}",
                        fieldPath));
    };
}

void validateEncryptedField(const EncryptedField* field) {
    if (field->getQueries().has_value()) {
        std::vector<QueryTypeConfig> queryTypeConfigs;
        visit(OverloadedVisitor{
                  [&queryTypeConfigs](QueryTypeConfig config) {
                      queryTypeConfigs.push_back(std::move(config));
                  },
                  [&queryTypeConfigs](std::vector<QueryTypeConfig> configs) {
                      uassert(9783412,
                              "At least one query type should be specified per field",
                              configs.size() >= 1);
                      queryTypeConfigs = std::move(configs);
                  },
              },
              field->getQueries().value());

        uassert(6412601,
                "BSON type needs to be specified for an indexed field",
                field->getBsonType().has_value());
        auto fieldType = typeFromName(field->getBsonType().value());

        if (queryTypeConfigs.size() > 1) {
            uassert(9783413,
                    "The number of query types for an encrypted field cannot exceed two",
                    queryTypeConfigs.size() == 2);
            auto qtype1 = queryTypeConfigs.front().getQueryType();
            auto qtype2 = queryTypeConfigs.back().getQueryType();
            uassert(9783414,
                    fmt::format("Multiple query types may only include the {} and {} query types",
                                QueryType_serializer(QueryTypeEnum::SuffixPreview),
                                QueryType_serializer(QueryTypeEnum::PrefixPreview)),
                    (qtype1 == QueryTypeEnum::SuffixPreview &&
                     qtype2 == QueryTypeEnum::PrefixPreview) ||
                        (qtype2 == QueryTypeEnum::SuffixPreview &&
                         qtype1 == QueryTypeEnum::PrefixPreview));
            validateTextSearchIndex(fieldType,
                                    field->getPath(),
                                    queryTypeConfigs.front(),
                                    boost::none,
                                    boost::none,
                                    boost::none);
            validateTextSearchIndex(fieldType,
                                    field->getPath(),
                                    queryTypeConfigs.back(),
                                    queryTypeConfigs.front().getCaseSensitive(),
                                    queryTypeConfigs.front().getDiacriticSensitive(),
                                    queryTypeConfigs.front().getContention());
            return;
        }

        auto& encryptedIndex = queryTypeConfigs.front();
        switch (encryptedIndex.getQueryType()) {
            case QueryTypeEnum::Equality:
                uassert(6338405,
                        str::stream() << "Type '" << typeName(fieldType)
                                      << "' is not a supported equality indexed type",
                        isFLE2EqualityIndexedSupportedType(fieldType));
                uassert(6775205,
                        "The field 'sparsity' is not allowed for equality index but is present",
                        !encryptedIndex.getSparsity().has_value());
                uassert(6775206,
                        "The field 'min' is not allowed for equality index but is present",
                        !encryptedIndex.getMin().has_value());
                uassert(6775207,
                        "The field 'max' is not allowed for equality index but is present",
                        !encryptedIndex.getMax().has_value());
                uassert(8574104,
                        "The field 'trimFactor' is not allowed for equality index but is present",
                        !encryptedIndex.getTrimFactor().has_value());
                uassert(10774900,
                        "The field 'precision' is not allowed for equality index but is present",
                        !encryptedIndex.getPrecision().has_value());
                uassert(10774901,
                        "The field 'strMaxLength' is not allowed for equality index but is present",
                        !encryptedIndex.getStrMaxLength().has_value());
                uassert(10774902,
                        "The field 'strMinQueryLength' is not allowed for equality index but is "
                        "present",
                        !encryptedIndex.getStrMinQueryLength().has_value());
                uassert(10774903,
                        "The field 'strMaxQueryLength' is not allowed for equality index but is "
                        "present",
                        !encryptedIndex.getStrMaxQueryLength().has_value());
                uassert(
                    10774904,
                    "The field 'caseSensitive' is not allowed for equality index but is present",
                    !encryptedIndex.getCaseSensitive().has_value());
                uassert(10774905,
                        "The field 'diacriticSensitive' is not allowed for equality index but is "
                        "present",
                        !encryptedIndex.getDiacriticSensitive().has_value());
                break;
            case QueryTypeEnum::RangePreviewDeprecated:
                // rangePreview is renamed to range in Range V2, but we still need to accept it as
                // valid so that we can start up with existing rangePreview collections.
            case QueryTypeEnum::Range: {
                validateRangeIndex(fieldType, field->getPath(), encryptedIndex);
                break;
            }
            case QueryTypeEnum::SubstringPreview:
            case QueryTypeEnum::SuffixPreview:
            case QueryTypeEnum::PrefixPreview: {
                validateTextSearchIndex(fieldType,
                                        field->getPath(),
                                        encryptedIndex,
                                        boost::none,
                                        boost::none,
                                        boost::none);
                break;
            }
        }
    } else {
        if (field->getBsonType().has_value()) {
            BSONType type = typeFromName(field->getBsonType().value());

            uassert(6338406,
                    str::stream() << "Type '" << typeName(type)
                                  << "' is not a supported unindexed type",
                    isFLE2UnindexedSupportedType(type));
        }
    }
}

void validateEncryptedFieldConfig(const EncryptedFieldConfig* config) {
    stdx::unordered_set<UUID, UUID::Hash> keys(config->getFields().size());
    std::vector<FieldRef> fieldPaths;
    fieldPaths.reserve(config->getFields().size());

    if (config->getEscCollection()) {
        uassert(
            7406900,
            "Encrypted State Collection name should follow enxcol_.<collection>.esc naming pattern",
            NamespaceString::isFLE2StateCollection(config->getEscCollection().get()));
    }
    if (config->getEcocCollection()) {
        uassert(7406902,
                "Encrypted Compaction Collection name should follow enxcol_.<collection>.ecoc "
                "naming pattern",
                NamespaceString::isFLE2StateCollection(config->getEcocCollection().get()));
    }
    for (const auto& field : config->getFields()) {
        UUID keyId = field.getKeyId();

        // Duplicate key ids are bad, it breaks the design
        uassert(6338401, "Duplicate key ids are not allowed", keys.count(keyId) == 0);
        keys.insert(keyId);

        uassert(6316402, "Encrypted field must have a non-empty path", !field.getPath().empty());
        FieldRef newPath(field.getPath());
        uassert(6316403, "Cannot encrypt _id or its subfields", newPath.getPart(0) != "_id");

        for (const auto& path : fieldPaths) {
            uassert(6338402, "Duplicate paths are not allowed", newPath != path);
            // Cannot have indexes on "a" and "a.b"
            uassert(6338403,
                    str::stream() << "Conflicting index paths found as one is a prefix of another '"
                                  << newPath.dottedField() << "' and '" << path.dottedField()
                                  << "'",
                    !path.fullyOverlapsWith(newPath));
        }

        fieldPaths.push_back(std::move(newPath));
    }
}

bool validateDoublePrecisionRange(double d, uint32_t precision) {
    double maybe_integer = d * pow(10.0, precision);
    // We truncate here as that is what is specified in the FLE OST document.
    double trunc_integer = trunc(maybe_integer);

    // We want to prevent users from making mistakes by specifing extra precision in the bounds.
    // Since floating point is inaccurate, we need to account for this when testing for equality by
    // considering the values almost equal to likely mean the bounds are within the precision range.
    auto e = std::numeric_limits<double>::epsilon();
    return fabs(maybe_integer - trunc_integer) <= fabs(e * trunc_integer);
}

bool validateDecimal128PrecisionRange(Decimal128& dec, uint32_t precision) {
    Decimal128 maybe_integer = dec.scale(precision);
    Decimal128 trunc_integer = maybe_integer.round(Decimal128::kRoundTowardZero);

    return maybe_integer == trunc_integer;
}

void setRangeDefaults(BSONType fieldType, StringData fieldPath, QueryTypeConfig* queryp) {
    auto& query = *queryp;

    // Make sure the QueryTypeConfig is valid before setting defaults
    validateRangeIndex(fieldType, fieldPath, query);

    auto [defMin, defMax] = getRangeMinMaxDefaults(fieldType);
    query.setMin(query.getMin().value_or(defMin));
    query.setMax(query.getMax().value_or(defMax));
    query.setSparsity(query.getSparsity().value_or(kFLERangeSparsityDefault));
}

void validateRangeBounds(BSONType fieldType,
                         const boost::optional<Value>& min,
                         const boost::optional<Value>& max,
                         uint32_t sparsity,
                         uint32_t trimFactor,
                         const boost::optional<uint32_t>& precision) {
    switch (fieldType) {
        case BSONType::numberInt:
            return validateRangeBoundsInt32(
                min.map(valToInt), max.map(valToInt), sparsity, trimFactor);
        case BSONType::numberLong:
            return validateRangeBoundsInt64(
                min.map(valToLong), max.map(valToLong), sparsity, trimFactor);
        case BSONType::date:
            return validateRangeBoundsInt64(
                min.map(valToDateToLong), max.map(valToDateToLong), sparsity, trimFactor);
        case BSONType::numberDouble:
            return validateRangeBoundsDouble(
                min.map(valToDouble), max.map(valToDouble), sparsity, trimFactor, precision);
        case BSONType::numberDecimal:
            return validateRangeBoundsDecimal128(
                min.map(valToDecimal), max.map(valToDecimal), sparsity, trimFactor, precision);
        default:
            uasserted(9203507,
                      "Field type is invalid; must be one of int, long, date, double, or decimal");
    }
}

void validateRangeBoundsInt32(const boost::optional<int32_t>& min,
                              const boost::optional<int32_t>& max,
                              uint32_t sparsity,
                              uint32_t trimFactor) {
    validateRangeBoundsInt(getTypeInfo32(min.value_or(0), min, max), sparsity, trimFactor);
}

void validateRangeBoundsInt64(const boost::optional<int64_t>& min,
                              const boost::optional<int64_t>& max,
                              uint32_t sparsity,
                              uint32_t trimFactor) {
    validateRangeBoundsInt(getTypeInfo64(min.value_or(0), min, max), sparsity, trimFactor);
}

void validateRangeBoundsDouble(const boost::optional<double>& min,
                               const boost::optional<double>& max,
                               uint32_t sparsity,
                               uint32_t trimFactor,
                               const boost::optional<uint32_t>& precision) {
    validateRangeBoundsInt(
        getTypeInfoDouble(min.value_or(0), min, max, precision), sparsity, trimFactor);
}

void validateRangeBoundsDecimal128(const boost::optional<Decimal128>& min,
                                   const boost::optional<Decimal128>& max,
                                   uint32_t sparsity,
                                   uint32_t trimFactor,
                                   const boost::optional<uint32_t>& precision) {
    auto typeInfo =
        getTypeInfoDecimal128(min.value_or(Decimal128::kNormalizedZero), min, max, precision);

    if (typeInfo.max < kMaxTagLimit) {
        return;
    }

    double domainSizeLog2 = 128;

    if (typeInfo.max < std::numeric_limits<boost::multiprecision::uint128_t>::max()) {
        // +1 since we want the number of values between min and max, inclusive
        domainSizeLog2 = boost::multiprecision::msb(typeInfo.max - typeInfo.min + 1) + 1;
    }

    // Compute the number of bits we need
    // domainSizeLog2 is <= 128 at this point
    validateRangeBoundsBase(domainSizeLog2, sparsity, trimFactor);
}


}  // namespace mongo
