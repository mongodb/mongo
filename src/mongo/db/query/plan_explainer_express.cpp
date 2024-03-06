/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/plan_explainer_express.h"

namespace mongo {

void PlanExplainerExpress::getSummaryStats(PlanSummaryStats* statsOut) const {
    statsOut->nReturned = _stats->advanced;

    if (_indexName) {
        statsOut->indexesUsed.insert(_indexName.get());
    }

    if (_stats->works > 0) {
        statsOut->totalKeysExamined = 1;
        statsOut->totalDocsExamined = 1;
    }
}

PlanExplainer::PlanStatsDetails PlanExplainerExpress::getWinningPlanStats(
    ExplainOptions::Verbosity verbosity) const {
    BSONObjBuilder bob;

    bob.append("isCached", false);
    bob.append("stage", getPlanSummary());
    PlanSummaryStats stats;
    getSummaryStats(&stats);

    if (!stats.indexesUsed.empty()) {
        bob.append("indexName", *stats.indexesUsed.begin());
    }

    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        bob.appendNumber("keysExamined", static_cast<long long>(stats.totalKeysExamined));
        bob.appendNumber("docsExamined", static_cast<long long>(stats.totalDocsExamined));
        bob.appendNumber("nReturned", static_cast<long long>(_stats->advanced));
    }

    return {bob.obj(), stats};
}
}  // namespace mongo
