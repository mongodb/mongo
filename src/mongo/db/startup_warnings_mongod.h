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

#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"

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
