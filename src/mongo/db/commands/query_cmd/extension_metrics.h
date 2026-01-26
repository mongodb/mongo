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

#include "mongo/base/counter.h"
#include "mongo/base/string_data.h"
#include "mongo/db/commands/query_cmd/cmd_specific_metric_helpers.h"
#include "mongo/db/topology/cluster_role.h"

namespace mongo {

/**
 * A housing container, meant to be instantiated once per command, so we can have each invocation
 * point to the same Counter64 metrics in serverStatus.
 */
struct ExtensionMetricsAllocation {
    ExtensionMetricsAllocation(StringData commandName, ClusterRole role)
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
