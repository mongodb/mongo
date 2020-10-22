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

#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/base/string_data.h"
#include "mongo/base/string_data_comparator_interface.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {

/**
 * LatchAnalyzer is a ServiceContext decoration that aggregates latch events
 *
 * This class is intended to provide a platform for hierarchical analysis on latches. To that end,
 * onContention(), onAcquire(), and onRelease() are currently called by a
 * latch_detail::DiagnosticListener subclass defined in source. This class does much more work for
 * each event when the enableLatchAnalysis failpoint is set to "alwaysOn". This failpoint provides a
 * wealth of data for future analysis, but involves additional mutexes and mapping structures that
 * may prove too costly for production usage at the least.
 */
class LatchAnalyzer {
public:
    static LatchAnalyzer& get(ServiceContext* serviceContext);
    static LatchAnalyzer& get(Client* client);
    static LatchAnalyzer& get();

    // Handler function for a failed latch acquire
    void onContention(const latch_detail::Identity& id);

    // Handler function for a successful latch acquire
    void onAcquire(const latch_detail::Identity& id);

    // Handler function for a latch release
    void onRelease(const latch_detail::Identity& id);

    // Append the current statistics in a form appropriate for server status to a BOB
    void appendToBSON(mongo::BSONObjBuilder& result) const;

    // Log the current statistics in JSON form to INFO
    void dump();

    void setAllowExitOnViolation(bool allowExitOnViolation);
    bool allowExitOnViolation();

private:
    struct HierarchyStats {
        const latch_detail::Identity* identity = nullptr;

        int acquiredAfter = 0;
        int releasedBefore = 0;
    };

    using SingleLatchHierarchy = stdx::unordered_map<int64_t, HierarchyStats>;

    struct HierarchicalAcquisitionLevelViolation {
        int onAcquire = 0;
        int onRelease = 0;
    };

    // Either warn about the violation or crash the process.
    void _handleViolation(ErrorCodes::Error ec,
                          StringData message,
                          const latch_detail::Identity& identity,
                          Client* client) noexcept;
    void _handleAcquireViolation(ErrorCodes::Error ec,
                                 StringData message,
                                 const latch_detail::Identity& identity,
                                 Client* client) noexcept;
    void _handleReleaseViolation(ErrorCodes::Error ec,
                                 StringData message,
                                 const latch_detail::Identity& identity,
                                 Client* client) noexcept;

    AtomicWord<bool> _allowExitOnViolation{true};

    mutable stdx::mutex _mutex;  // NOLINT
    stdx::unordered_map<int64_t, SingleLatchHierarchy> _hierarchies;
    stdx::unordered_map<int64_t, HierarchicalAcquisitionLevelViolation> _violations;
};

class LatchAnalyzerDisabledBlock {

public:
    LatchAnalyzerDisabledBlock();
    ~LatchAnalyzerDisabledBlock();
};

}  // namespace mongo
