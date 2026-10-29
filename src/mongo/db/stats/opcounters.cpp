// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/stats/opcounters.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/static_immortal.h"

namespace mongo {

OpCounter::OpCounter(otel::metrics::MetricName name)
    : _otelCounter(&otel::metrics::MetricsService::instance().createInt64Counter(
          name, "Number of operations", otel::metrics::MetricUnit::kOperations)) {}

OpCounters::OpCounters() = default;

OpCounters::OpCounters(otel::metrics::MetricName insertName,
                       otel::metrics::MetricName queryName,
                       otel::metrics::MetricName updateName,
                       otel::metrics::MetricName deleteOpName,
                       otel::metrics::MetricName getMoreName,
                       otel::metrics::MetricName commandName,
                       otel::metrics::MetricName aggregateName)
    : inserts(insertName),
      queries(queryName),
      updates(updateName),
      deletes(deleteOpName),
      getMores(getMoreName),
      commands(commandName),
      aggregates(aggregateName) {}

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
