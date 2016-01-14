/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/fts/fts_matcher.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/fts/fts_spec.h"

namespace mongo {

using std::unique_ptr;

using fts::FTSMatcher;
using fts::FTSQueryImpl;
using fts::FTSSpec;


class OperationContext;
class RecordID;

/**
 * A stage that returns every document in the child that satisfies the FTS text matcher built with
 * the query parameter.
 *
 * Prerequisites: A single child stage that passes up WorkingSetMembers in the LOC_AND_OBJ state,
 * with associated text scores.
 */
class TextMatchStage final : public PlanStage {
public:
    TextMatchStage(OperationContext* opCtx,
                   unique_ptr<PlanStage> child,
                   const FTSQueryImpl& query,
                   const FTSSpec& spec,
                   WorkingSet* ws);
    ~TextMatchStage();

    void addChild(PlanStage* child);

    bool isEOF() final;

    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_TEXT_MATCH;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    // Text-specific phrase and negated term matcher.
    FTSMatcher _ftsMatcher;

    // Not owned by us.
    WorkingSet* _ws;

    TextMatchStats _specificStats;
};
}  // namespace mongo
