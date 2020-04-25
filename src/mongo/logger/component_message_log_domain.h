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

#include "mongo/logger/message_log_domain.h"
#include "mongo/logv2/log_component_settings.h"

namespace mongo {
namespace logger {

/**
 * Logging domain for ephemeral messages with minimum severity.
 */
class ComponentMessageLogDomain : public MessageLogDomain {
    ComponentMessageLogDomain(const ComponentMessageLogDomain&) = delete;
    ComponentMessageLogDomain& operator=(const ComponentMessageLogDomain&) = delete;

public:
    ComponentMessageLogDomain();

    ~ComponentMessageLogDomain();

    /**
     * Predicate that answers the question, "Should I, the caller, append to you, the log
     * domain, messages of the given severity?"  True means yes.
     */
    bool shouldLog(logv2::LogComponent component, logv2::LogSeverity severity) const;
    bool shouldLog(logv2::LogComponent component1,
                   logv2::LogComponent component2,
                   logv2::LogSeverity severity) const;
    bool shouldLog(logv2::LogComponent component1,
                   logv2::LogComponent component2,
                   logv2::LogComponent component3,
                   logv2::LogSeverity severity) const;

    /**
     * Returns true if a minimum log severity has been set for this component.
     * Called by log level commands to query component severity configuration.
     */
    bool hasMinimumLogSeverity(logv2::LogComponent component) const;

    /**
     * Gets the minimum severity of messages that should be sent to this LogDomain.
     */
    logv2::LogSeverity getMinimumLogSeverity() const;
    logv2::LogSeverity getMinimumLogSeverity(logv2::LogComponent component) const;

    /**
     * Sets the minimum severity of messages that should be sent to this LogDomain.
     */
    void setMinimumLoggedSeverity(logv2::LogSeverity severity);
    void setMinimumLoggedSeverity(logv2::LogComponent, logv2::LogSeverity severity);

    /**
     * Clears the minimum log severity for component.
     * For kDefault, severity level is initialized to default value.
     */
    void clearMinimumLoggedSeverity(logv2::LogComponent component);

private:
    logv2::LogComponentSettings _settings;
    AtomicWord<bool> _shouldRedact{false};
};

}  // namespace logger
}  // namespace mongo
