/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/s/write_ops/write_op.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using std::stringstream;
using std::vector;

static void clear(vector<ChildWriteOp*>* childOps) {
    for (vector<ChildWriteOp*>::const_iterator it = childOps->begin(); it != childOps->end();
         ++it) {
        delete *it;
    }
    childOps->clear();
}

WriteOp::~WriteOp() {
    clear(&_childOps);
    clear(&_history);
}

const BatchItemRef& WriteOp::getWriteItem() const {
    return _itemRef;
}

WriteOpState WriteOp::getWriteState() const {
    return _state;
}

const WriteErrorDetail& WriteOp::getOpError() const {
    dassert(_state == WriteOpState_Error);
    return *_error;
}

Status WriteOp::targetWrites(OperationContext* txn,
                             const NSTargeter& targeter,
                             std::vector<TargetedWrite*>* targetedWrites) {
    bool isUpdate = _itemRef.getOpType() == BatchedCommandRequest::BatchType_Update;
    bool isDelete = _itemRef.getOpType() == BatchedCommandRequest::BatchType_Delete;
    bool isIndexInsert = _itemRef.getRequest()->isInsertIndexRequest();

    Status targetStatus = Status::OK();
    OwnedPointerVector<ShardEndpoint> endpointsOwned;
    vector<ShardEndpoint*>& endpoints = endpointsOwned.mutableVector();

    if (isUpdate) {
        targetStatus = targeter.targetUpdate(txn, *_itemRef.getUpdate(), &endpoints);
    } else if (isDelete) {
        targetStatus = targeter.targetDelete(txn, *_itemRef.getDelete(), &endpoints);
    } else {
        dassert(_itemRef.getOpType() == BatchedCommandRequest::BatchType_Insert);

        ShardEndpoint* endpoint = NULL;
        // TODO: Remove the index targeting stuff once there is a command for it
        if (!isIndexInsert) {
            targetStatus = targeter.targetInsert(txn, _itemRef.getDocument(), &endpoint);
        } else {
            // TODO: Retry index writes with stale version?
            targetStatus = targeter.targetCollection(&endpoints);
        }

        if (!targetStatus.isOK()) {
            dassert(NULL == endpoint);
            return targetStatus;
        }

        // Store single endpoint result if we targeted a single endpoint
        if (endpoint)
            endpoints.push_back(endpoint);
    }

    // If we're targeting more than one endpoint with an update/delete, we have to target
    // everywhere since we cannot currently retry partial results.
    // NOTE: Index inserts are currently specially targeted only at the current collection to
    // avoid creating collections everywhere.
    if (targetStatus.isOK() && endpoints.size() > 1u && !isIndexInsert) {
        endpointsOwned.clear();
        invariant(endpoints.empty());
        targetStatus = targeter.targetAllShards(&endpoints);
    }

    // If we had an error, stop here
    if (!targetStatus.isOK())
        return targetStatus;

    for (vector<ShardEndpoint*>::iterator it = endpoints.begin(); it != endpoints.end(); ++it) {
        ShardEndpoint* endpoint = *it;

        _childOps.push_back(new ChildWriteOp(this));

        WriteOpRef ref(_itemRef.getItemIndex(), _childOps.size() - 1);

        // For now, multiple endpoints imply no versioning - we can't retry half a multi-write
        if (endpoints.size() == 1u) {
            targetedWrites->push_back(new TargetedWrite(*endpoint, ref));
        } else {
            ShardEndpoint broadcastEndpoint(endpoint->shardName, ChunkVersion::IGNORED());
            targetedWrites->push_back(new TargetedWrite(broadcastEndpoint, ref));
        }

        _childOps.back()->pendingWrite = targetedWrites->back();
        _childOps.back()->state = WriteOpState_Pending;
    }

    _state = WriteOpState_Pending;
    return Status::OK();
}

size_t WriteOp::getNumTargeted() {
    return _childOps.size();
}

static bool isRetryErrCode(int errCode) {
    return errCode == ErrorCodes::StaleShardVersion;
}

