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
#include <vector>

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {
class Collection;

/**
 * A simple wrapper around a SortedDataInterface::Cursor, which will advance the cursor until it is
 * exhausted.
 */
class IndexIteratorStage final : public PlanStage {
public:
    IndexIteratorStage(OperationContext* txn,
                       WorkingSet* ws,
                       Collection* collection,
                       IndexAccessMethod* iam,
                       BSONObj keyPattern,
                       std::unique_ptr<SortedDataInterface::Cursor> cursor);

    PlanStage::StageState doWork(WorkingSetID* out) final;

    bool isEOF() final;

    void doSaveState() final {
        _cursor->save();
    }
    void doRestoreState() final {
        _cursor->restore();
    }
    void doDetachFromOperationContext() final {
        _cursor->detachFromOperationContext();
    }
    void doReattachToOperationContext() final {
        _cursor->reattachToOperationContext(getOpCtx());
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    // Not used.
    SpecificStats* getSpecificStats() const final {
        return nullptr;
    }

    StageType stageType() const final {
        return STAGE_INDEX_ITERATOR;
    }

    static const char* kStageType;

private:
    Collection* _collection;
    WorkingSet* _ws;
    IndexAccessMethod* _iam;  // owned by Collection -> IndexCatalog.

    std::unique_ptr<SortedDataInterface::Cursor> _cursor;
    const BSONObj _keyPattern;
};

}  // namespace mongo
