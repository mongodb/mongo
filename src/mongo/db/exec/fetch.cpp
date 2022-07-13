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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/fetch.h"

#include <memory>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

namespace mongo {

using std::unique_ptr;
using std::vector;

// static
const char* FetchStage::kStageType = "FETCH";

FetchStage::FetchStage(ExpressionContext* expCtx,
                       WorkingSet* ws,
                       std::unique_ptr<PlanStage> child,
                       const MatchExpression* filter,
                       const CollectionPtr& collection)
    : RequiresCollectionStage(kStageType, expCtx, collection),
      _ws(ws),
      _filter((filter && !filter->isTriviallyTrue()) ? filter : nullptr),
      _idRetrying(WorkingSet::INVALID_ID) {
    _children.emplace_back(std::move(child));
}

FetchStage::~FetchStage() {}

bool FetchStage::isEOF() {
    if (WorkingSet::INVALID_ID != _idRetrying) {
        // We have a working set member that we need to retry.
        return false;
    }

    return child()->isEOF();
}

PlanStage::StageState FetchStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    // Either retry the last WSM we worked on or get a new one from our child.
    WorkingSetID id;
    StageState status;
    if (_idRetrying == WorkingSet::INVALID_ID) {
        status = child()->work(&id);
    } else {
        status = ADVANCED;
        id = _idRetrying;
        _idRetrying = WorkingSet::INVALID_ID;
    }

    if (PlanStage::ADVANCED == status) {
        WorkingSetMember* member = _ws->get(id);

        // If there's an obj there, there is no fetching to perform.
        if (member->hasObj()) {
            ++_specificStats.alreadyHasObj;
        } else {
            // We need a valid RecordId to fetch from and this is the only state that has one.
            verify(WorkingSetMember::RID_AND_IDX == member->getState());
            verify(member->hasRecordId());

            const auto ret = handlePlanStageYield(
                expCtx(),
                "FetchStage",
                collection()->ns().ns(),
                [&] {
                    const auto& coll = collection();
                    if (!_cursor)
                        _cursor = coll->getCursor(opCtx());

                    if (!WorkingSetCommon::fetch(
                            opCtx(), _ws, id, _cursor.get(), coll, coll->ns())) {
                        _ws->free(id);
                        return NEED_TIME;
                    }
                    return PlanStage::ADVANCED;
                },
                [&] {
                    // yieldHandler
                    // Ensure that the BSONObj underlying the WorkingSetMember is owned because it
                    // may be freed when we yield.
                    member->makeObjOwnedIfNeeded();
                    _idRetrying = id;
                    *out = WorkingSet::INVALID_ID;
                });
            if (ret != PlanStage::ADVANCED) {
                return ret;
            }
        }
        return returnIfMatches(member, id, out);
    } else if (PlanStage::NEED_YIELD == status) {
        *out = id;
    }

    return status;
}

void FetchStage::doSaveStateRequiresCollection() {
    if (_cursor) {
        _cursor->saveUnpositioned();
    }
}

void FetchStage::doRestoreStateRequiresCollection() {
    if (_cursor) {
        const bool couldRestore = _cursor->restore();
        uassert(50982, "could not restore cursor for FETCH stage", couldRestore);
    }
}

void FetchStage::doDetachFromOperationContext() {
    if (_cursor)
        _cursor->detachFromOperationContext();
}

void FetchStage::doReattachToOperationContext() {
    if (_cursor)
        _cursor->reattachToOperationContext(opCtx());
}

PlanStage::StageState FetchStage::returnIfMatches(WorkingSetMember* member,
                                                  WorkingSetID memberID,
                                                  WorkingSetID* out) {
    // We consider "examining a document" to be every time that we pass a document through
    // a filter by calling Filter::passes(...) below. Therefore, the 'docsExamined' metric
    // is not always equal to the number of documents that were fetched from the collection.
    // In particular, we can sometimes generate plans which have two fetch stages. The first
    // one actually grabs the document from the collection, and the second passes the
    // document through a second filter.
    //
    // One common example of this is geoNear. Suppose that a geoNear plan is searching an
    // annulus to find 2dsphere-indexed documents near some point (x, y) on the globe.
    // After fetching documents within geo hashes that intersect this annulus, the docs are
    // fetched and filtered to make sure that they really do fall into this annulus. However,
    // the user might also want to find only those documents for which accommodationType==
    // "restaurant". The planner will add a second fetch stage to filter by this non-geo
    // predicate.
    ++_specificStats.docsExamined;

    if (Filter::passes(member, _filter)) {
        *out = memberID;
        return PlanStage::ADVANCED;
    } else {
        _ws->free(memberID);
        return PlanStage::NEED_TIME;
    }
}

unique_ptr<PlanStageStats> FetchStage::getStats() {
    _commonStats.isEOF = isEOF();

    // Add a BSON representation of the filter to the stats tree, if there is one.
    if (nullptr != _filter) {
        BSONObjBuilder bob;
        _filter->serialize(&bob);
        _commonStats.filter = bob.obj();
    }

    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_FETCH);
    ret->specific = std::make_unique<FetchStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* FetchStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
