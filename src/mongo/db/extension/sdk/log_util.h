/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/util/modules.h"

namespace mongo::extension::sdk {
inline void sdk_log(const std::string& message,
                    std::int32_t code,
                    MongoExtensionLogSeverity severity,
                    const std::vector<ExtensionLogAttribute>& attrs) {
    sdk::HostServicesHandle::getHostServices()->getLogger().log(message, code, severity, attrs);
}

inline void sdk_log(const std::string& message, std::int32_t code) {
    sdk_log(message, code, MongoExtensionLogSeverity::kInfo, {});
}

inline void sdk_log(const std::string& message,
                    std::int32_t code,
                    MongoExtensionLogSeverity severity) {
    sdk_log(message, code, severity, {});
}

inline void sdk_log(const std::string& message,
                    std::int32_t code,
                    const std::vector<ExtensionLogAttribute>& attrs) {
    sdk_log(message, code, MongoExtensionLogSeverity::kInfo, attrs);
}

inline void sdk_logDebug(const std::string& message,
                         std::int32_t code,
                         std::int32_t level,
                         const std::vector<ExtensionLogAttribute>& attrs) {
    sdk::HostServicesHandle::getHostServices()->getLogger().logDebug(message, code, level, attrs);
}

inline void sdk_logDebug(const std::string& message, std::int32_t code) {
    sdk_logDebug(message, code, 1, {});
}

inline void sdk_logDebug(const std::string& message, std::int32_t code, std::int32_t level) {
    sdk_logDebug(message, code, level, {});
}

inline void sdk_logDebug(const std::string& message,
                         std::int32_t code,
                         const std::vector<ExtensionLogAttribute>& attrs) {
    sdk_logDebug(message, code, 1, attrs);
}

}  // namespace mongo::extension::sdk
