
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

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/query/count_request.h"

namespace mongo {

struct CountStageParams {
    CountStageParams(const CountRequest& request)
        : nss(request.getNs()), limit(request.getLimit()), skip(request.getSkip()) {}

    // Namespace to operate on (e.g. "foo.bar").
    NamespaceString nss;

    // An integer limiting the number of documents to count. 0 means no limit.
    long long limit;

    // An integer indicating to not include the first n documents in the count. 0 means no skip.
    long long skip;
};

/**
 * Stage used by the count command. This stage sits at the root of a plan tree and counts the number
 * of results returned by its child stage.
 *
 * This should not be confused with the CountScan stage. CountScan is a special index access stage
 * which can optimize index access for count operations in some cases. On the other hand, *every*
 * count op has a CountStage at its root.
 *
 * Only returns NEED_TIME until hitting EOF. The count result can be obtained by examining the
 * specific stats.
 */
class CountStage final : public PlanStage {
public:
    CountStage(OperationContext* opCtx,
               Collection* collection,
               CountStageParams params,
               WorkingSet* ws,
               PlanStage* child);

    bool isEOF() final;
    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_COUNT;
    }

    std::unique_ptr<PlanStageStats> getStats();

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    CountStageParams _params;

    // The number of documents that we still need to skip.
    long long _leftToSkip;

    // The working set used to pass intermediate results between stages. Not owned
    // by us.
    WorkingSet* _ws;

    CountStats _specificStats;
};

}  // namespace mongo
