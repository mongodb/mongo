/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/record_id.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

/**
 * This stage outputs the union of its children.  It optionally deduplicates on RecordId.
 *
 * Preconditions: Valid RecordId.
 *
 * If we're deduping, we may fail to dedup any invalidated RecordId properly.
 */
class OrStage final : public PlanStage {
public:
    OrStage(OperationContext* opCtx, WorkingSet* ws, bool dedup, const MatchExpression* filter);

    void addChild(PlanStage* child);

    bool isEOF() final;

    StageState doWork(WorkingSetID* out) final;

    void doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) final;

    StageType stageType() const final {
        return STAGE_OR;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    // Not owned by us.
    WorkingSet* _ws;

    // The filter is not owned by us.
    const MatchExpression* _filter;

    // Which of _children are we calling work(...) on now?
    size_t _currentChild;

    // True if we dedup on RecordId, false otherwise.
    bool _dedup;

    // Which RecordIds have we returned?
    unordered_set<RecordId, RecordId::Hasher> _seen;

    // Stats
    OrStats _specificStats;
};

}  // namespace mongo
