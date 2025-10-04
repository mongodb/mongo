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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/stdx/unordered_map.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <pdh.h>
#include <pdhmsg.h>

namespace mongo {

class BSONObjBuilder;

/**
 * PerfCounterCollection contains a set of counters for PerfCounterCollector to collect. This class
 * supports adding counters with wildcards. It also optionally supports grouping counters by
 * instance name.
 */
class PerfCounterCollection {
    PerfCounterCollection(const PerfCounterCollection&) = delete;
    PerfCounterCollection& operator=(const PerfCounterCollection&) = delete;

    friend class PerfCounterCollector;

public:
    PerfCounterCollection() = default;
    PerfCounterCollection(PerfCounterCollection&&) = default;
    PerfCounterCollection& operator=(PerfCounterCollection&&) = default;

    /**
     * Add vector of counters grouped under 'name'.
     *
     * group name - the name of the BSON document to add these counters into.
     * paths - a vector of counter paths. These may contain wildcards.
     *
     * Errors if groupName duplicates an existing group or if paths has duplicate keys. Does not
     * validate if the counters exist.
     *
     * Output document:
     * For the following counters in "cpu":
     *   "\System\Processes"
     *   "\Processor(_Total)\% Idle Time"
     *
     * {
     *   "cpu" : {
     *       "\System\Processes" : 42,
     *       "\Processor\% Idle Time" : 12,
     *       "\Processor\% Idle Time Base" : 53,
     *   }
     * }
     */
    Status addCountersGroup(StringData groupName, const std::vector<StringData>& paths);

    /**
     * Add vector of counters grouped under 'name', and grouped by instance name.
     *
     * group name - the name of the BSON document to add these counters into.
     * paths - a vector of counter paths. These may contain wildcards. The '_Total' instance is
     *         automatically filtered since it can be computed by summing other instances.
     *
     * Errors if groupName duplicates an existing group or if paths has duplicate keys. Does not
     * validate if the counters exist.
     *
     * Output document:
     * For the following counters in "disks":
     *   "\PhysicalDisk(*)\% Disk Write Time"
     *
     * {
     *   "disks" : {
     *       "0 C:" : {
     *           "\PhysicalDisk\% Disk Write Time": 42,
     *           "\PhysicalDisk\% Disk Write Time Base": 32,
     *       },
     *       "1 D:" : {
     *           "\PhysicalDisk\% Disk Write Time": 43,
     *           "\PhysicalDisk\% Disk Write Time Base": 37,
     *       }
     *   }
     * }
     */
    Status addCountersGroupedByInstanceName(StringData groupName,
                                            const std::vector<StringData>& paths);

private:
    /**
     * Check for duplicate group and counters.
     */
    StatusWith<std::vector<std::string>> checkCounters(StringData groupName,
                                                       const std::vector<StringData>& paths);

private:
    // Vector of counters which are not sub-grouped by instance name.
    stdx::unordered_map<std::string, std::vector<std::string>> _counters;

    // Vector of counters sub grouped by instance name.
    stdx::unordered_map<std::string, std::vector<std::string>> _nestedCounters;
};

/**
 * PerfCounterCollector collects a series of counters from a Performance Data Helper (PDH) Query and
 * output the raw counter values to BSONObjBuilder.
 */
class PerfCounterCollector {
    PerfCounterCollector(const PerfCounterCollector&) = delete;
    PerfCounterCollector& operator=(const PerfCounterCollector&) = delete;

public:
    ~PerfCounterCollector();
    PerfCounterCollector(PerfCounterCollector&&) = default;

    /**
     * Create a PerfCounterCollector to collect the performance counters in the specified
     * PerfCounterCollection.
     */
    static StatusWith<std::unique_ptr<PerfCounterCollector>> create(PerfCounterCollection builder);

    /**
     * Collect the counters from PDH, and output their raw values into builder. The exception is
     * elapsed time counters which returns computed values instead of raw values.
     *
     * For each counters, if the counter is a precision counter (see PERF_COUNTER_PRECISION), the
     * second value is output under the name "<counter> Base". Also, a single field is output called
     * "timebase" if any counter depends on system ticks per second. See counterHasTickBasedTimeBase
     * for more details about timebase.
     */
    Status collect(BSONObjBuilder* builder);

private:
    /**
     * Describes a counter by querying PDH, and contains the necessary information to retrieve a
     * counter from PDH.
     */
    struct CounterInfo {
        /**
         * The name of the first value for a counter. This is output as:
         * "\<Object Name>\<Counter Name>".
         */
        std::string firstName;

        /**
         * The name of the second value of a counter if the counter is a precision counter. This is
         * output as: "\<Object Name>\<Counter Name> Base".
         */
        std::string secondName;

        /**
         * True if the counter is a precision counter, and its value should be output in the output
         * BSON document.
         */
        bool hasSecondValue;

        /**
         * Instance name of the counter. Empty if the counter has no instance name.
         */
        std::string instanceName;

        /**
         * Counter Type. See PERF_* constants in winperf.h.
         * https://technet.microsoft.com/en-us/library/cc785636(v=ws.10).aspx
         */
        uint32_t type;

        /**
         * Handle of counter to collect from.
         */
        PDH_HCOUNTER handle;
    };

    /**
     * A set of counters that are part of "name" in the final bson document.
     */
    struct CounterGroup {
        /**
         * Name of the counter group.
         */
        std::string name;

        /**
         * Vector of counters in this group.
         */
        std::vector<CounterInfo> counters;
    };

    /**
     * A set of counters that are part of "name" and "instanceName" in the final bson document.
     */
    struct NestedCounterGroup {
        /**
         * Name of the counter group.
         */
        std::string name;

        /**
         * A map of instance name to vector of counters to collect for each instance name.
         * Ordered Map to ensure output is well-ordered.
         */
        std::map<std::string, std::vector<CounterInfo>> counters;
    };

private:
    PerfCounterCollector() = default;

    /**
     * Open the PDH Query.
     */
    Status open();

    /**
     * Add the specified counter group to the PDH Query.
     */
    Status addCountersGroup(StringData groupName, const std::vector<std::string>& paths);

    /**
     * Add the specified counter group to the PDH Query grouped by instance name.
     */
    Status addCountersGroupedByInstanceName(StringData groupName,
                                            const std::vector<std::string>& paths);

    /**
     * Add a counter to the PDH query and get a description of it.
     */
    StatusWith<CounterInfo> addCounter(StringData path);

    /**
     * Add a set of counters to the PDH query, and get descriptions of them.
     */
    StatusWith<std::vector<CounterInfo>> addCounters(StringData path);

    /**
     * Collect a vector of counters and output them to builder.
     */
    Status collectCounters(const std::vector<CounterInfo>& counters, BSONObjBuilder* builder);

    /**
     * Check if any of the counters we want depends on system ticks per second as a time base.
     */
    void checkForTicksTimeBase();

    /**
     * Add and get a counter by an English name in a language independent way.
     */
    StatusWith<std::tuple<PDH_HCOUNTER, std::unique_ptr<PDH_COUNTER_INFO>>> addAndGetCounter(
        StringData path);

private:
    // PDH Query
    HQUERY _query{INVALID_HANDLE_VALUE};

    // Typically: CPU & Memory counters
    std::vector<CounterGroup> _counters;

    // Typically: Disks counters
    std::vector<NestedCounterGroup> _nestedCounters;

    // A counter that uses ticks as a timebase
    const CounterInfo* _timeBaseTicksCounter{nullptr};
};

}  // namespace mongo
