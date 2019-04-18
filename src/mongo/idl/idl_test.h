/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/idl/unittest_import_gen.h"

namespace mongo {
namespace idl {
namespace test {

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

}  // namespace test
}  // namespace idl
}  // namespace mongo
