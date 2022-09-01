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

#include "mongo/s/write_ops/write_op.h"

#include "mongo/s/transaction_router.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

bool isRetryErrCode(int errCode) {
    return errCode == ErrorCodes::StaleConfig || errCode == ErrorCodes::StaleDbVersion ||
        errCode == ErrorCodes::ShardCannotRefreshDueToLocksHeld ||
        errCode == ErrorCodes::TenantMigrationAborted;
}

bool errorsAllSame(const std::vector<ChildWriteOp const*>& errOps) {
    auto errCode = errOps.front()->error->getStatus().code();
    if (std::all_of(++errOps.begin(), errOps.end(), [errCode](const ChildWriteOp* errOp) {
            return errOp->error->getStatus().code() == errCode;
        })) {
        return true;
    }

    return false;
}

// Aggregate a bunch of errors for a single op together
write_ops::WriteError combineOpErrors(const std::vector<ChildWriteOp const*>& errOps) {
    // Special case single response or all errors are the same
    if (errOps.size() == 1 || errorsAllSame(errOps)) {
        return *errOps.front()->error;
    }

    // Generate the multi-error message below
    std::stringstream msg("multiple errors for op : ");

    BSONArrayBuilder errB;
    for (std::vector<ChildWriteOp const*>::const_iterator it = errOps.begin(); it != errOps.end();
         ++it) {
        const ChildWriteOp* errOp = *it;
        if (it != errOps.begin())
            msg << " :: and :: ";
        msg << errOp->error->getStatus().reason();
        errB.append(errOp->error->serialize());
    }

    return write_ops::WriteError(errOps.front()->error->getIndex(),
                                 Status(MultipleErrorsOccurredInfo(errB.arr()), msg.str()));
}

}  // namespace

const BatchItemRef& WriteOp::getWriteItem() const {
    return _itemRef;
}

WriteOpState WriteOp::getWriteState() const {
    return _state;
}

const write_ops::WriteError& WriteOp::getOpError() const {
    dassert(_state == WriteOpState_Error);
    return *_error;
}

void WriteOp::targetWrites(OperationContext* opCtx,
                           const NSTargeter& targeter,
                           std::vector<std::unique_ptr<TargetedWrite>>* targetedWrites) {
    auto endpoints = [&] {
        if (_itemRef.getOpType() == BatchedCommandRequest::BatchType_Insert) {
            return std::vector{targeter.targetInsert(opCtx, _itemRef.getDocument())};
        } else if (_itemRef.getOpType() == BatchedCommandRequest::BatchType_Update) {
            return targeter.targetUpdate(opCtx, _itemRef);
        } else if (_itemRef.getOpType() == BatchedCommandRequest::BatchType_Delete) {
            return targeter.targetDelete(opCtx, _itemRef);
        }
        MONGO_UNREACHABLE;
    }();

    // Unless executing as part of a transaction, if we're targeting more than one endpoint with an
    // update/delete, we have to target everywhere since we cannot currently retry partial results.
    //
    // NOTE: Index inserts are currently specially targeted only at the current collection to avoid
    // creating collections everywhere.
    const bool inTransaction = bool(TransactionRouter::get(opCtx));
    if (endpoints.size() > 1u && !inTransaction) {
        endpoints = targeter.targetAllShards(opCtx);
    }

    for (auto&& endpoint : endpoints) {
        // If the operation was already successfull on that shard, do not repeat it
        if (_successfulShardSet.count(endpoint.shardName))
            continue;

        _childOps.emplace_back(this);

        WriteOpRef ref(_itemRef.getItemIndex(), _childOps.size() - 1);

        // Outside of a transaction, multiple endpoints currently imply no versioning, since we
        // can't retry half a regular multi-write.
        if (endpoints.size() > 1u && !inTransaction) {
            endpoint.shardVersion = ShardVersion::IGNORED();
        }

        targetedWrites->push_back(std::make_unique<TargetedWrite>(std::move(endpoint), ref));

        _childOps.back().pendingWrite = targetedWrites->back().get();
        _childOps.back().state = WriteOpState_Pending;
    }

    // If all operations currently targeted were successful on a previous round we might have 0
    // childOps, that would mean that the operation is finished.
    _state = _childOps.size() ? WriteOpState_Pending : WriteOpState_Completed;
}

size_t WriteOp::getNumTargeted() {
    return _childOps.size();
}

/**
 * This is the core function which aggregates all the results of a write operation on multiple
 * shards and updates the write operation's state.
 */
void WriteOp::_updateOpState() {
    std::vector<ChildWriteOp const*> childErrors;

    bool isRetryError = true;
    bool hasPendingChild = false;
    for (const auto& childOp : _childOps) {
        // Don't do anything till we have all the info. Unless we're in a transaction because
        // we abort aggresively whenever we get an error during a transaction.
        if (childOp.state != WriteOpState_Completed && childOp.state != WriteOpState_Error) {
            hasPendingChild = true;

            if (!_inTxn) {
                return;
            }
        }

        if (childOp.state == WriteOpState_Error) {
            childErrors.push_back(&childOp);

            // Any non-retry error aborts all
            if (_inTxn || !isRetryErrCode(childOp.error->getStatus().code())) {
                isRetryError = false;
            }
        }
    }

    if (!childErrors.empty() && isRetryError) {
        _state = WriteOpState_Ready;
    } else if (!childErrors.empty()) {
        _error = combineOpErrors(childErrors);
        _state = WriteOpState_Error;
    } else if (hasPendingChild && _inTxn) {
        // Return early here since this means that there were no errors while in txn
        // but there are still ops that have not yet finished.
        return;
    } else {
        _state = WriteOpState_Completed;
    }

    invariant(_state != WriteOpState_Pending);
    _childOps.clear();
}

void WriteOp::cancelWrites(const write_ops::WriteError* why) {
    invariant(_state == WriteOpState_Pending || _state == WriteOpState_Ready);

    for (auto& childOp : _childOps) {
        if (childOp.state == WriteOpState_Pending) {
            childOp.endpoint.reset(new ShardEndpoint(childOp.pendingWrite->endpoint));
            if (why)
                childOp.error = *why;
            childOp.state = WriteOpState_Cancelled;
        }
    }

    _state = WriteOpState_Ready;
    _childOps.clear();
}

void WriteOp::noteWriteComplete(const TargetedWrite& targetedWrite) {
    const WriteOpRef& ref = targetedWrite.writeOpRef;
    auto& childOp = _childOps[ref.second];

    _successfulShardSet.emplace(targetedWrite.endpoint.shardName);
    childOp.pendingWrite = nullptr;
    childOp.endpoint.reset(new ShardEndpoint(targetedWrite.endpoint));
    childOp.state = WriteOpState_Completed;
    _updateOpState();
}

void WriteOp::noteWriteError(const TargetedWrite& targetedWrite,
                             const write_ops::WriteError& error) {
    const WriteOpRef& ref = targetedWrite.writeOpRef;
    auto& childOp = _childOps[ref.second];

    childOp.pendingWrite = nullptr;
    childOp.endpoint.reset(new ShardEndpoint(targetedWrite.endpoint));
    childOp.error = error;
    dassert(ref.first == _itemRef.getItemIndex());
    childOp.error->setIndex(_itemRef.getItemIndex());
    childOp.state = WriteOpState_Error;
    _updateOpState();
}

void WriteOp::setOpError(const write_ops::WriteError& error) {
    dassert(_state == WriteOpState_Ready);
    _error = error;
    _error->setIndex(_itemRef.getItemIndex());
    _state = WriteOpState_Error;
    // No need to updateOpState, set directly
}

}  // namespace mongo
