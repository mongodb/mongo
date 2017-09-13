/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/exec/keep_mutations.h"

#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

// static
const char* KeepMutationsStage::kStageType = "KEEP_MUTATIONS";

KeepMutationsStage::KeepMutationsStage(OperationContext* opCtx,
                                       const MatchExpression* filter,
                                       WorkingSet* ws,
                                       PlanStage* child)
    : PlanStage(kStageType, opCtx),
      _workingSet(ws),
      _filter(filter),
      _doneReadingChild(false),
      _doneReturningFlagged(false) {
    _children.emplace_back(child);
}

KeepMutationsStage::~KeepMutationsStage() {}

bool KeepMutationsStage::isEOF() {
    return _doneReadingChild && _doneReturningFlagged;
}

PlanStage::StageState KeepMutationsStage::doWork(WorkingSetID* out) {
    // If we've returned as many results as we're limited to, isEOF will be true.
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    // Stream child results until the child is all done.
    if (!_doneReadingChild) {
        StageState status = child()->work(out);

        // Child is still returning results.  Pass them through.
        if (PlanStage::IS_EOF != status) {
            return status;
        }

        // Child is EOF.  We want to stream flagged results if there are any.
        _doneReadingChild = true;

        // Read out all of the flagged results from the working set.  We can't iterate through
        // the working set's flagged result set directly, since it may be modified later if
        // further documents are invalidated during a yield.
        std::copy(_workingSet->getFlagged().begin(),
                  _workingSet->getFlagged().end(),
                  std::back_inserter(_flagged));
        _flaggedIterator = _flagged.begin();
    }

    // We're streaming flagged results.
    invariant(!_doneReturningFlagged);
    if (_flaggedIterator == _flagged.end()) {
        _doneReturningFlagged = true;
        return PlanStage::IS_EOF;
    }

    WorkingSetID idToTest = *_flaggedIterator;
    _flaggedIterator++;

    WorkingSetMember* member = _workingSet->get(idToTest);
    if (Filter::passes(member, _filter)) {
        *out = idToTest;
        return PlanStage::ADVANCED;
    } else {
        _workingSet->free(idToTest);
        return PlanStage::NEED_TIME;
    }
}

unique_ptr<PlanStageStats> KeepMutationsStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret =
        make_unique<PlanStageStats>(_commonStats, STAGE_KEEP_MUTATIONS);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* KeepMutationsStage::getSpecificStats() const {
    return NULL;
}

}  // namespace mongo
