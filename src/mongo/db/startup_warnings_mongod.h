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

#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"

namespace mongo {

#ifdef __linux__

constexpr inline auto kTHPEnabledParameter = "enabled";

class StartupWarningsMongodLinux {
private:
    StartupWarningsMongodLinux();

public:
    enum class THPEnablementWarningLogCase {
        kWronglyEnabled,
        kSystemValueError,
        kSystemValueErrorWithOptOutError,
        kOptOutError,
        kNone
    };

    /**
     * Reads Transparent HugePages kernel parameter in sysfs directory.
     * Linux only.
     */
    static StatusWith<std::string> readTransparentHugePagesParameter(const std::string& parameter);

    /**
     * For testing only.
     * Supports alternate directory for transparent huge pages files.
     */
    static StatusWith<std::string> readTransparentHugePagesParameter(const std::string& parameter,
                                                                     const std::string& directory);

    /**
     * Return the right THP enablement warning based on system conditions.
     */
    static THPEnablementWarningLogCase getTHPEnablementWarningCase(
        const StatusWith<std::string>& thpEnabled,
        const std::variant<std::error_code, bool>& optingOutOfTHPForProcess);

    /**
     * Emit the correct log message corresponding to the inputted warning case.
     */
    static void warnForTHPEnablementCases(
        THPEnablementWarningLogCase warningCase,
        const StatusWith<std::string>& thpEnabled,
        const std::variant<std::error_code, bool>& optingOutOfTHPForProcess);

    /**
     * Take the values of THP on the system and process, verify their correctness in
     * isolation and combination, and emit a warning to the logs.
     */
    static void verifyCorrectTHPSettings(
        const StatusWith<std::string>& thpEnabled,
        const std::variant<std::error_code, bool>& optingOutOfTHPForProcess);

    /**
     * Log startup warnings that only apply to Linux.
     */
    static void logLinuxMongodWarnings(const StorageGlobalParams& storageParams,
                                       const ServerGlobalParams& serverParams,
                                       ServiceContext* svcCtx);
};

#endif  // __linux__

// Checks various startup conditions and logs any necessary warnings that
// are specific to the mongod process.
void logMongodStartupWarnings(const StorageGlobalParams& storageParams,
                              const ServerGlobalParams& serverParams,
                              ServiceContext* svcCtx);

}  // namespace mongo
