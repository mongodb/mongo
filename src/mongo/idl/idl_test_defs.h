// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
// IWYU pragma: no_include <ctype.h>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/idl/unittest_import_gen.h"
#include "mongo/util/ctype.h"  // IWYU pragma: keep

namespace mongo {
namespace idl {
namespace test {

class StructWithValidator;

/**
 * Validates the given number is even
 */
inline Status validateEvenNumber(std::int32_t value) {
    if (value & 1) {
        return {ErrorCodes::BadValue, "Value must be even"};
    }
    return Status::OK();
}

inline Status validateEvenNumber(const std::vector<std::int32_t>& values) {
    for (auto& value : values) {
        auto status = validateEvenNumber(value);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

/**
 * Validates that the number presented is within 0.1 of an integer value
 */
inline Status validateNearlyInt(double value) {
    value = fabs(value);
    value = value - static_cast<std::uint64_t>(value);
    if ((value > 0.1) && (value < 0.9)) {
        return {ErrorCodes::BadValue, "Value is too far from being an integer"};
    }
    return Status::OK();
}

/**
 * Validates that the provided string starts with a given letter.
 */
template <char letter>
Status validateStartsWith(const std::string& value) {
    if ((value.empty() || value[0] != letter)) {
        return {ErrorCodes::BadValue, "Value does not begin with correct letter"};
    }
    return Status::OK();
}


/**
 * Validate a struct
 */
inline Status validateOneInt(const mongo::idl::import::One_int& one) {
    return validateEvenNumber(one.getValue());
}

inline Status validateOneInt(const std::vector<mongo::idl::import::One_int>& values) {
    for (auto& value : values) {
        auto status = validateEvenNumber(value.getValue());
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

/**
 * Validate strings in a vector are only made up of capital letters.
 */
inline Status validateAllCapsArray(const std::vector<std::string>& array) {
    for (const auto& str : array) {
        if (!std::all_of(str.begin(), str.end(), [](char c) { return std::isupper(c); })) {
            return {ErrorCodes::BadValue, "A non-upper character exists in the string."};
        }
    }

    return Status::OK();
}


/**
 * Check that the two values in the struct are equal, assert otherwise.
 */
void checkValuesEqual(StructWithValidator* structToValidate);


}  // namespace test
}  // namespace idl
}  // namespace mongo
