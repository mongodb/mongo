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

#include "mongo/db/exec/working_set.h"
#include "mongo/util/unowned_ptr.h"

namespace mongo {

class CanonicalQuery;
class Collection;
class OperationContext;
class SeekableRecordCursor;

class WorkingSetCommon {
public:
    /**
     * Get an owned copy of the BSONObj the WSM refers to.
     * Requires either a valid BSONObj or valid RecordId.
     * Returns true if the fetch and invalidate succeeded, false otherwise.
     */
    static bool fetchAndInvalidateRecordId(OperationContext* txn,
                                           WorkingSetMember* member,
                                           const Collection* collection);

    /**
     * This must be called as part of "saveState" operations after all nodes in the tree save their
     * state.
     *
     * Iterates over WorkingSetIDs in 'workingSet' which are "sensitive to yield". These are ids
     * that have transitioned into the RID_AND_IDX state since the previous yield.
     *
     * The RID_AND_IDX members are tagged as suspicious so that they can be handled properly in case
     * the document keyed by the index key is deleted or updated during the yield.
     */
    static void prepareForSnapshotChange(WorkingSet* workingSet);

    /**
     * Transitions the WorkingSetMember with WorkingSetID 'id' from the RID_AND_IDX state to the
     * RID_AND_OBJ state by fetching a document. Does the fetch using 'cursor'.
     *
     * If false is returned, the document should not be considered for the result set. It is the
     * caller's responsibility to free 'id' in this case.
     *
     * WriteConflict exceptions may be thrown. When they are, 'member' will be unmodified.
     */
    static bool fetch(OperationContext* txn,
                      WorkingSet* workingSet,
                      WorkingSetID id,
                      unowned_ptr<SeekableRecordCursor> cursor);

    static bool fetchIfUnfetched(OperationContext* txn,
                                 WorkingSet* workingSet,
                                 WorkingSetID id,
                                 unowned_ptr<SeekableRecordCursor> cursor) {
        WorkingSetMember* member = workingSet->get(id);
        if (member->hasObj())
            return true;
        return fetch(txn, workingSet, id, cursor);
    }

    /**
     * Build a BSONObj which represents a Status to return in a WorkingSet.
     */
    static BSONObj buildMemberStatusObject(const Status& status);

    /**
     * Allocate a new WSM and initialize it with
     * the code and reason from the status.
     * Owned BSON object will have the following layout:
     * {
     *     ok: <ok>, // 1 for OK; 0 otherwise.
     *     code: <code>, // Status::code()
     *     errmsg: <errmsg> // Status::reason()
     * }
     */
    static WorkingSetID allocateStatusMember(WorkingSet* ws, const Status& status);

    /**
     * Returns true if object was created by allocateStatusMember().
     */
    static bool isValidStatusMemberObject(const BSONObj& obj);

    /**
     * Returns object in working set member created with allocateStatusMember().
     * Does not assume isValidStatusMemberObject.
     * If the WSID is invalid or the working set member is created by
     * allocateStatusMember, objOut will not be updated.
     */
    static void getStatusMemberObject(const WorkingSet& ws, WorkingSetID wsid, BSONObj* objOut);

    /**
     * Returns status from working set member object.
     * Assumes isValidStatusMemberObject().
     */
    static Status getMemberObjectStatus(const BSONObj& memberObj);

    /**
     * Returns status from working set member created with allocateStatusMember().
     * Assumes isValidStatusMemberObject().
     */
    static Status getMemberStatus(const WorkingSetMember& member);

    /**
     * Formats working set member object created with allocateStatusMember().
     */
    static std::string toStatusString(const BSONObj& obj);
};

}  // namespace mongo
