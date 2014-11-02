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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/group.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"

namespace mongo {

    namespace {

        // Helper function that extracts the group key from a BSONObj.
        Status getKey(const BSONObj& obj,
                      const BSONObj& keyPattern,
                      ScriptingFunction func,
                      Scope* s,
                      BSONObj* key) {
            if (func) {
                BSONObjBuilder b(obj.objsize() + 32);
                b.append("0", obj);
                const BSONObj& k = b.obj();
                int res = s->invoke(func, &k, 0);
                if (res != 0) {
                    return Status(ErrorCodes::BadValue,
                                  str::stream() << "invoke failed in $keyf: " << s->getError());
                }
                int type = s->type("__returnValue");
                if (type != Object) {
                    return Status(ErrorCodes::BadValue, "return of $key has to be an object");
                }
                *key = s->getObject("__returnValue");
                return Status::OK();
            }
            *key = obj.extractFields(keyPattern, true).getOwned();
            return Status::OK();
        }

    }  // namespace

    // static
    const char* GroupStage::kStageType = "GROUP";

    GroupStage::GroupStage(OperationContext* txn,
                           const GroupRequest& request,
                           WorkingSet* workingSet,
                           PlanStage* child)
        : _txn(txn),
          _request(request),
          _ws(workingSet),
          _commonStats(kStageType),
          _specificStats(),
          _child(child),
          _groupState(GroupState_Initializing),
          _reduceFunction(0),
          _keyFunction(0) {}

    void GroupStage::initGroupScripting() {
        // Initialize _scope.
        const std::string userToken =
            ClientBasic::getCurrent()->getAuthorizationSession()
                                     ->getAuthenticatedUserNamesToken();

        const NamespaceString nss(_request.ns);
        _scope = globalScriptEngine->getPooledScope(_txn, nss.db().toString(), "group" + userToken);
        if (!_request.reduceScope.isEmpty()) {
            _scope->init(&_request.reduceScope);
        }
        _scope->setObject("$initial", _request.initial, true);
        _scope->exec("$reduce = " + _request.reduceCode, "$group reduce setup", false, true, true,
                     100);
        _scope->exec("$arr = [];", "$group reduce setup 2", false, true, true, 100);

        // Initialize _reduceFunction.
        _reduceFunction = _scope->createFunction("function(){ "
                                                 "  if ( $arr[n] == null ){ "
                                                 "    next = {}; "
                                                 "    Object.extend( next , $key ); "
                                                 "    Object.extend( next , $initial , true ); "
                                                 "    $arr[n] = next; "
                                                 "    next = null; "
                                                 "  } "
                                                 "  $reduce( obj , $arr[n] ); "
                                                 "}");

        // Initialize _keyFunction, if a key function was provided.
        if (_request.keyFunctionCode.size()) {
            _keyFunction = _scope->createFunction(_request.keyFunctionCode.c_str());
        }
    }

