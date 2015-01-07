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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/exec/delete.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/util/log.h"

namespace mongo {

    // static
    const char* DeleteStage::kStageType = "DELETE";

    DeleteStage::DeleteStage(OperationContext* txn,
                             const DeleteStageParams& params,
                             WorkingSet* ws,
                             Collection* collection,
                             PlanStage* child)
        : _txn(txn),
          _params(params),
          _ws(ws),
          _collection(collection),
          _child(child),
          _commonStats(kStageType) { }

    DeleteStage::~DeleteStage() {}

    bool DeleteStage::isEOF() {
        if (!_collection) {
            return true;
        }
        if (!_params.isMulti && _specificStats.docsDeleted > 0) {
            return true;
        }
        return _child->isEOF();
    }

    PlanStage::StageState DeleteStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (isEOF()) { return PlanStage::IS_EOF; }
        invariant(_collection); // If isEOF() returns false, we must have a collection.

        WorkingSetID id = WorkingSet::INVALID_ID;
        StageState status = _child->work(&id);

        if (PlanStage::ADVANCED == status) {
            WorkingSetMember* member = _ws->get(id);
            if (!member->hasLoc()) {
                _ws->free(id);
                const std::string errmsg = "delete stage failed to read member w/ loc from child";
                *out = WorkingSetCommon::allocateStatusMember(_ws, Status(ErrorCodes::InternalError,
                                                                          errmsg));
                return PlanStage::FAILURE;
            }
            RecordId rloc = member->loc;

            // If the working set member is in the owned obj with loc state, then the document may
            // have already been deleted after-being force-fetched.
            if (WorkingSetMember::LOC_AND_OWNED_OBJ == member->state) {
                BSONObj deletedDoc;
                if (!_collection->findDoc(_txn, rloc, &deletedDoc)) {
                    // Doc is already deleted. Nothing more to do.
                    ++_commonStats.needTime;
                    return PlanStage::NEED_TIME;
                }
            }

            _ws->free(id);

            BSONObj deletedDoc;

            // TODO: Do we want to buffer docs and delete them in a group rather than
            // saving/restoring state repeatedly?
            _child->saveState();

            {
                WriteUnitOfWork wunit(_txn);

                const bool deleteCappedOK = false;
                const bool deleteNoWarn = false;

                // Do the write, unless this is an explain.
                if (!_params.isExplain) {
                    _collection->deleteDocument(_txn, rloc, deleteCappedOK, deleteNoWarn,
                                                _params.shouldCallLogOp ? &deletedDoc : NULL);

                    if (_params.shouldCallLogOp) {
                        if (deletedDoc.isEmpty()) {
                            log() << "Deleted object without id in collection " << _collection->ns()
                            << ", not logging.";
                        }
                        else {
                            bool replJustOne = true;
                            repl::logOp(_txn, "d", _collection->ns().ns().c_str(), deletedDoc, 0,
                                        &replJustOne, _params.fromMigrate);
                        }
                    }
                }

                wunit.commit();
            }

            //  As restoreState may restore (recreate) cursors, cursors are tied to the
            //  transaction in which they are created, and a WriteUnitOfWork is a
            //  transaction, make sure to restore the state outside of the WritUnitOfWork.
            _child->restoreState(_txn);

            ++_specificStats.docsDeleted;

            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
        else if (PlanStage::FAILURE == status) {
            *out = id;
            // If a stage fails, it may create a status WSM to indicate why it failed, in which case
            // 'id' is valid.  If ID is invalid, we create our own error message.
            if (WorkingSet::INVALID_ID == id) {
                const std::string errmsg = "delete stage failed to read in results from child";
                *out = WorkingSetCommon::allocateStatusMember(_ws, Status(ErrorCodes::InternalError,
                                                                          errmsg));
                return PlanStage::FAILURE;
            }
            return status;
        }
        else if (PlanStage::NEED_TIME == status) {
            ++_commonStats.needTime;
        }
        else if (PlanStage::NEED_FETCH == status) {
            *out = id;
            ++_commonStats.needFetch;
        }

        return status;
    }

    void DeleteStage::saveState() {
        _txn = NULL;
        ++_commonStats.yields;
        _child->saveState();
    }

    void DeleteStage::restoreState(OperationContext* opCtx) {
        invariant(_txn == NULL);
        _txn = opCtx;
        ++_commonStats.unyields;
        _child->restoreState(opCtx);

        const NamespaceString& ns(_collection->ns());
        massert(28537,
                str::stream() << "Demoted from primary while removing from " << ns.ns(),
                !_params.shouldCallLogOp ||
                repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(ns.db()));
    }

    void DeleteStage::invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
        ++_commonStats.invalidates;
        _child->invalidate(txn, dl, type);
    }

    vector<PlanStage*> DeleteStage::getChildren() const {
        vector<PlanStage*> children;
        children.push_back(_child.get());
        return children;
    }

    PlanStageStats* DeleteStage::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_DELETE));
        ret->specific.reset(new DeleteStats(_specificStats));
        ret->children.push_back(_child->getStats());
        return ret.release();
    }

    const CommonStats* DeleteStage::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* DeleteStage::getSpecificStats() {
        return &_specificStats;
    }

    // static
    long long DeleteStage::getNumDeleted(PlanExecutor* exec) {
        invariant(exec->getRootStage()->isEOF());
        invariant(exec->getRootStage()->stageType() == STAGE_DELETE);
        DeleteStage* deleteStage = static_cast<DeleteStage*>(exec->getRootStage());
        const DeleteStats* deleteStats =
            static_cast<const DeleteStats*>(deleteStage->getSpecificStats());
        return deleteStats->docsDeleted;
    }

}  // namespace mongo
