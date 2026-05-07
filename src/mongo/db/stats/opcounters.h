/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/util/modules.h"

#include <memory>

MONGO_MOD_PUBLIC;

namespace mongo {

/**
 * Abstraction over a single operation counter. Provides a simple add/value interface that hides
 * whether the counter is backed by an OTel instrument or a plain atomic.
 */
class OpCounter {
public:
    virtual ~OpCounter() = default;
    virtual void add(int64_t value) = 0;
    virtual int64_t value() const = 0;
};

/**
 * Creates a local (non-OTel-exported) OpCounter backed by an atomic int64.
 */
std::unique_ptr<OpCounter> makeLocalOpCounter();

/**
 * Creates an OTel-exported OpCounter registered with MetricsService under the given metric name.
 */
std::unique_ptr<OpCounter> makeOtelOpCounter(otel::metrics::MetricName metricName);

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

    std::unique_ptr<OpCounter> inserts;
    std::unique_ptr<OpCounter> queries;
    std::unique_ptr<OpCounter> updates;
    std::unique_ptr<OpCounter> deletes;  // 'delete' is a keyword.
    std::unique_ptr<OpCounter> getMores;
    std::unique_ptr<OpCounter> commands;
    std::unique_ptr<OpCounter> aggregates;
    std::unique_ptr<OpCounter> nestedAggregates;
    std::unique_ptr<OpCounter> insertsOnExistingDoc;
    std::unique_ptr<OpCounter> updatesOnMissingDoc;
    std::unique_ptr<OpCounter> deletesWasEmpty;
    std::unique_ptr<OpCounter> deletesFromMissingNamespace;
    std::unique_ptr<OpCounter> acceptableErrorsInCommand;
    std::unique_ptr<OpCounter> recordIdsReplicatedDocIdMismatches;
    std::unique_ptr<OpCounter> queriesDeprecated;
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
