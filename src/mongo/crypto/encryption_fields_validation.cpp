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

#include <fmt/format.h>

#include <cmath>
#include <limits>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/container/small_vector.hpp>
#include <boost/cstdint.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include <boost/move/utility_core.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/fle_numeric.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

namespace mongo {

Value coerceValueToRangeIndexTypes(Value val, BSONType fieldType) {
    BSONType valType = val.getType();

    if (valType == fieldType)
        return val;

    if (valType == Date || fieldType == Date) {
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
    if (valType == NumberDecimal || valType == NumberDouble || fieldType == NumberDecimal ||
        fieldType == NumberDouble) {
        uasserted(
            6742002,
            str::stream() << "If the value type and the field type are not the same type and one "
                             "or both of them is a double or a decimal, coercion of the value to "
                             "field type is not supported, due to possible loss of precision.");
    }

    switch (fieldType) {
        case NumberInt:
            return Value(val.coerceToInt());
        case NumberLong:
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
}  // namespace

uint32_t getNumberOfBitsInDomain(BSONType fieldType,
                                 const boost::optional<BSONElement>& min,
                                 const boost::optional<BSONElement>& max,
                                 const boost::optional<uint32_t>& precision) {
    uassert(8574112,
            "Precision may only be set when type is double or decimal",
            !precision || fieldType == NumberDouble || fieldType == NumberDecimal);
    switch (fieldType) {
        case NumberInt:
            return getNumberOfBitsInDomain(min.map(bsonToInt), max.map(bsonToInt));
        case NumberLong:
            return getNumberOfBitsInDomain(min.map(bsonToLong), max.map(bsonToLong));
        case Date:
            return getNumberOfBitsInDomain(min.map(bsonToDateToLong), max.map(bsonToDateToLong));
        case NumberDouble:
            return getNumberOfBitsInDomain(min.map(bsonToDouble), max.map(bsonToDouble), precision);
        case NumberDecimal:
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
            !precision || fieldType == NumberDouble || fieldType == NumberDecimal);
    switch (fieldType) {
        case NumberInt:
            return getNumberOfBitsInDomain(min.map(valToInt), max.map(valToInt));
        case NumberLong:
            return getNumberOfBitsInDomain(min.map(valToLong), max.map(valToLong));
        case Date:
            return getNumberOfBitsInDomain(min.map(valToDateToLong), max.map(valToDateToLong));
        case NumberDouble:
            return getNumberOfBitsInDomain(min.map(valToDouble), max.map(valToDouble), precision);
        case NumberDecimal:
            return getNumberOfBitsInDomain(min.map(valToDecimal), max.map(valToDecimal), precision);
        default:
            uasserted(8574114,
                      "Field type is invalid; must be one of int, long, date, double, or decimal");
    }
}

void validateRangeIndex(BSONType fieldType, QueryTypeConfig& query) {
    uassert(6775201,
            str::stream() << "Type '" << typeName(fieldType)
                          << "' is not a supported range indexed type",
            isFLE2RangeIndexedSupportedType(fieldType));

    uassert(6775202,
            "The field 'sparsity' is missing but required for range index",
            query.getSparsity().has_value());
    uassert(6775214,
            "The field 'sparsity' must be between 1 and 4",
            query.getSparsity().value() >= 1 && query.getSparsity().value() <= 4);


    switch (fieldType) {
        case NumberDouble:
        case NumberDecimal: {
            if (!((query.getMin().has_value() == query.getMax().has_value()) &&
                  (query.getMin().has_value() == query.getPrecision().has_value()))) {
                uasserted(6967100,
                          str::stream() << "Precision, min, and max must all be specified "
                                        << "together for floating point fields");
            }

            if (!query.getMin().has_value()) {
                if (fieldType == NumberDouble) {
                    query.setMin(mongo::Value(std::numeric_limits<double>::lowest()));
                    query.setMax(mongo::Value(std::numeric_limits<double>::max()));
                } else {
                    query.setMin(mongo::Value(Decimal128::kLargestNegative));
                    query.setMax(mongo::Value(Decimal128::kLargestPositive));
                }
            }

            if (query.getPrecision().has_value()) {
                uint32_t precision = query.getPrecision().value();
                if (fieldType == NumberDouble) {
                    uassert(
                        6966805,
                        "The number of decimal digits for minimum value must be less than or equal "
                        "to precision",
                        validateDoublePrecisionRange(query.getMin()->coerceToDouble(), precision));
                    uassert(
                        6966806,
                        "The number of decimal digits for maximum value must be less than or equal "
                        "to precision",
                        validateDoublePrecisionRange(query.getMax()->coerceToDouble(), precision));

                } else {
                    auto minDecimal = query.getMin()->coerceToDecimal();
                    uassert(6966807,
                            "The number of decimal digits for minimum value must be less than or "
                            "equal to precision",
                            validateDecimal128PrecisionRange(minDecimal, precision));
                    auto maxDecimal = query.getMax()->coerceToDecimal();
                    uassert(6966808,
                            "The number of decimal digits for maximum value must be less than or "
                            "equal to precision",
                            validateDecimal128PrecisionRange(maxDecimal, precision));
                }
            }
        }
            // We want to perform the same validation after sanitizing floating
            // point parameters, so we call FMT_FALLTHROUGH here.

            FMT_FALLTHROUGH;
        case NumberInt:
        case NumberLong:
        case Date: {
            uassert(6775203,
                    "The field 'min' is missing but required for range index",
                    query.getMin().has_value());
            uassert(6775204,
                    "The field 'max' is missing but required for range index",
                    query.getMax().has_value());

            auto indexMin = query.getMin().value();
            auto indexMax = query.getMax().value();

            uassert(7018200,
                    "Min should have the same type as the field.",
                    fieldType == indexMin.getType());
            uassert(7018201,
                    "Max should have the same type as the field.",
                    fieldType == indexMax.getType());

            uassert(6720005,
                    "Min must be less than max.",
                    Value::compare(indexMin, indexMax, nullptr) < 0);
        }

        break;
        default:
            uasserted(7018202, "Range index only supports numeric types and the Date type.");
    }

    if (gFeatureFlagQERangeV2.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
        query.getTrimFactor().has_value()) {
        uint32_t tf = query.getTrimFactor().value();
        uint32_t bits = getNumberOfBitsInDomain(
            fieldType,
            query.getMin().value(),
            query.getMax().value(),
            query.getPrecision().map([](int32_t i) { return (uint32_t)(i); }));
        // We allow the case where #bits = TF = 0.
        uassert(8574000,
                fmt::format("The field 'trimFactor' must be >= 0 and less than the total "
                            "number of bits needed to represent elements in the domain ({})",
                            bits),
                tf == 0 || tf < bits);
    }
}

void validateEncryptedField(const EncryptedField* field) {
    if (field->getQueries().has_value()) {
        auto encryptedIndex =
            visit(OverloadedVisitor{
                      [](QueryTypeConfig config) { return config; },
                      [](std::vector<QueryTypeConfig> configs) {
                          // TODO SERVER-67421 - remove restriction that only one query type
                          // can be specified per field
                          uassert(6338404,
                                  "Exactly one query type should be specified per field",
                                  configs.size() == 1);
                          return configs[0];
                      },
                  },
                  field->getQueries().value());

        uassert(6412601,
                "Bson type needs to be specified for an indexed field",
                field->getBsonType().has_value());
        auto fieldType = typeFromName(field->getBsonType().value());

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
                break;
            case QueryTypeEnum::RangePreviewDeprecated:
                // rangePreview is renamed to range in Range V2, but we still need to accept it as
                // valid so that we can start up with existing rangePreview collections.
            case QueryTypeEnum::Range: {
                validateRangeIndex(fieldType, encryptedIndex);
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

}  // namespace mongo
