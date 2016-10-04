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
#include "mongo/db/query/count_request.h"

namespace mongo {

struct CountStageParams {
    CountStageParams(const CountRequest& request, bool useRecordStoreCount)
        : nss(request.getNs()),
          limit(request.getLimit()),
          skip(request.getSkip()),
          useRecordStoreCount(useRecordStoreCount) {}

    // Namespace to operate on (e.g. "foo.bar").
    NamespaceString nss;

    // An integer limiting the number of documents to count. 0 means no limit.
    long long limit;

    // An integer indicating to not include the first n documents in the count. 0 means no skip.
    long long skip;

    // True if this count stage should just ask the record store for a count instead of computing
    // one itself.
    //
    // Note: This strategy can lead to inaccurate counts on certain storage engines (including
    // WiredTiger).
    bool useRecordStoreCount;
};

/**
 * Stage used by the count command. This stage sits at the root of a plan tree
 * and counts the number of results returned by its child stage.
 *
 * This should not be confused with the CountScan stage. CountScan is a special
 * index access stage which can optimize index access for count operations in
 * some cases. On the other hand, *every* count op has a CountStage at its root.
 *
 * Only returns NEED_TIME until hitting EOF. The count result can be obtained by examining
 * the specific stats.
 */
class CountStage final : public PlanStage {
public:
    CountStage(OperationContext* txn,
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
    /**
     * Asks the record store for the count, applying the skip and limit if necessary. The result is
     * stored in '_specificStats'.
     *
     * This is only valid if the query and hint are both empty.
     */
    void recordStoreCount();

    // The collection over which we are counting.
    Collection* _collection;

    CountStageParams _params;

    // The number of documents that we still need to skip.
    long long _leftToSkip;

    // The working set used to pass intermediate results between stages. Not owned
    // by us.
    WorkingSet* _ws;

    CountStats _specificStats;
};

}  // namespace mongo