// Aggregate a bunch of errors for a single op together
static void combineOpErrors(const vector<ChildWriteOp*>& errOps, WriteErrorDetail* error) {
    // Special case single response
    if (errOps.size() == 1) {
        errOps.front()->error->cloneTo(error);
        return;
    }

    error->setErrCode(ErrorCodes::MultipleErrorsOccurred);

    // Generate the multi-error message below
    stringstream msg;
    msg << "multiple errors for op : ";

    BSONArrayBuilder errB;
    for (vector<ChildWriteOp*>::const_iterator it = errOps.begin(); it != errOps.end(); ++it) {
        const ChildWriteOp* errOp = *it;
        if (it != errOps.begin())
            msg << " :: and :: ";
        msg << errOp->error->getErrMessage();
        errB.append(errOp->error->toBSON());
    }

    error->setErrInfo(BSON("causedBy" << errB.arr()));
    error->setIndex(errOps.front()->error->getIndex());
    error->setErrMessage(msg.str());
}

/**
 * This is the core function which aggregates all the results of a write operation on multiple
 * shards and updates the write operation's state.
 */
void WriteOp::updateOpState() {
    vector<ChildWriteOp*> childErrors;

    bool isRetryError = true;
    for (vector<ChildWriteOp*>::iterator it = _childOps.begin(); it != _childOps.end(); it++) {
        ChildWriteOp* childOp = *it;

        // Don't do anything till we have all the info
        if (childOp->state != WriteOpState_Completed && childOp->state != WriteOpState_Error) {
            return;
        }

        if (childOp->state == WriteOpState_Error) {
            childErrors.push_back(childOp);
            // Any non-retry error aborts all
            if (!isRetryErrCode(childOp->error->getErrCode()))
                isRetryError = false;
        }
    }

    if (!childErrors.empty() && isRetryError) {
        // Since we're using broadcast mode for multi-shard writes, which cannot SCE
        dassert(childErrors.size() == 1u);
        _state = WriteOpState_Ready;
    } else if (!childErrors.empty()) {
        _error.reset(new WriteErrorDetail);
        combineOpErrors(childErrors, _error.get());
        _state = WriteOpState_Error;
    } else {
        _state = WriteOpState_Completed;
    }

    // Now that we're done with the child ops, do something with them
    // TODO: Don't store unlimited history?
    dassert(_state != WriteOpState_Pending);
    _history.insert(_history.end(), _childOps.begin(), _childOps.end());
    _childOps.clear();
}

void WriteOp::cancelWrites(const WriteErrorDetail* why) {
    dassert(_state == WriteOpState_Pending || _state == WriteOpState_Ready);
    for (vector<ChildWriteOp*>::iterator it = _childOps.begin(); it != _childOps.end(); ++it) {
        ChildWriteOp* childOp = *it;

        if (childOp->state == WriteOpState_Pending) {
            childOp->endpoint.reset(new ShardEndpoint(childOp->pendingWrite->endpoint));
            if (why) {
                childOp->error.reset(new WriteErrorDetail);
                why->cloneTo(childOp->error.get());
            }
            childOp->state = WriteOpState_Cancelled;
        }
    }

    _history.insert(_history.end(), _childOps.begin(), _childOps.end());
    _childOps.clear();

    _state = WriteOpState_Ready;
}

void WriteOp::noteWriteComplete(const TargetedWrite& targetedWrite) {
    const WriteOpRef& ref = targetedWrite.writeOpRef;
    dassert(static_cast<size_t>(ref.second) < _childOps.size());
    ChildWriteOp& childOp = *_childOps.at(ref.second);

    childOp.pendingWrite = NULL;
    childOp.endpoint.reset(new ShardEndpoint(targetedWrite.endpoint));
    childOp.state = WriteOpState_Completed;
    updateOpState();
}

void WriteOp::noteWriteError(const TargetedWrite& targetedWrite, const WriteErrorDetail& error) {
    const WriteOpRef& ref = targetedWrite.writeOpRef;
    ChildWriteOp& childOp = *_childOps.at(ref.second);

    childOp.pendingWrite = NULL;
    childOp.endpoint.reset(new ShardEndpoint(targetedWrite.endpoint));
    childOp.error.reset(new WriteErrorDetail);
    error.cloneTo(childOp.error.get());
    dassert(ref.first == _itemRef.getItemIndex());
    childOp.error->setIndex(_itemRef.getItemIndex());
    childOp.state = WriteOpState_Error;
    updateOpState();
}

void WriteOp::setOpError(const WriteErrorDetail& error) {
    dassert(_state == WriteOpState_Ready);
    _error.reset(new WriteErrorDetail);
    error.cloneTo(_error.get());
    _error->setIndex(_itemRef.getItemIndex());
    _state = WriteOpState_Error;
    // No need to updateOpState, set directly
}
}
