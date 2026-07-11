// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

// Checks various startup conditions and logs any necessary warnings that
// are specific to the mongod process.
void logMongodStartupWarnings(const StorageGlobalParams& storageParams,
                              const ServerGlobalParams& serverParams,
                              ServiceContext* svcCtx);

namespace startup_warning_detail {
#ifdef __linux__

enum class THPEnablementWarningLogCase {
    kWronglyEnabled,
    kWronglyDisabledViaOptOut,
    kWronglyDisabledOnSystem,
    kSystemValueError,
    kSystemValueErrorWithWrongOptOut,
    kSystemValueErrorWithOptOutError,
    kOptOutError,
    kNone
};

enum class THPDefragWarningLogCase { kWronglyNotUsingDeferMadvise, kError, kNone };

/**
 * Verify that the system max_ptes_none parameter is properly set.
 */
bool verifyMaxPtesNoneIsCorrect(bool usingGoogleTCMallocAllocator, unsigned value);

/**
 * Return the right THP enablement warning based on system conditions.
 */
THPEnablementWarningLogCase getTHPEnablementWarningCase(
    bool usingGoogleTCMallocAllocator,
    const StatusWith<std::string>& thpEnabled,
    const std::variant<std::error_code, bool>& optingOutOfTHPForProcess);

/**
 * Return the right defrag warning case based on system conditions.
 */
THPDefragWarningLogCase getDefragWarningCase(bool usingGoogleTCMallocAllocator,
                                             const StatusWith<std::string>& thpDefragSettings);

#endif  // __linux__
}  // namespace startup_warning_detail

}  // namespace mongo
