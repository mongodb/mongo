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
#include "mongo/db/catalog/database.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/scripting/engine.h"

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
                           Database* db,
                           const GroupRequest& request,
                           WorkingSet* workingSet,
                           PlanStage* child)
        : _txn(txn),
          _db(db),
          _request(request),
          _ws(workingSet),
          _commonStats(kStageType),
          _specificStats(),
          _child(child),
          _groupCompleted(false) {}

    PlanStage::StageState GroupStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (isEOF()) { return PlanStage::IS_EOF; }

        // Set the completed flag; this stage returns all results in a single call to work.
        // Subsequent calls will return EOF.
        _groupCompleted = true;

        // Initialize the Scope object.
        const std::string userToken =
            ClientBasic::getCurrent()->getAuthorizationSession()
                                     ->getAuthenticatedUserNamesToken();
        auto_ptr<Scope> s = globalScriptEngine->getPooledScope(_txn, _db->name(),
                                                               "group" + userToken);
        if (_request.reduceScope.size()) {
            s->init(_request.reduceScope.c_str());
        }
        s->setObject("$initial", _request.initial, true);
        s->exec("$reduce = " + _request.reduceCode, "$group reduce setup", false, true, true, 100);
        s->exec("$arr = [];", "$group reduce setup 2", false, true, true, 100);
        ScriptingFunction f =
            s->createFunction("function(){ "
                              "  if ( $arr[n] == null ){ "
                              "    next = {}; "
                              "    Object.extend( next , $key ); "
                              "    Object.extend( next , $initial , true ); "
                              "    $arr[n] = next; "
                              "    next = null; "
                              "  } "
                              "  $reduce( obj , $arr[n] ); "
                              "}");
        ScriptingFunction keyFunction = 0;
        if (_request.keyFunctionCode.size()) {
            keyFunction = s->createFunction(_request.keyFunctionCode.c_str());
        }

        // Construct the set of groups.
        map<BSONObj, int, BSONObjCmp> map;
        while (!_child->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            StageState status = _child->work(&id);

            if (PlanStage::IS_EOF == status) {
                break;
            }
            else if (PlanStage::NEED_TIME == status) {
                continue;
            }
            else if (PlanStage::FAILURE == status) {
                *out = id;
                // If a stage fails, it may create a status WSM to indicate why it failed, in which
                // case 'id' is valid.  If ID is invalid, we create our own error message.
                if (WorkingSet::INVALID_ID == id) {
                    const std::string errmsg = "group stage failed to read in results from child";
                    *out = WorkingSetCommon::allocateStatusMember(_ws,
                                                                  Status(ErrorCodes::InternalError,
                                                                         errmsg));
                }
                return status;
            }
            else if (PlanStage::DEAD == status) {
                return status;
            }
            invariant(PlanStage::ADVANCED == status);

            WorkingSetMember* member = _ws->get(id);
            // Group queries can't have projections. This means that covering analysis will always
            // add a fetch. We should always get fetched data, and never just key data.
            invariant(member->hasObj());
            BSONObj obj = member->obj;
            _ws->free(id);

            BSONObj key;
            Status getKeyStatus = getKey(obj, _request.keyPattern, keyFunction, s.get(), &key);
            if (!getKeyStatus.isOK()) {
                *out = WorkingSetCommon::allocateStatusMember(_ws, getKeyStatus);
                return PlanStage::FAILURE;
            }

            int& n = map[key];
            if (n == 0) {
                n = map.size();
                s->setObject("$key", key, true);
                if (n > 20000) {
                    const std::string errmsg = "group() can't handle more than 20000 unique keys";
                    *out = WorkingSetCommon::allocateStatusMember(_ws, Status(ErrorCodes::BadValue,
                                                                              errmsg));
                    return PlanStage::FAILURE;
                }
            }

            s->setObject("obj", obj, true);
            s->setNumber("n", n - 1);
            if (s->invoke(f, 0, 0, 0, true)) {
                const std::string errmsg = str::stream() << "reduce invoke failed: "
                                                         << s->getError();
                *out = WorkingSetCommon::allocateStatusMember(_ws, Status(ErrorCodes::BadValue,
                                                                          errmsg));
                return PlanStage::FAILURE;
            }
        }
        _specificStats.nGroups = map.size();

        // Invoke the finalize function.
        if (!_request.finalize.empty()) {
            s->exec("$finalize = " + _request.finalize, "$group finalize define", false, true,
                    true, 100);
            ScriptingFunction g =
                s->createFunction("function(){ "
                                  "  for(var i=0; i < $arr.length; i++){ "
                                  "  var ret = $finalize($arr[i]); "
                                  "  if (ret !== undefined) "
                                  "    $arr[i] = ret; "
                                  "  } "
                                  "}");
            s->invoke(g, 0, 0, 0, true);
        }

        // Return array of results.
        *out = _ws->allocate();
        WorkingSetMember* member = _ws->get(*out);
        member->obj = s->getObject("$arr").getOwned();
        member->state = WorkingSetMember::OWNED_OBJ;

        s->exec("$arr = [];", "$group reduce setup 2", false, true, true, 100);
        s->gc();

        ++_commonStats.advanced;
        return PlanStage::ADVANCED;
    }

    bool GroupStage::isEOF() {
        return _groupCompleted;
    }

    void GroupStage::saveState() {
        ++_commonStats.yields;
        _child->saveState();
        return;
    }

    void GroupStage::restoreState(OperationContext* opCtx) {
        ++_commonStats.unyields;
        _child->restoreState(opCtx);
        return;
    }

    void GroupStage::invalidate(const DiskLoc& dl, InvalidationType type) {
        ++_commonStats.invalidates;
        _child->invalidate(dl, type);
        return;
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
