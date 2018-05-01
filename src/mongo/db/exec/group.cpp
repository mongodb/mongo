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
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/stdx/memory.h"

namespace mongo {

// Forces a hang in the javascript execution while initializing the group stage.
MONGO_FP_DECLARE(hangInGroupReduceJs);

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

namespace dps = ::mongo::dotted_path_support;

namespace {

// Helper function that extracts the group key from a BSONObj.
Status getKey(
    const BSONObj& obj, const BSONObj& keyPattern, ScriptingFunction func, Scope* s, BSONObj* key) {
    if (func) {
        BSONObjBuilder b(obj.objsize() + 32);
        b.append("0", obj);
        const BSONObj& k = b.obj();
        try {
            s->invoke(func, &k, 0);
        } catch (const AssertionException& e) {
            return e.toStatus("Failed to invoke group keyf function: ");
        }
        int type = s->type("__returnValue");
        if (type != Object) {
            return Status(ErrorCodes::BadValue, "return of $key has to be an object");
        }
        *key = s->getObject("__returnValue");
        return Status::OK();
    }
    *key = dps::extractElementsBasedOnTemplate(obj, keyPattern, true).getOwned();
    return Status::OK();
}

}  // namespace

// static
const char* GroupStage::kStageType = "GROUP";

GroupStage::GroupStage(OperationContext* opCtx,
                       const GroupRequest& request,
                       WorkingSet* workingSet,
                       PlanStage* child)
    : PlanStage(kStageType, opCtx),
      _request(request),
      _ws(workingSet),
      _specificStats(),
      _groupState(GroupState_Initializing),
      _reduceFunction(0),
      _keyFunction(0),
      _groupMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<int>()) {
    _children.emplace_back(child);
}

