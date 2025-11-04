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

#include "mongo/db/extension/sdk/host_services.h"

#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"

namespace mongo::extension::sdk {

// The static instance of HostServicesHandle is initially set to nullptr. It will be set "for real"
// at the very start of extension initialization, before any extension should attempt to access it.
HostServicesHandle HostServicesHandle::_hostServices(nullptr);

::MongoExtensionLogMessage HostServicesHandle::createLogMessageStruct(
    const std::string& message, std::int32_t code, MongoExtensionLogSeverity severity) {
    // Convert message string to byte view.
    auto messageBytes = stringViewAsByteView(std::string_view(message));

    // TODO SERVER-111339 Handle attributes.
    ::MongoExtensionLogMessage logMessage{
        static_cast<uint32_t>(code), messageBytes, ::MongoExtensionLogType::kLog};
    // Set union field for severity.
    logMessage.severityOrLevel.severity = severity;

    return logMessage;
}

::MongoExtensionLogMessage HostServicesHandle::createDebugLogMessageStruct(
    const std::string& message, std::int32_t code, std::int32_t level) {
    // Convert message string to byte view.
    auto messageBytes = stringViewAsByteView(std::string_view(message));

    // TODO SERVER-111339 Handle attributes.
    ::MongoExtensionLogMessage logMessage{
        static_cast<uint32_t>(code), messageBytes, ::MongoExtensionLogType::kDebug};
    // Set union field for level.
    logMessage.severityOrLevel.level = level;

    return logMessage;
}

void HostServicesHandle::_assertVTableConstraints(const VTable_t& vtable) const {
    sdk_tassert(
        11097801, "Host services' 'user_asserted' is null", vtable.user_asserted != nullptr);
    sdk_tassert(11188200, "Host services' 'log' is null", vtable.log != nullptr);
    sdk_tassert(11188201, "Host services' 'log_debug' is null", vtable.log_debug != nullptr);
    // Note that we intentionally do not validate tripwire_asserted here. If it wasn't valid, the
    // tripwire assert would fire and we would dereference the nullptr anyway.
    sdk_tassert(11149304,
                "Host services' 'create_host_agg_stage_parse_node' is null",
                vtable.create_host_agg_stage_parse_node != nullptr);
}

}  // namespace mongo::extension::sdk
