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
#include "mongo/scripting/engine.h"

namespace mongo {

class Collection;

/**
 * A description of a request for a group operation.  Copyable.
 */
struct GroupRequest {
    // Namespace to operate on (e.g. "foo.bar").
    std::string ns;

    // A predicate describing the set of documents to group.
    BSONObj query;

    // The field(s) to group by.  Alternative to "keyFunctionCode".  Empty if "keyFunctionCode"
    // is being used instead.
    BSONObj keyPattern;

    // A Javascript function that maps a document to a key object.  Alternative to "keyPattern".
    // Empty is "keyPattern" is being used instead.
    std::string keyFunctionCode;

    // The collation used for string comparisons. If empty, simple binary comparison with memcmp()
    // is used.
    BSONObj collation;

    // A Javascript function that takes a (input document, group result) pair and
    // updates the group result document.
    std::string reduceCode;

    // Scope for the reduce function.  Optional.
    BSONObj reduceScope;

    // The initial value for the group result.
    BSONObj initial;

    // A Javascript function that "finalizes" a group result.  Optional.
    std::string finalize;

    // Whether this is an explain of a group.
    bool explain;
};

/**
 * Stage used by the group command.  Consumes input documents from its child stage (returning
 * NEED_TIME once for each document produced by the child), returns ADVANCED exactly once with
 * the entire group result, then returns EOF.
 *
 * Only created through the getExecutorGroup path.
 */
class GroupStage final : public PlanStage {
    MONGO_DISALLOW_COPYING(GroupStage);

public:
    GroupStage(OperationContext* txn,
               const GroupRequest& request,
               WorkingSet* workingSet,
               PlanStage* child);

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() final;

    StageType stageType() const final {
        return STAGE_GROUP;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    /**
     * Keeps track of what this group is currently doing so that it can do the right thing on
     * the next call to work().
     */
    enum GroupState {
        // Need to initialize the underlying Javascript machinery.
        GroupState_Initializing,

        // Retrieving the next document from the child stage and processing it.
        GroupState_ReadingFromChild,

        // Results have been returned.
        GroupState_Done
    };

    // Initializes _scope, _reduceFunction and _keyFunction using the global scripting engine.
    Status initGroupScripting();

    // Updates _groupMap and _scope to account for the group key associated with this object.
    // Returns an error status if an error occurred, else Status::OK().
    Status processObject(const BSONObj& obj);

    // Finalize the results for this group operation. On success, returns with a BSONObj with
    // the results array. On failure, returns a non-OK status. Does not throw.
    StatusWith<BSONObj> finalizeResults();

    GroupRequest _request;

    // The WorkingSet we annotate with results.  Not owned by us.
    WorkingSet* _ws;

    GroupStats _specificStats;

    // Current state for this stage.
    GroupState _groupState;

    // The Scope object that all script operations for this group stage will use.  Initialized
    // by initGroupScripting().  Owned here.
    std::unique_ptr<Scope> _scope;

    // The reduce function for the group operation.  Initialized by initGroupScripting().  Owned
    // by _scope.
    ScriptingFunction _reduceFunction;

    // The key function for the group operation if one was provided by the user, else 0.
    // Initialized by initGroupScripting().  Owned by _scope.
    ScriptingFunction _keyFunction;

    // Map from group key => group index.  The group index is used to index into "$arr", a
    // variable owned by _scope which contains the group data for this key.
    std::map<BSONObj, int, BSONObjCmp> _groupMap;
};

}  // namespace mongo