Status GroupStage::initGroupScripting() {
    // Initialize _scope.
    const std::string userToken =
        AuthorizationSession::get(Client::getCurrent())->getAuthenticatedUserNamesToken();

    _scope = getGlobalScriptEngine()->getPooledScope(
        getOpCtx(), _request.ns.db().toString(), "group" + userToken);
    if (!_request.reduceScope.isEmpty()) {
        _scope->init(&_request.reduceScope);
    }
    _scope->setObject("$initial", _request.initial, true);

    try {
        _scope->exec(
            "$reduce = " + _request.reduceCode, "group reduce init", false, true, true, 2 * 1000);
    } catch (const AssertionException& e) {
        return e.toStatus("Failed to initialize group reduce function: ");
    }

    try {
        _scope->exec("$arr = [];",
                     "group reduce init 2",
                     false,  // printResult
                     true,   // reportError
                     true,   // assertOnError
                     2 * 1000);
    } catch (const AssertionException& e) {
        return e.toStatus("Failed to initialize group reduce function: ");
    }

    // Initialize _reduceFunction.
    _reduceFunction = _scope->createFunction(
        "function(){ "
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

    return Status::OK();
}

Status GroupStage::processObject(const BSONObj& obj) {
    BSONObj key;
    Status getKeyStatus = getKey(obj, _request.keyPattern, _keyFunction, _scope.get(), &key);
    if (!getKeyStatus.isOK()) {
        return getKeyStatus;
    }

    _scope->advanceGeneration();

    int& n = _groupMap[key];
    if (n == 0) {
        n = _groupMap.size();
        _scope->setObject("$key", key, true);
        if (n > 20000) {
            return Status(ErrorCodes::BadValue, "group() can't handle more than 20000 unique keys");
        }
    }

    BSONObj objCopy = obj.getOwned();
    _scope->setObject("obj", objCopy, true);
    _scope->setNumber("n", n - 1);

    boost::optional<std::string> oldMsg;
    if (MONGO_FAIL_POINT(hangInGroupReduceJs)) {
        oldMsg = CurOpFailpointHelpers::updateCurOpMsg(getOpCtx(), "hangInGroupReduceJs");
    }
    auto resetMsgGuard = MakeGuard([&] {
        if (oldMsg) {
            CurOpFailpointHelpers::updateCurOpMsg(getOpCtx(), *oldMsg);
        }
    });
    try {
        _scope->invoke(_reduceFunction, 0, 0, 0, true /*assertOnError*/);
    } catch (const AssertionException& e) {
        return e.toStatus("Failed to invoke group reduce function: ");
    }

    return Status::OK();
}

StatusWith<BSONObj> GroupStage::finalizeResults() {
    if (!_request.finalize.empty()) {
        try {
            _scope->exec("$finalize = " + _request.finalize,
                         "group finalize init",
                         false,  // printResult
                         true,   // reportError
                         true,   // assertOnError
                         2 * 1000);
        } catch (const AssertionException& e) {
            return e.toStatus("Failed to initialize group finalize function: ");
        }
        ScriptingFunction finalizeFunction = _scope->createFunction(
            "function(){ "
            "  for(var i=0; i < $arr.length; i++){ "
            "  var ret = $finalize($arr[i]); "
            "  if (ret !== undefined) "
            "    $arr[i] = ret; "
            "  } "
            "}");
        try {
            _scope->invoke(finalizeFunction, 0, 0, 0, true /*assertOnError*/);
        } catch (const AssertionException& e) {
            return e.toStatus("Failed to invoke group finalize function: ");
        }
    }

    _specificStats.nGroups = _groupMap.size();

    BSONObj results = _scope->getObject("$arr").getOwned();

    try {
        _scope->exec("$arr = [];",
                     "group clean up",
                     false,  // printResult
                     true,   // reportError
                     true,   // assertOnError
                     2 * 1000);
    } catch (const AssertionException& e) {
        return e.toStatus("Failed to clean up group: ");
    }

    _scope->gc();

    return results;
}

PlanStage::StageState GroupStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    // On the first call to work(), call initGroupScripting().
    if (_groupState == GroupState_Initializing) {
        Status status = initGroupScripting();
        if (!status.isOK()) {
            *out = WorkingSetCommon::allocateStatusMember(_ws, status);
            return PlanStage::FAILURE;
        }
        _groupState = GroupState_ReadingFromChild;
        return PlanStage::NEED_TIME;
    }

    // Otherwise, read from our child.
    invariant(_groupState == GroupState_ReadingFromChild);
    WorkingSetID id = WorkingSet::INVALID_ID;
    StageState state = child()->work(&id);

    if (PlanStage::NEED_TIME == state) {
        return state;
    } else if (PlanStage::NEED_YIELD == state) {
        *out = id;
        return state;
    } else if (PlanStage::FAILURE == state || PlanStage::DEAD == state) {
        // The stage which produces a failure is responsible for allocating a working set member
        // with error details.
        invariant(WorkingSet::INVALID_ID != id);
        *out = id;
        return state;
    } else if (PlanStage::ADVANCED == state) {
        WorkingSetMember* member = _ws->get(id);
        // Group queries can't have projections. This means that covering analysis will always
        // add a fetch. We should always get fetched data, and never just key data.
        invariant(member->hasObj());

        Status status = processObject(member->obj.value());
        if (!status.isOK()) {
            *out = WorkingSetCommon::allocateStatusMember(_ws, status);
            return PlanStage::FAILURE;
        }

        _ws->free(id);

        return PlanStage::NEED_TIME;
    } else {
        // We're done reading from our child.
        invariant(PlanStage::IS_EOF == state);

        auto results = finalizeResults();
        if (!results.isOK()) {
            *out = WorkingSetCommon::allocateStatusMember(_ws, results.getStatus());
            return PlanStage::FAILURE;
        }

        // Transition to state "done."  Future calls to work() will return IS_EOF.
        _groupState = GroupState_Done;

        *out = _ws->allocate();
        WorkingSetMember* member = _ws->get(*out);
        member->obj = Snapshotted<BSONObj>(SnapshotId(), results.getValue());
        member->transitionToOwnedObj();

        return PlanStage::ADVANCED;
    }
}

bool GroupStage::isEOF() {
    return _groupState == GroupState_Done;
}

unique_ptr<PlanStageStats> GroupStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_GROUP);
    ret->specific = make_unique<GroupStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* GroupStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
