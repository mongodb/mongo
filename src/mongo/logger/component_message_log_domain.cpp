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

#include "mongo/platform/basic.h"

#include "mongo/logger/component_message_log_domain.h"

namespace mongo {
namespace logger {

ComponentMessageLogDomain::ComponentMessageLogDomain() {}

ComponentMessageLogDomain::~ComponentMessageLogDomain() {}

bool ComponentMessageLogDomain::hasMinimumLogSeverity(logv2::LogComponent component) const {
    return _settings.hasMinimumLogSeverity(component);
}

bool ComponentMessageLogDomain::shouldLog(logv2::LogComponent component,
                                          logv2::LogSeverity severity) const {
    return _settings.shouldLog(component, severity);
}

bool ComponentMessageLogDomain::shouldLog(logv2::LogComponent component1,
                                          logv2::LogComponent component2,
                                          logv2::LogSeverity severity) const {
    return _settings.shouldLog(component1, severity) || _settings.shouldLog(component2, severity);
}

bool ComponentMessageLogDomain::shouldLog(logv2::LogComponent component1,
                                          logv2::LogComponent component2,
                                          logv2::LogComponent component3,
                                          logv2::LogSeverity severity) const {
    return _settings.shouldLog(component1, severity) || _settings.shouldLog(component2, severity) ||
        _settings.shouldLog(component3, severity);
}

logv2::LogSeverity ComponentMessageLogDomain::getMinimumLogSeverity() const {
    return _settings.getMinimumLogSeverity(logv2::LogComponent::kDefault);
}

logv2::LogSeverity ComponentMessageLogDomain::getMinimumLogSeverity(
    logv2::LogComponent component) const {
    return _settings.getMinimumLogSeverity(component);
}

void ComponentMessageLogDomain::setMinimumLoggedSeverity(logv2::LogSeverity severity) {
    _settings.setMinimumLoggedSeverity(logv2::LogComponent::kDefault, severity);
}

void ComponentMessageLogDomain::setMinimumLoggedSeverity(logv2::LogComponent component,
                                                         logv2::LogSeverity severity) {
    _settings.setMinimumLoggedSeverity(component, severity);
}

void ComponentMessageLogDomain::clearMinimumLoggedSeverity(logv2::LogComponent component) {
    _settings.clearMinimumLoggedSeverity(component);
}

}  // namespace logger
}  // namespace mongo
