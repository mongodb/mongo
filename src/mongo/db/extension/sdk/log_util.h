// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/util/modules.h"

namespace mongo::extension::sdk {
inline void sdk_log(const std::string& message,
                    std::int32_t code,
                    MongoExtensionLogSeverity severity,
                    const std::vector<ExtensionLogAttribute>& attrs) {
    sdk::HostServicesAPI::getInstance()->getLogger()->log(message, code, severity, attrs);
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
    sdk::HostServicesAPI::getInstance()->getLogger()->logDebug(message, code, level, attrs);
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
