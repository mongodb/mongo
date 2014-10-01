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

#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/util/mongoutils/str.h"

// for updateCache
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/qlog.h"

namespace mongo {

    // static
    const char* CachedPlanStage::kStageType = "CACHED_PLAN";

    CachedPlanStage::CachedPlanStage(const Collection* collection,
                                     CanonicalQuery* cq,
                                     PlanStage* mainChild,
                                     QuerySolution* mainQs,
                                     PlanStage* backupChild,
                                     QuerySolution* backupQs)
        : _collection(collection),
          _canonicalQuery(cq),
          _mainQs(mainQs),
          _backupQs(backupQs),
          _mainChildPlan(mainChild),
          _backupChildPlan(backupChild),
          _usingBackupChild(false),
          _alreadyProduced(false),
          _updatedCache(false),
          _commonStats(kStageType) {}

    CachedPlanStage::~CachedPlanStage() {
        // We may have produced all necessary results without hitting EOF.
        // In this case, we still want to update the cache with feedback.
        if (!_updatedCache) {
            updateCache();
        }
    }

    bool CachedPlanStage::isEOF() { return getActiveChild()->isEOF(); }

    PlanStage::StageState CachedPlanStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (isEOF()) { return PlanStage::IS_EOF; }

        StageState childStatus = getActiveChild()->work(out);

        if (PlanStage::ADVANCED == childStatus) {
            // we'll skip backupPlan processing now
            _alreadyProduced = true;
        }
        else if (PlanStage::IS_EOF == childStatus) {
            updateCache();
        }
        else if (PlanStage::FAILURE == childStatus
             && !_alreadyProduced
             && !_usingBackupChild
             && NULL != _backupChildPlan.get()) {
            _usingBackupChild = true;
            childStatus = _backupChildPlan->work(out);
        }
        return childStatus;
    }

    void CachedPlanStage::saveState() {
        _mainChildPlan->saveState();

        if (NULL != _backupChildPlan.get()) {
            _backupChildPlan->saveState();
        }
        ++_commonStats.yields;
    }

    void CachedPlanStage::restoreState(OperationContext* opCtx) {
        _mainChildPlan->restoreState(opCtx);

        if (NULL != _backupChildPlan.get()) {
            _backupChildPlan->restoreState(opCtx);
        }
        ++_commonStats.unyields;
    }

    void CachedPlanStage::invalidate(const DiskLoc& dl, InvalidationType type) {
        if (! _usingBackupChild) {
            _mainChildPlan->invalidate(dl, type);
        }
        if (NULL != _backupChildPlan.get()) {
            _backupChildPlan->invalidate(dl, type);
        }
        ++_commonStats.invalidates;
    }

    vector<PlanStage*> CachedPlanStage::getChildren() const {
        vector<PlanStage*> children;
        if (_usingBackupChild) {
            children.push_back(_backupChildPlan.get());
        }
        else {
            children.push_back(_mainChildPlan.get());
        }
        return children;
    }

    PlanStageStats* CachedPlanStage::getStats() {
        _commonStats.isEOF = isEOF();

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_CACHED_PLAN));
        ret->specific.reset(new CachedPlanStats(_specificStats));

        if (_usingBackupChild) {
            ret->children.push_back(_backupChildPlan->getStats());
        }
        else {
            ret->children.push_back(_mainChildPlan->getStats());
        }

        return ret.release();
    }

    const CommonStats* CachedPlanStage::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* CachedPlanStage::getSpecificStats() {
        return &_specificStats;
    }

    void CachedPlanStage::updateCache() {
        _updatedCache = true;

        std::auto_ptr<PlanCacheEntryFeedback> feedback(new PlanCacheEntryFeedback());
        feedback->stats.reset(getStats());
        feedback->score = PlanRanker::scoreTree(feedback->stats.get());

        PlanCache* cache = _collection->infoCache()->getPlanCache();
        Status fbs = cache->feedback(*_canonicalQuery, feedback.release());

        if (!fbs.isOK()) {
            QLOG() << _canonicalQuery->ns() << ": Failed to update cache with feedback: "
                   << fbs.toString() << " - "
                   << "(query: " << _canonicalQuery->getQueryObj()
                   << "; sort: " << _canonicalQuery->getParsed().getSort()
                   << "; projection: " << _canonicalQuery->getParsed().getProj()
                   << ") is no longer in plan cache.";
        }
    }

    PlanStage* CachedPlanStage::getActiveChild() const {
        return _usingBackupChild ? _backupChildPlan.get() : _mainChildPlan.get();
    }

}  // namespace mongo
