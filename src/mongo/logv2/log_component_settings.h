// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <mutex>

namespace mongo::logv2 {

/**
 * Contains log severities for a list of log components.
 * kDefault always has a log severity defined but it is not necessary to
 * provide log severities for the other components (up to but not including kNumLogComponents).
 */
class [[MONGO_MOD_PUBLIC]] LogComponentSettings {
    LogComponentSettings(const LogComponentSettings&) = delete;
    LogComponentSettings& operator=(const LogComponentSettings&) = delete;

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
    void _setMinimumLoggedSeverityInLock(LogComponent component, LogSeverity severity);

    // A mutex to synchronize writes to the severity arrays. This mutex is to synchronize changes to
    // the entire array, and the atomics are to synchronize individual elements.
    std::mutex _mtx;

    // True if a log severity is explicitly set for a component.
    // This differentiates between unconfigured components and components that happen to have
    // the same severity as kDefault.
    // This is also used to update the severities of unconfigured components when the severity
    // for kDefault is modified.
    Atomic<bool> _hasMinimumLoggedSeverity[LogComponent::kNumLogComponents];

    // Log severities for components.
    // Store numerical values of severities to be cache-line friendly.
    // Set to kDefault minimum logged severity if _hasMinimumLoggedSeverity[i] is false.
    Atomic<int> _minimumLoggedSeverity[LogComponent::kNumLogComponents];
};

}  // namespace mongo::logv2
