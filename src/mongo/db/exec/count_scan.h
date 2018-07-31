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

#pragma once

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {

class WorkingSet;

struct CountScanParams {
    CountScanParams(const IndexDescriptor& descriptor,
                    std::string indexName,
                    BSONObj keyPattern,
                    MultikeyPaths multikeyPaths,
                    bool multikey)
        : accessMethod(descriptor.getIndexCatalog()->getIndex(&descriptor)),
          name(std::move(indexName)),
          keyPattern(std::move(keyPattern)),
          multikeyPaths(std::move(multikeyPaths)),
          isMultiKey(multikey),
          isSparse(descriptor.isSparse()),
          isUnique(descriptor.unique()),
          isPartial(descriptor.isPartial()),
          version(descriptor.version()),
          collation(descriptor.infoObj()
                        .getObjectField(IndexDescriptor::kCollationFieldName)
                        .getOwned()) {
        invariant(accessMethod);
    }

    CountScanParams(OperationContext* opCtx, const IndexDescriptor& descriptor)
        : CountScanParams(descriptor,
                          descriptor.indexName(),
                          descriptor.keyPattern(),
                          descriptor.getMultikeyPaths(opCtx),
                          descriptor.isMultikey(opCtx)) {}

    const IndexAccessMethod* accessMethod;
    std::string name;

    BSONObj keyPattern;

    MultikeyPaths multikeyPaths;
    bool isMultiKey;

    bool isSparse;
    bool isUnique;
    bool isPartial;

    IndexDescriptor::IndexVersion version;

    BSONObj collation;

    BSONObj startKey;
    bool startKeyInclusive{true};

    BSONObj endKey;
    bool endKeyInclusive{true};
};

/**
 * Used by the count command. Scans an index from a start key to an end key. Creates a
 * WorkingSetMember for each matching index key in RID_AND_OBJ state. It has a null record id and an
 * empty object with a null snapshot id rather than real data. Returning real data is unnecessary
 * since all we need is the count.
 *
 * Only created through the getExecutorCount() path, as count is the only operation that doesn't
 * care about its data.
 */
class CountScan final : public PlanStage {
public:
    CountScan(OperationContext* opCtx, CountScanParams params, WorkingSet* workingSet);

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() final;
    void doSaveState() final;
    void doRestoreState() final;
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    StageType stageType() const final {
        return STAGE_COUNT_SCAN;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    // The WorkingSet we annotate with results.  Not owned by us.
    WorkingSet* _workingSet;

    // Index access. The pointer below is owned by Collection -> IndexCatalog.
    const IndexAccessMethod* _iam;

    std::unique_ptr<SortedDataInterface::Cursor> _cursor;

    // Could our index have duplicates?  If so, we use _returned to dedup.
    const bool _shouldDedup;
    stdx::unordered_set<RecordId, RecordId::Hasher> _returned;

    CountScanParams _params;

    CountScanStats _specificStats;
};

}  // namespace mongo
