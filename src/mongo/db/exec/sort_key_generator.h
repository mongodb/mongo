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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/stage_types.h"

namespace mongo {

class CollatorInterface;
class Collection;
class WorkingSetMember;

/**
 * Maps a WSM value to a BSONObj key that can then be sorted via BSONObjCmp.
 */
class SortKeyGenerator {
public:
    /**
     * 'sortSpec' is the BSONObj in the .sort(...) clause.
     *
     * 'opCtx' must point to a valid OperationContext, but 'opCtx' does not need to outlive the
     * constructed SortKeyGenerator.
     */
    SortKeyGenerator(OperationContext* opCtx,
                     const BSONObj& sortSpec,
                     const CollatorInterface* collator);

    /**
     * Returns the key used to sort 'member'. If the member is in LOC_AND_IDX state, it must not
     * contain a $meta textScore in its sort spec, and this function will use the index key data
     * stored in 'member' to extract the sort key. Otherwise, if the member is in LOC_AND_OBJ or
     * OWNED_OBJ state, this function will use the object data stored in 'member' to extract the
     * sort key.
     */
    Status getSortKey(const WorkingSetMember& member, BSONObj* objOut) const;

private:
    StatusWith<BSONObj> getSortKeyFromIndexKey(const WorkingSetMember& member) const;
    StatusWith<BSONObj> getSortKeyFromObject(const WorkingSetMember& member) const;

    const CollatorInterface* _collator = nullptr;

    // The raw object in .sort()
    BSONObj _rawSortSpec;

    // The sort pattern with any non-Btree sort pulled out.
    BSONObj _btreeObj;

    // If we're not sorting with a $meta value we can short-cut some work.
    bool _sortHasMeta = false;

    // Helper to extract sorting keys from documents.
    std::unique_ptr<BtreeKeyGenerator> _keyGen;
};

/**
 * Passes results from the child through after adding the sort key for each result as
 * WorkingSetMember computed data.
 */
class SortKeyGeneratorStage final : public PlanStage {
public:
    SortKeyGeneratorStage(OperationContext* opCtx,
                          PlanStage* child,
                          WorkingSet* ws,
                          const BSONObj& sortSpecObj,
                          const CollatorInterface* collator);

    bool isEOF() final;

    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_SORT_KEY_GENERATOR;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    WorkingSet* const _ws;

    // The raw sort pattern as expressed by the user.
    const BSONObj _sortSpec;

    const CollatorInterface* _collator;

    std::unique_ptr<SortKeyGenerator> _sortKeyGen;
};

}  // namespace mongo
