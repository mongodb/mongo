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

#pragma once

#include "mongo/platform/basic.h"

#include "mongo/db/server_parameter.h"
#include "mongo/idl/server_parameter_with_storage_test_structs_gen.h"

namespace mongo {
namespace test {

constexpr std::int32_t kStartupIntWithExpressionsMinimum = 10;
constexpr std::int32_t kStartupIntWithExpressionsMaximum = 1000;

// Storage for set parameter defined in server_parameter_with_storage.idl
extern AtomicWord<int> gStdIntPreallocated;

// Counter for how many times gStdIntPreallocated has been modified.
extern AtomicWord<int> gStdIntPreallocatedUpdateCount;

// Counter for how many times changeStreamOptions has been modified.
extern size_t count;

/**
 * Validates that the proposed new value is odd.
 */
inline Status validateOdd(const std::int32_t& value) {
    return (value & 1) ? Status::OK() : Status(ErrorCodes::BadValue, "Must be odd");
}

inline Status validateOddSP(const std::int32_t& value, const boost::optional<TenantId>&) {
    return validateOdd(value);
}

/**
 * Validates that the new expireAfterSeconds is non-negative.
 */
inline Status validateNonNegativeExpireAfterSeconds(const ChangeStreamOptionsClusterParam& newVal,
                                                    const boost::optional<TenantId>& tenantId) {
    if (newVal.getPreAndPostImages().getExpireAfterSeconds() < 0) {
        return Status(ErrorCodes::BadValue, "Should be non-negative value only");
    }
    return Status::OK();
}

/**
 * Bumps the count of gStdIntPreallocatedUpdateCount in response
 * to the successful update of gStdIntPreallocated.
 */
inline Status onUpdateStdIntPreallocated(const std::int32_t&) {
    gStdIntPreallocatedUpdateCount.fetchAndAdd(1);
    return Status::OK();
}

/**
 * Bumps count in response to the successful update of changeStreamOptions.
 */
inline Status onUpdateChangeStreamOptions(const ChangeStreamOptionsClusterParam&) {
    ++count;
    return Status::OK();
}

}  // namespace test
}  // namespace mongo
