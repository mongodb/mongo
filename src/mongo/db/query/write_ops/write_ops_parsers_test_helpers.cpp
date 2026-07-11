// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/write_ops/write_ops_parsers_test_helpers.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <memory>
#include <set>
#include <string_view>
#include <vector>

namespace mongo {
namespace {
std::set<std::string_view> sequenceFields{"documents", "updates", "deletes", "GARBAGE"};
}

OpMsgRequest toOpMsg(std::string_view db, const BSONObj& cmd, bool useDocSequence) {
    OpMsgRequest request;
    BSONObjBuilder body;
    for (auto field : cmd) {
        if (useDocSequence && sequenceFields.count(field.fieldNameStringData())) {
            request.sequences.push_back(OpMsg::DocumentSequence{field.fieldName()});
            for (auto obj : field.Obj()) {
                request.sequences.back().objs.push_back(obj.Obj());
            }
        } else {
            body.append(field);
        }
    }
    body.append("$db", db);
    request.body = body.obj();
    return request;
}

write_ops::QueryStatsMetrics makeQueryStatsMetrics(int originalOpIndex,
                                                   long long keysExamined,
                                                   long long docsExamined,
                                                   long long nMatched,
                                                   long long nInserted) {
    CursorMetrics metrics;
    metrics.setKeysExamined(keysExamined);
    metrics.setDocsExamined(docsExamined);
    metrics.setBytesRead(100);
    metrics.setReadingTimeMicros(50);
    metrics.setWorkingTimeMillis(10);
    metrics.setHasSortStage(false);
    metrics.setUsedDisk(false);
    metrics.setFromMultiPlanner(false);
    metrics.setFromPlanCache(false);
    metrics.setPlanningTimeMicros(5);
    metrics.setNDocsSampled(0);
    metrics.setCpuNanos(1000);
    metrics.setNumInterruptChecks(1);
    metrics.setNMatched(nMatched);
    metrics.setNUpserted(0);
    metrics.setNModified(nMatched);
    metrics.setNDeleted(0);
    metrics.setNInserted(nInserted);

    write_ops::QueryStatsMetrics qsm;
    qsm.setOriginalOpIndex(originalOpIndex);
    qsm.setMetrics(metrics);
    return qsm;
}

}  // namespace mongo
