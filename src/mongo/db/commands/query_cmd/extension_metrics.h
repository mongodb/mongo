// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/counter.h"
#include "mongo/db/commands/query_cmd/cmd_specific_metric_helpers.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

/**
 * A housing container, meant to be instantiated once per command, so we can have each invocation
 * point to the same Counter64 metrics in serverStatus.
 */
struct ExtensionMetricsAllocation {
    ExtensionMetricsAllocation(std::string_view commandName, ClusterRole role)
        : successMetricCounter(
              getSingletonMetricPtrWithinCmd(commandName, "withExtension.succeeded", role)),
          failedMetricCounter(
              getSingletonMetricPtrWithinCmd(commandName, "withExtension.failed", role)) {}

    // A counter for how many times this command has been successful *and* used an extension.
    Counter64* successMetricCounter;

    // A counter for how many times this command has failed *and* used an extension.
    Counter64* failedMetricCounter;
};

/**
 * Tool for tracking the success/failure metrics for commands involving extensions.
 * At the time of this writing, the only command which might involve extensions is aggregate, but
 * this metrics objects builds a section designed to be put within serverStatus.commands.*
 */
class ExtensionMetrics {
public:
    /**
     * Construct metrics for a command identified by commandName. If you construct this and do not
     * call 'markSuccess()' before this object is destroyed, the command is considered a failure.
     */
    ExtensionMetrics(const ExtensionMetricsAllocation& allocation) : _allocation(allocation) {}

    /**
     * Upon destruction increments the appropriate success or failure metric. If 'markSuccess' has
     * not been called, then tracks it as a failure.
     */
    ~ExtensionMetrics() {
        if (!_usedExtensions) {
            return;
        }
        if (_succeeded) {
            _allocation.successMetricCounter->increment();
        } else {
            _allocation.failedMetricCounter->increment();
        }
    }

    /**
     * Indicates this command succeeded. If this method is not called before destruction, it is
     * considered a failure.
     */
    void markSuccess() {
        _succeeded = true;
    }

    void trackThisRequestAsHavingUsedExtensions() {
        _usedExtensions = true;
    }

private:
    const ExtensionMetricsAllocation& _allocation;
    bool _usedExtensions = false;
    bool _succeeded = false;
};
}  // namespace mongo
