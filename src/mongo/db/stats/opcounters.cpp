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

#include "mongo/db/stats/opcounters.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/static_immortal.h"

namespace mongo {

namespace {

/**
 * OpCounter backed by a plain atomic — values are tracked locally but not exported via OTel.
 */
class LocalOpCounter final : public OpCounter {
public:
    void add(int64_t value) override {
        _counter.fetchAndAdd(value);
    }
    int64_t value() const override {
        return _counter.loadRelaxed();
    }

private:
    Atomic<int64_t> _counter{0};
};

/**
 * OpCounter backed by an OTel counter registered with MetricsService.
 */
class OtelOpCounter final : public OpCounter {
public:
    explicit OtelOpCounter(otel::metrics::MetricName metricName)
        : _counter(otel::metrics::MetricsService::instance().createInt64Counter(
              metricName, "Number of operations", otel::metrics::MetricUnit::kOperations)) {}

    void add(int64_t value) override {
        _counter.add(value);
    }

    int64_t value() const override {
        return _counter.valueForLegacyUse();
    }

private:
    otel::metrics::Counter<int64_t>& _counter;
};

}  // namespace

std::unique_ptr<OpCounter> makeLocalOpCounter() {
    return std::make_unique<LocalOpCounter>();
}

std::unique_ptr<OpCounter> makeOtelOpCounter(otel::metrics::MetricName metricName) {
    return std::make_unique<OtelOpCounter>(metricName);
}

OpCounters::OpCounters()
    : inserts(makeLocalOpCounter()),
      queries(makeLocalOpCounter()),
      updates(makeLocalOpCounter()),
      deletes(makeLocalOpCounter()),
      getMores(makeLocalOpCounter()),
      commands(makeLocalOpCounter()),
      aggregates(makeLocalOpCounter()),
      nestedAggregates(makeLocalOpCounter()),
      insertsOnExistingDoc(makeLocalOpCounter()),
      updatesOnMissingDoc(makeLocalOpCounter()),
      deletesWasEmpty(makeLocalOpCounter()),
      deletesFromMissingNamespace(makeLocalOpCounter()),
      acceptableErrorsInCommand(makeLocalOpCounter()),
      recordIdsReplicatedDocIdMismatches(makeLocalOpCounter()),
      queriesDeprecated(makeLocalOpCounter()) {}

OpCounters::OpCounters(otel::metrics::MetricName insertName,
                       otel::metrics::MetricName queryName,
                       otel::metrics::MetricName updateName,
                       otel::metrics::MetricName deleteOpName,
                       otel::metrics::MetricName getMoreName,
                       otel::metrics::MetricName commandName,
                       otel::metrics::MetricName aggregateName)
    : inserts(makeOtelOpCounter(insertName)),
      queries(makeOtelOpCounter(queryName)),
      updates(makeOtelOpCounter(updateName)),
      deletes(makeOtelOpCounter(deleteOpName)),
      getMores(makeOtelOpCounter(getMoreName)),
      commands(makeOtelOpCounter(commandName)),
      aggregates(makeOtelOpCounter(aggregateName)),
      nestedAggregates(makeLocalOpCounter()),
      insertsOnExistingDoc(makeLocalOpCounter()),
      updatesOnMissingDoc(makeLocalOpCounter()),
      deletesWasEmpty(makeLocalOpCounter()),
      deletesFromMissingNamespace(makeLocalOpCounter()),
      acceptableErrorsInCommand(makeLocalOpCounter()),
      recordIdsReplicatedDocIdMismatches(makeLocalOpCounter()),
      queriesDeprecated(makeLocalOpCounter()) {}

BSONObj OpCounters::getObj() const {
    BSONObjBuilder b;
    b.append("insert", inserts->value());
    b.append("query", queries->value());
    b.append("update", updates->value());
    b.append("delete", deletes->value());
    b.append("getmore", getMores->value());
    b.append("command", commands->value());
    b.append("aggregate", aggregates->value());

    auto queryDep = queriesDeprecated->value();
    if (queryDep > 0) {
        BSONObjBuilder d(b.subobjStart("deprecated"));
        d.append("query", queryDep);
    }

    // Append counters for constraint relaxations, only if any have fired.
    auto nInsertOnExistingDoc = insertsOnExistingDoc->value();
    auto nUpdateOnMissingDoc = updatesOnMissingDoc->value();
    auto nDeleteWasEmpty = deletesWasEmpty->value();
    auto nDeleteFromMissingNamespace = deletesFromMissingNamespace->value();
    auto nAcceptableErrorInCommand = acceptableErrorsInCommand->value();
    auto nRecordIdsReplicatedDocIdMismatch = recordIdsReplicatedDocIdMismatches->value();
    auto totalRelaxed = nInsertOnExistingDoc + nUpdateOnMissingDoc + nDeleteWasEmpty +
        nDeleteFromMissingNamespace + nAcceptableErrorInCommand + nRecordIdsReplicatedDocIdMismatch;

    if (totalRelaxed > 0) {
        BSONObjBuilder d(b.subobjStart("constraintsRelaxed"));
        d.append("insertOnExistingDoc", nInsertOnExistingDoc);
        d.append("updateOnMissingDoc", nUpdateOnMissingDoc);
        d.append("deleteWasEmpty", nDeleteWasEmpty);
        d.append("deleteFromMissingNamespace", nDeleteFromMissingNamespace);
        d.append("acceptableErrorInCommand", nAcceptableErrorInCommand);
        d.append("recordIdsReplicatedDocIdMismatch", nRecordIdsReplicatedDocIdMismatch);
    }

    return b.obj();
}

OpCounters& globalOpCounters() {
    using namespace otel::metrics;
    static StaticImmortal<OpCounters> instance{
        MetricNames::kInsertOpCount,
        MetricNames::kQueryOpCount,
        MetricNames::kUpdateOpCount,
        MetricNames::kDeleteOpCount,
        MetricNames::kGetMoreOpCount,
        MetricNames::kCommandOpCount,
        MetricNames::kAggregateOpCount,
    };
    return *instance;
}

OpCounters& replOpCounters() {
    static StaticImmortal<OpCounters> instance;
    return *instance;
}

}  // namespace mongo
