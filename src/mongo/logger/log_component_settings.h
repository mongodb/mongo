/*    Copyright 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/logger/log_component.h"
#include "mongo/logger/log_severity.h"

namespace mongo {
namespace logger {

/**
 * Contains log severities for a list of log components.
 * kDefault always has a log severity defined but it is not necessary to
 * provide log severities for the other components (up to but not including kNumLogComponents).
 */
class LogComponentSettings {
    MONGO_DISALLOW_COPYING(LogComponentSettings);

public:
    LogComponentSettings();
    ~LogComponentSettings();

    /**
     * Returns true if a minimum log severity has been set for this component.
     * Used by log level commands to query component severity configuration.
     */
    bool hasMinimumLogSeverity(LogComponent component) const;

    /**
     * Gets the minimum log severity for component.
     * Result is defined only if hasMinimumLogSeverity() returns true for component.
     */
    LogSeverity getMinimumLogSeverity(LogComponent component) const;

    /**
     * Sets the minimum log severity for component.
     */
    void setMinimumLoggedSeverity(LogComponent component, LogSeverity severity);

    /**
     * Clears the minimum log severity for component.
     * For kDefault, severity level is initialized to default value.
     */
    void clearMinimumLoggedSeverity(LogComponent component);

    /**
     * Predicate that answers the question, "Should I, the caller, append to you, the log
     * domain, componented messages of the given severity?"  True means yes.
     *
     * If minimum severity levels are not configured, compare 'severity' against the configured
     * level for kDefault.
     */
    bool shouldLog(LogComponent component, LogSeverity severity) const;

private:
    // True if a log severity is explicitly set for a component.
    // This differentiates between unconfigured components and components that happen to have
    // the same severity as kDefault.
    // This is also used to update the severities of unconfigured components when the severity
    // for kDefault is modified.
    bool _hasMinimumLoggedSeverity[LogComponent::kNumLogComponents];

    // Log severities for components.
    // Store numerical values of severities to be cache-line friendly.
    // Set to kDefault minimum logged severity if _hasMinimumLoggedSeverity[i] is false.
    signed char _minimumLoggedSeverity[LogComponent::kNumLogComponents];
};

}  // namespace logger
}  // namespace mongo
