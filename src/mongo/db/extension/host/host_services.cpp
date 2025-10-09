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
#include "mongo/db/extension/host/host_services.h"

#include "mongo/db/extension/public/extension_log_gen.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/logv2/log_options.h"
#include "mongo/util/assert_util.h"

#include <string>

namespace mongo::extension::host {

bool HostServices::alwaysTrue_TEMPORARY() {
    return true;
}

void HostServices::log(const mongo::extension::MongoExtensionLog& log) {
    // For now we always log extension messages under the EXTENSION-MONGOT component. Someday we'd
    // like to dynamically create EXTENSION sub-components per extension.
    logv2::LogOptions options(logv2::LogComponent::kExtensionMongot);

    logv2::LogSeverity severity = [&]() {
        switch (log.getSeverity()) {
            case mongo::extension::MongoExtensionLogSeverityEnum::kWarning:
                return logv2::LogSeverity::Warning();
            case mongo::extension::MongoExtensionLogSeverityEnum::kError:
                return logv2::LogSeverity::Error();
            default:
                return logv2::LogSeverity::Info();
        }
    }();

    // TODO SERVER-111339 Populate attributes.
    logv2::TypeErasedAttributeStorage attrs;

    // We must go through logv2::detail::doLogImpl since the LOGV2 macros expect a static string
    // literal for the message, but we have to log the message received at runtime from the
    // extension.
    logv2::detail::doLogImpl(log.getCode(), severity, options, log.getMessage(), attrs);
}

void HostServices::logDebug(const mongo::extension::MongoExtensionDebugLog& debugLog) {
    // For now we always log extension messages under the EXTENSION-MONGOT component. Someday we'd
    // like to dynamically create EXTENSION sub-components per extension.
    logv2::LogOptions options(logv2::LogComponent::kExtensionMongot);

    // We're trimming the debug levels to the range [1, 5] since we want to make sure that the log
    // line is using one of the server's logv2 debug severities.
    logv2::LogSeverity level =
        logv2::LogSeverity::Debug(std::min(5, std::max(1, debugLog.getLevel())));

    std::int32_t code = debugLog.getCode();
    StringData message = debugLog.getMessage();

    // TODO SERVER-111339 Populate attributes.
    logv2::TypeErasedAttributeStorage attrs;

    logv2::detail::doLogImpl(code, level, options, message, attrs);
}

}  // namespace mongo::extension::host