    Status GroupStage::processObject(const BSONObj& obj) {
        BSONObj key;
        Status getKeyStatus = getKey(obj, _request.keyPattern, _keyFunction, _scope.get(),
                                     &key);
        if (!getKeyStatus.isOK()) {
            return getKeyStatus;
        }

        int& n = _groupMap[key];
        if (n == 0) {
            n = _groupMap.size();
            _scope->setObject("$key", key, true);
            if (n > 20000) {
                return Status(ErrorCodes::BadValue,
                              "group() can't handle more than 20000 unique keys");
            }
        }

        _scope->setObject("obj", obj, true);
        _scope->setNumber("n", n - 1);
        if (_scope->invoke(_reduceFunction, 0, 0, 0, true)) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "reduce invoke failed: " << _scope->getError());
        }

        return Status::OK();
    }

    BSONObj GroupStage::finalizeResults() {
        if (!_request.finalize.empty()) {
            _scope->exec("$finalize = " + _request.finalize, "$group finalize define", false,
                         true, true, 100);
            ScriptingFunction finalizeFunction =
                _scope->createFunction("function(){ "
                                       "  for(var i=0; i < $arr.length; i++){ "
                                       "  var ret = $finalize($arr[i]); "
                                       "  if (ret !== undefined) "
                                       "    $arr[i] = ret; "
                                       "  } "
                                       "}");
            _scope->invoke(finalizeFunction, 0, 0, 0, true);
        }

        _specificStats.nGroups = _groupMap.size();

        BSONObj results = _scope->getObject("$arr").getOwned();

        _scope->exec("$arr = [];", "$group reduce setup 2", false, true, true, 100);
        _scope->gc();

        return results;
    }

    PlanStage::StageState GroupStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (isEOF()) { return PlanStage::IS_EOF; }

        // On the first call to work(), call initGroupScripting().
        if (_groupState == GroupState_Initializing) {
            initGroupScripting();
            _groupState = GroupState_ReadingFromChild;
            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }

        // Otherwise, read from our child.
        invariant(_groupState == GroupState_ReadingFromChild);
        WorkingSetID id = WorkingSet::INVALID_ID;
        StageState state = _child->work(&id);

        if (PlanStage::NEED_TIME == state) {
            ++_commonStats.needTime;
            return state;
        }
        else if (PlanStage::FAILURE == state) {
            *out = id;
            // If a stage fails, it may create a status WSM to indicate why it failed, in which
            // case 'id' is valid.  If ID is invalid, we create our own error message.
            if (WorkingSet::INVALID_ID == id) {
                const std::string errmsg = "group stage failed to read in results from child";
                *out = WorkingSetCommon::allocateStatusMember(_ws,
                                                              Status(ErrorCodes::InternalError,
                                                                     errmsg));
            }
            return state;
        }
        else if (PlanStage::DEAD == state) {
            return state;
        }
        else if (PlanStage::ADVANCED == state) {
            WorkingSetMember* member = _ws->get(id);
            // Group queries can't have projections. This means that covering analysis will always
            // add a fetch. We should always get fetched data, and never just key data.
            invariant(member->hasObj());

            Status status = processObject(member->obj);
            if (!status.isOK()) {
                *out = WorkingSetCommon::allocateStatusMember(_ws, status);
                return PlanStage::FAILURE;
            }

            _ws->free(id);

            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
        else {
            // We're done reading from our child.
            invariant(PlanStage::IS_EOF == state);

            // Transition to state "done."  Future calls to work() will return IS_EOF.
            _groupState = GroupState_Done;

            BSONObj results = finalizeResults();

            *out = _ws->allocate();
            WorkingSetMember* member = _ws->get(*out);
            member->obj = results;
            member->state = WorkingSetMember::OWNED_OBJ;

            ++_commonStats.advanced;
            return PlanStage::ADVANCED;
        }
    }

    bool GroupStage::isEOF() {
        return _groupState == GroupState_Done;
    }

    void GroupStage::saveState() {
        ++_commonStats.yields;
        _child->saveState();
    }

    void GroupStage::restoreState(OperationContext* opCtx) {
        _txn = opCtx;
        ++_commonStats.unyields;
        _child->restoreState(opCtx);
    }

    void GroupStage::invalidate(const DiskLoc& dl, InvalidationType type) {
        ++_commonStats.invalidates;
        _child->invalidate(dl, type);
    }

    vector<PlanStage*> GroupStage::getChildren() const {
        vector<PlanStage*> children;
        children.push_back(_child.get());
        return children;
    }

    PlanStageStats* GroupStage::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_GROUP));
        GroupStats* groupStats = new GroupStats(_specificStats);
        ret->specific.reset(groupStats);
        ret->children.push_back(_child->getStats());
        return ret.release();
    }

    const CommonStats* GroupStage::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* GroupStage::getSpecificStats() {
        return &_specificStats;
    }

}  // namespace mongo
