// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/aligned.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * A single operation counter. Always maintains a local atomic for value() reads; optionally also
 * reports increments to an OTel instrument for export.
 */
class OpCounter {
public:
    OpCounter() = default;
    explicit OpCounter(otel::metrics::MetricName name);

    OpCounter(const OpCounter&) = delete;
    OpCounter& operator=(const OpCounter&) = delete;
    OpCounter(OpCounter&&) = delete;
    OpCounter& operator=(OpCounter&&) = delete;

    void add(int64_t v) {
        _value.fetchAndAddRelaxed(v);
        if (_otelCounter)
            _otelCounter->add(v);
    }

    int64_t value() const {
        return _value.loadRelaxed();
    }

private:
    Atomic<int64_t> _value{0};
    otel::metrics::Counter<int64_t>* const _otelCounter = nullptr;
};

/**
 * Tracks operation counters. Counter values are always owned by this struct; when metric names are
 * supplied at construction the main counters are also registered with MetricsService for OTel
 * export.
 *
 * The address of each OpCounters instance must remain stable for its lifetime (the struct is
 * neither copyable nor movable) because MetricsService stores pointers to the embedded counters.
 */
struct OpCounters {
    /**
     * Constructs an OpCounters whose main counters are registered with MetricsService for OTel
     * export under the given metric names.
     */
    OpCounters(otel::metrics::MetricName insertName,
               otel::metrics::MetricName queryName,
               otel::metrics::MetricName updateName,
               otel::metrics::MetricName deleteOpName,
               otel::metrics::MetricName getMoreName,
               otel::metrics::MetricName commandName,
               otel::metrics::MetricName aggregateName);

    /**
     * Constructs an OpCounters whose counters are local only — values are tracked but not exported
     * via OTel.
     */
    OpCounters();

    OpCounters(const OpCounters&) = delete;
    OpCounters& operator=(const OpCounters&) = delete;
    OpCounters(OpCounters&&) = delete;
    OpCounters& operator=(OpCounters&&) = delete;

    BSONObj getObj() const;

    void gotInserts(int n) {
        inserts->add(n);
    }
    void gotInsert() {
        inserts->add(1);
    }
    void gotQuery() {
        queries->add(1);
    }
    void gotUpdate() {
        updates->add(1);
    }
    void gotUpdates(int n) {
        updates->add(n);
    }
    void gotDelete() {
        deletes->add(1);
    }
    void gotDeletes(int n) {
        deletes->add(n);
    }
    void gotGetMore() {
        getMores->add(1);
    }
    void gotCommand() {
        commands->add(1);
    }
    void gotAggregate() {
        aggregates->add(1);
    }
    void gotQueryDeprecated() {
        queriesDeprecated->add(1);
    }
    void gotNestedAggregate() {
        nestedAggregates->add(1);
    }

    // These opcounters record operations that would fail if we were fully enforcing our
    // consistency constraints in steady-state oplog application mode.
    void gotInsertOnExistingDoc() {
        insertsOnExistingDoc->add(1);
    }
    void gotUpdateOnMissingDoc() {
        updatesOnMissingDoc->add(1);
    }
    void gotDeleteWasEmpty() {
        deletesWasEmpty->add(1);
    }
    void gotDeleteFromMissingNamespace() {
        deletesFromMissingNamespace->add(1);
    }
    void gotAcceptableErrorInCommand() {
        acceptableErrorsInCommand->add(1);
    }
    void gotRecordIdsReplicatedDocIdMismatch() {
        recordIdsReplicatedDocIdMismatches->add(1);
    }

    CacheExclusive<OpCounter> inserts;
    CacheExclusive<OpCounter> queries;
    CacheExclusive<OpCounter> updates;
    CacheExclusive<OpCounter> deletes;  // 'delete' is a keyword.
    CacheExclusive<OpCounter> getMores;
    CacheExclusive<OpCounter> commands;
    CacheExclusive<OpCounter> aggregates;
    CacheExclusive<OpCounter> nestedAggregates;
    CacheExclusive<OpCounter> insertsOnExistingDoc;
    CacheExclusive<OpCounter> updatesOnMissingDoc;
    CacheExclusive<OpCounter> deletesWasEmpty;
    CacheExclusive<OpCounter> deletesFromMissingNamespace;
    CacheExclusive<OpCounter> acceptableErrorsInCommand;
    CacheExclusive<OpCounter> recordIdsReplicatedDocIdMismatches;
    CacheExclusive<OpCounter> queriesDeprecated;
};

/**
 * Process-global op counters. Exposed via a function in case we need to change initialization or
 * anything later without impacting call sites.
 */
OpCounters& globalOpCounters();

/**
 * A separate process-global OpCounters instance for tracking replication related ops. Exposed via
 * a function in case we need to change initialization or anything later without impacting call
 * sites.
 */
OpCounters& replOpCounters();

}  // namespace mongo
