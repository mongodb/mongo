/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/logger/log_component.h"
#include "mongo/logger/log_severity_limiter.h"
#include "mongo/logger/log_version_util.h"
#include "mongo/logger/logger.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_manager.h"

namespace mongo {

bool logV2Enabled();
void logV2Set(bool setting);

/**
 * Runs the same logic as log()/warning()/error(), without actually outputting a stream.
 */

inline bool shouldLog(logger::LogComponent logComponent1, logger::LogSeverity severity) {
    if (logV2Enabled())
        return logv2::LogManager::global().getGlobalSettings().shouldLog(
            logComponentV1toV2(logComponent1), logSeverityV1toV2(severity));
    return logger::globalLogDomain()->shouldLog(logComponent1, severity);
}

inline logger::LogSeverity getMinimumLogSeverity() {
    if (logV2Enabled())
        return logSeverityV2toV1(
            logv2::LogManager::global().getGlobalSettings().getMinimumLogSeverity(
                mongo::logv2::LogComponent::kDefault));
    return logger::globalLogDomain()->getMinimumLogSeverity();
}

inline logger::LogSeverity getMinimumLogSeverity(logger::LogComponent component) {
    if (logV2Enabled())
        return logSeverityV2toV1(
            logv2::LogManager::global().getGlobalSettings().getMinimumLogSeverity(
                logComponentV1toV2(component)));
    return logger::globalLogDomain()->getMinimumLogSeverity(component);
}

inline void setMinimumLoggedSeverity(logger::LogSeverity severity) {
    if (logV2Enabled())
        return logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            mongo::logv2::LogComponent::kDefault, mongo::logSeverityV1toV2(severity));
    logger::globalLogDomain()->setMinimumLoggedSeverity(severity);
}

inline void setMinimumLoggedSeverity(logger::LogComponent component, logger::LogSeverity severity) {
    if (logV2Enabled())
        return logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            logComponentV1toV2(component), mongo::logSeverityV1toV2(severity));
    logger::globalLogDomain()->setMinimumLoggedSeverity(component, severity);
}

inline void clearMinimumLoggedSeverity(logger::LogComponent component) {
    if (logV2Enabled())
        return logv2::LogManager::global().getGlobalSettings().clearMinimumLoggedSeverity(
            logComponentV1toV2(component));
    logger::globalLogDomain()->clearMinimumLoggedSeverity(component);
}

inline bool hasMinimumLogSeverity(logger::LogComponent component) {
    if (logV2Enabled())
        return logv2::LogManager::global().getGlobalSettings().hasMinimumLogSeverity(
            logComponentV1toV2(component));
    return logger::globalLogDomain()->hasMinimumLogSeverity(component);
}

}  // namespace mongo
