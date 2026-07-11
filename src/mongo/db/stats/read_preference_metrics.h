// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/read_preference.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/read_preference_metrics_gen.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
        Atomic<unsigned long long> internal{0};
        Atomic<unsigned long long> external{0};

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
