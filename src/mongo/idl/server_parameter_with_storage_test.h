// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/server_parameter_with_storage_test_structs_gen.h"
#include "mongo/platform/atomic.h"

#include <cstddef>
#include <cstdint>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace test {

constexpr std::int32_t kStartupIntWithExpressionsMinimum = 10;
constexpr std::int32_t kStartupIntWithExpressionsMaximum = 1000;

// Storage for set parameter defined in server_parameter_with_storage.idl
extern Atomic<int> gStdIntPreallocated;

// Counter for how many times gStdIntPreallocated has been modified.
extern Atomic<int> gStdIntPreallocatedUpdateCount;

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
inline Status validateNonNegativeExpireAfterSeconds(const TestClusterParamStruct& newVal,
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
 * Bumps count in response to the successful update of testClusterServerParameter.
 */
inline Status onUpdateTestClusterServerParameter(const TestClusterParamStruct&) {
    ++count;
    return Status::OK();
}

}  // namespace test
}  // namespace mongo
