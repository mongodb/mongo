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

#include "mongo/s/write_ops/write_op.h"

#include "mongo/s/transaction_router.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using std::stringstream;
using std::vector;

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

Status WriteOp::targetWrites(OperationContext* opCtx,
                             const NSTargeter& targeter,
                             std::vector<TargetedWrite*>* targetedWrites) {
    auto swEndpoints = [&]() -> StatusWith<std::vector<ShardEndpoint>> {
        if (_itemRef.getOpType() == BatchedCommandRequest::BatchType_Insert) {
            auto swEndpoint = targeter.targetInsert(opCtx, _itemRef.getDocument());
            if (!swEndpoint.isOK())
                return swEndpoint.getStatus();

            return std::vector<ShardEndpoint>{std::move(swEndpoint.getValue())};
        } else if (_itemRef.getOpType() == BatchedCommandRequest::BatchType_Update) {
            return targeter.targetUpdate(opCtx, _itemRef.getUpdate());
        } else if (_itemRef.getOpType() == BatchedCommandRequest::BatchType_Delete) {
            return targeter.targetDelete(opCtx, _itemRef.getDelete());
        } else {
            MONGO_UNREACHABLE;
        }
    }();

    // Unless executing as part of a transaction, if we're targeting more than one endpoint with an
    // update/delete, we have to target everywhere since we cannot currently retry partial results.
    //
    // NOTE: Index inserts are currently specially targeted only at the current collection to avoid
    // creating collections everywhere.
    const bool inTransaction = TransactionRouter::get(opCtx) != nullptr;
    if (swEndpoints.isOK() && swEndpoints.getValue().size() > 1u && !inTransaction) {
        swEndpoints = targeter.targetAllShards(opCtx);
    }

    // If we had an error, stop here
    if (!swEndpoints.isOK())
        return swEndpoints.getStatus();

    auto& endpoints = swEndpoints.getValue();

    for (auto&& endpoint : endpoints) {
        _childOps.emplace_back(this);

        WriteOpRef ref(_itemRef.getItemIndex(), _childOps.size() - 1);

        // Outside of a transaction, multiple endpoints currently imply no versioning, since we
        // can't retry half a regular multi-write.
        if (endpoints.size() > 1u && !inTransaction) {
            endpoint.shardVersion = ChunkVersion::IGNORED();
        }

        targetedWrites->push_back(new TargetedWrite(std::move(endpoint), ref));

        _childOps.back().pendingWrite = targetedWrites->back();
        _childOps.back().state = WriteOpState_Pending;
    }

    _state = WriteOpState_Pending;
    return Status::OK();
}

size_t WriteOp::getNumTargeted() {
    return _childOps.size();
}

static bool isRetryErrCode(int errCode) {
    return errCode == ErrorCodes::StaleShardVersion ||
        errCode == ErrorCodes::CannotImplicitlyCreateCollection;
}

static bool errorsAllSame(const vector<ChildWriteOp const*>& errOps) {
    auto errCode = errOps.front()->error->toStatus().code();
    if (std::all_of(++errOps.begin(), errOps.end(), [errCode](const ChildWriteOp* errOp) {
            return errOp->error->toStatus().code() == errCode;
        })) {
        return true;
    }

    return false;
}

// Aggregate a bunch of errors for a single op together
static void combineOpErrors(const vector<ChildWriteOp const*>& errOps, WriteErrorDetail* error) {
    // Special case single response or all errors are the same
    if (errOps.size() == 1 || errorsAllSame(errOps)) {
        errOps.front()->error->cloneTo(error);
        return;
    }

    // Generate the multi-error message below
    stringstream msg;
    msg << "multiple errors for op : ";

    BSONArrayBuilder errB;
    for (vector<ChildWriteOp const*>::const_iterator it = errOps.begin(); it != errOps.end();
         ++it) {
        const ChildWriteOp* errOp = *it;
        if (it != errOps.begin())
            msg << " :: and :: ";
        msg << errOp->error->toStatus().reason();
        errB.append(errOp->error->toBSON());
    }

    error->setErrInfo(BSON("causedBy" << errB.arr()));
    error->setIndex(errOps.front()->error->getIndex());
    error->setStatus({ErrorCodes::MultipleErrorsOccurred, msg.str()});
}

/**
 * This is the core function which aggregates all the results of a write operation on multiple
 * shards and updates the write operation's state.
 */
void WriteOp::_updateOpState() {
    std::vector<ChildWriteOp const*> childErrors;

    bool isRetryError = true;
    for (const auto& childOp : _childOps) {
        // Don't do anything till we have all the info
        if (childOp.state != WriteOpState_Completed && childOp.state != WriteOpState_Error) {
            return;
        }

        if (childOp.state == WriteOpState_Error) {
            childErrors.push_back(&childOp);

            // Any non-retry error aborts all
            if (_inTxn || !isRetryErrCode(childOp.error->toStatus().code())) {
                isRetryError = false;
            }
        }
    }

    if (!childErrors.empty() && isRetryError) {
        // Since we're using broadcast mode for multi-shard writes, which cannot SCE
        invariant(childErrors.size() == 1u);
        _state = WriteOpState_Ready;
    } else if (!childErrors.empty()) {
        _error.reset(new WriteErrorDetail);
        combineOpErrors(childErrors, _error.get());
        _state = WriteOpState_Error;
    } else {
        _state = WriteOpState_Completed;
    }

    invariant(_state != WriteOpState_Pending);
    _childOps.clear();
}

void WriteOp::cancelWrites(const WriteErrorDetail* why) {
    invariant(_state == WriteOpState_Pending || _state == WriteOpState_Ready);

    for (auto& childOp : _childOps) {
        if (childOp.state == WriteOpState_Pending) {
            childOp.endpoint.reset(new ShardEndpoint(childOp.pendingWrite->endpoint));
            if (why) {
                childOp.error.reset(new WriteErrorDetail);
                why->cloneTo(childOp.error.get());
            }

            childOp.state = WriteOpState_Cancelled;
        }
    }

    _state = WriteOpState_Ready;
    _childOps.clear();
}

void WriteOp::noteWriteComplete(const TargetedWrite& targetedWrite) {
    const WriteOpRef& ref = targetedWrite.writeOpRef;
    auto& childOp = _childOps[ref.second];

    childOp.pendingWrite = NULL;
    childOp.endpoint.reset(new ShardEndpoint(targetedWrite.endpoint));
    childOp.state = WriteOpState_Completed;
    _updateOpState();
}

void WriteOp::noteWriteError(const TargetedWrite& targetedWrite, const WriteErrorDetail& error) {
    const WriteOpRef& ref = targetedWrite.writeOpRef;
    auto& childOp = _childOps[ref.second];

    childOp.pendingWrite = NULL;
    childOp.endpoint.reset(new ShardEndpoint(targetedWrite.endpoint));
    childOp.error.reset(new WriteErrorDetail);
    error.cloneTo(childOp.error.get());
    dassert(ref.first == _itemRef.getItemIndex());
    childOp.error->setIndex(_itemRef.getItemIndex());
    childOp.state = WriteOpState_Error;
    _updateOpState();
}

void WriteOp::setOpError(const WriteErrorDetail& error) {
    dassert(_state == WriteOpState_Ready);
    _error.reset(new WriteErrorDetail);
    error.cloneTo(_error.get());
    _error->setIndex(_itemRef.getItemIndex());
    _state = WriteOpState_Error;
    // No need to updateOpState, set directly
}

}  // namespace mongo
