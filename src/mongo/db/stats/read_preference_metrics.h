/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/client/read_preference.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/read_preference_metrics_gen.h"

namespace mongo {

/**
 * Contains server-wide metrics on read preference usage. Metrics are first split into two
 * sub-documents that specify if the operation containing the read preference executed while the
 * node was a primary or secondary. For each sub-document, metrics are collected for each read
 * preference mode and if a tag set list was used. For each metric, operations are
 * further split into internal and external operations.
 */

class ReadPreferenceMetrics {
    ReadPreferenceMetrics(const ReadPreferenceMetrics&) = delete;
    ReadPreferenceMetrics& operator=(const ReadPreferenceMetrics&) = delete;

public:
    ReadPreferenceMetrics() = default;

    static ReadPreferenceMetrics* get(ServiceContext* service);
    static ReadPreferenceMetrics* get(OperationContext* opCtx);

    void recordReadPreference(ReadPreferenceSetting readPref, bool isInternal, bool isPrimary);

    // Generates the overall read preference metrics document, with two sub-documents for operations
    // that executed while in primary state and secondary state. This function and functions called
    // within it construct the metrics IDL classes, fill their values, and serialize them to BSON
    // for serverStatus.
    void generateMetricsDoc(ReadPreferenceMetricsDoc* stats);

private:
    struct Counter {
        AtomicWord<unsigned long long> internal{0};
        AtomicWord<unsigned long long> external{0};

        // Loads an individual counter's internal and external operation counters into a
        // 'ReadPrefOps' object.
        ReadPrefOps toReadPrefOps() const;
        // Increments one of the two counters, based on 'isInternal'.
        void increment(bool isInternal);
    };
    struct Counters {
        Counter primary;
        Counter primaryPreferred;
        Counter secondary;
        Counter secondaryPreferred;
        Counter nearest;
        Counter tagged;

        // Flushes all the individual read preference metrics while in a particular replica set
        // state, which is part of generating the overall read preference metrics document.
        void flushCounters(ReadPrefDoc* doc);
        // Increments the correct counter based on the passed in 'readPref' and 'isInternal'
        // arguments.
        void increment(ReadPreferenceSetting readPref, bool isInternal);
    };

    Counters primaryCounters;
    Counters secondaryCounters;
};

}  // namespace mongo
