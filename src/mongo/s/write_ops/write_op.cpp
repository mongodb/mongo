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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/write_op_helper.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <ostream>
#include <string>

#include <absl/container/flat_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterCompletingWriteWithoutShardKeyWithId);

// Aggregate a bunch of errors for a single op together
write_ops::WriteError combineOpErrors(const std::vector<ChildWriteOp const*>& errOps) {
    auto getStatusCode = [](ChildWriteOp const* item) {
        return item->error->getStatus().code();
    };
    // Special case single response, all errors are the same, or a single non-retryable error
    if (errOps.size() == 1 || write_op_helpers::errorsAllSame(errOps, getStatusCode)) {
        return *errOps.front()->error;
    } else if (write_op_helpers::hasOnlyOneNonRetryableError(errOps, getStatusCode)) {
        auto nonRetryableError =
            write_op_helpers::getFirstNonRetryableError(errOps, getStatusCode)->error;
        tassert(
            10412304, "Expected the erroring child operation to have an error", nonRetryableError);
        return nonRetryableError.value();
    }

    bool skipRetryableErrors = write_op_helpers::hasAnyNonRetryableError(errOps, getStatusCode);

    // Generate the multi-error message below
    std::stringstream msg("multiple errors for op : ");

    bool firstError = true;
    BSONArrayBuilder errB;
    for (std::vector<ChildWriteOp const*>::const_iterator it = errOps.begin(); it != errOps.end();
         ++it) {
        const ChildWriteOp* errOp = *it;
        if (!skipRetryableErrors ||
            !write_op_helpers::isRetryErrCode(errOp->error->getStatus().code())) {
            if (firstError) {
                msg << " :: and :: ";
                firstError = false;
            }
            msg << errOp->error->getStatus().reason();
            errB.append(errOp->error->serialize());
        }
    }

    return write_ops::WriteError(errOps.front()->error->getIndex(),
                                 Status(MultipleErrorsOccurredInfo(errB.arr()), msg.str()));
}

bool isSafeToIgnoreErrorInPartiallyAppliedOp(write_ops::WriteError& error) {
    // UUID mismatch errors are safe to ignore if the actualCollection is null in conjuntion with
    // other successful operations. This is true because it means we wrongly targeted a non-owning
    // shard with the operation and we wouldn't have applied any modifications anyway.
    //
    // Note this is only safe if we're using ShardVersion::IGNORED since we're ignoring any
    // placement concern and broadcasting to all shards.
    return error.getStatus().code() == ErrorCodes::CollectionUUIDMismatch &&
        !error.getStatus().extraInfo<CollectionUUIDMismatchInfo>()->actualCollection();
}
}  // namespace

const BatchItemRef& WriteOp::getWriteItem() const {
    return _itemRef;
}

WriteOpState WriteOp::getWriteState() const {
    return _state;
}

StringData WriteOp::getWriteStateAsString() const {
    switch (_state) {
        case WriteOpState_Ready:
            return "Ready";
        case WriteOpState_Pending:
            return "Pending";
        case WriteOpState_Deferred:
            return "Deferred";
        case WriteOpState_Completed:
            return "Completed";
        case WriteOpState_NoOp:
            return "NoOp";
        case WriteOpState_Error:
            return "Error";
    };
    MONGO_UNREACHABLE;
}

const write_ops::WriteError& WriteOp::getOpError() const {
    dassert(_state == WriteOpState_Error);
    return *_error;
}

bool WriteOp::hasBulkWriteReplyItem() const {
    return _bulkWriteReplyItem != boost::none;
}

BulkWriteReplyItem WriteOp::takeBulkWriteReplyItem() {
    invariant(_state >= WriteOpState_Completed);
    invariant(_bulkWriteReplyItem);
    return std::move(_bulkWriteReplyItem.value());
}

WriteOp::TargetWritesResult WriteOp::targetWrites(OperationContext* opCtx,
                                                  const NSTargeter& targeter,
                                                  bool enableMultiWriteBlockingMigrations) {
    invariant(_childOps.empty());
    const BatchedCommandRequest::BatchType opType = _itemRef.getOpType();
    const bool isInsert = opType == BatchedCommandRequest::BatchType_Insert;
    const bool isUpdate = opType == BatchedCommandRequest::BatchType_Update;
    const bool isDelete = opType == BatchedCommandRequest::BatchType_Delete;
    const bool inTransaction = bool(TransactionRouter::get(opCtx));

    std::vector<ShardEndpoint> endpoints;
    bool useTwoPhaseWriteProtocol = false;
    bool isNonTargetedRetryableWriteWithId = false;

    if (isInsert) {
        endpoints = std::vector{targeter.targetInsert(opCtx, _itemRef.getInsertOp().getDocument())};
    } else if (isUpdate || isDelete) {
        auto targetingResult = isUpdate ? targeter.targetUpdate(opCtx, _itemRef)
                                        : targeter.targetDelete(opCtx, _itemRef);

        endpoints = std::move(targetingResult.endpoints);
        useTwoPhaseWriteProtocol = targetingResult.useTwoPhaseWriteProtocol;
        isNonTargetedRetryableWriteWithId = targetingResult.isNonTargetedRetryableWriteWithId;
    } else {
        MONGO_UNREACHABLE;
    }

    const bool multipleEndpoints = endpoints.size() > 1u;

    const bool isMultiWrite = _itemRef.getMulti();

    // Check if an update or delete requires using a non ordinary writeType. An updateOne
    // or deleteOne necessitates using the two phase write in the case where the query does
    // not contain a shard key or _id to target by.
    //
    // Handle time-series retryable updates using the two phase write protocol only when
    // there is more than one shard that owns chunks.
    WriteType writeType = [&] {
        if (isUpdate || isDelete) {
            if (!isMultiWrite && isNonTargetedRetryableWriteWithId) {
                return WriteType::WithoutShardKeyWithId;
            }
            if (!isMultiWrite && useTwoPhaseWriteProtocol) {
                return WriteType::WithoutShardKeyOrId;
            }
            if (isMultiWrite && enableMultiWriteBlockingMigrations) {
                return WriteType::MultiWriteBlockingMigrations;
            }
            if (isUpdate && targeter.isTrackedTimeSeriesNamespace() && opCtx->isRetryableWrite() &&
                !opCtx->inMultiDocumentTransaction() && !isRawDataOperation(opCtx)) {
                return WriteType::TimeseriesRetryableUpdate;
            }
        }

        return WriteType::Ordinary;
    }();

    // If the op is an update or delete which targets multiple endpoints and 'inTransaction'
    // is false and 'writeType' is "Ordinary" or "WithoutShardKeyWithId", -AND- if either the
    // op is not multi:true or the "onlyTargetDataOwningShardsForMultiWrites" cluster param is
    // not enabled, then we must target all endpoints (since partial results cannot be retried)
    // and for Ordinary writes we must also set 'shardVersion' to IGNORED on all endpoints.
    if ((isUpdate || isDelete) && multipleEndpoints && !inTransaction &&
        (writeType == WriteType::Ordinary || writeType == WriteType::WithoutShardKeyWithId)) {
        // We only need to target all shards (and set 'shardVersion' to IGNORED on all endpoints
        // for Ordinary writes when 'onlyTargetDataOwningShardsForMultiWrites' is false or when
        // 'isMultiWrite' is false.
        //
        // In the case where 'isMultiWrite' is true and 'onlyTargetDataOwningShardsForMultiWrites'
        // is true, StaleConfig errors with partially applied writes will fail with non-retryable
        // QueryPlanKilled, and the user can choose to manually re-run the command if they determine
        // it's safe to do so (i.e. if the operation is idempotent).
        const bool targetAllShards = [&] {
            if (isMultiWrite) {
                // Fetch the "onlyTargetDataOwningShardsForMultiWrites" cluster param.
                auto* clusterParam = ServerParameterSet::getClusterParameterSet()
                                         ->get<ClusterParameterWithStorage<
                                             OnlyTargetDataOwningShardsForMultiWritesParam>>(
                                             "onlyTargetDataOwningShardsForMultiWrites");
                // Return false if cluster param is enabled, otherwise return true.
                return !clusterParam->getValue(boost::none).getEnabled();
            }
            return true;
        }();

        if (targetAllShards && writeType == WriteType::WithoutShardKeyWithId) {
            // For WithoutShardKeyWithId WriteOps running outside of a transaction that need to
            // target more than one endpoint, all shards are targeted.
            //
            // TODO SERVER-101167: For WithoutShardKeyWithId write ops, we should only target the
            // shards that are needed (instead of targeting all shards).
            endpoints = targeter.targetAllShards(opCtx);
        }

        if (targetAllShards && writeType == WriteType::Ordinary) {
            // For Ordinary WriteOps running outside of a transaction that need to target more than
            // one endpoint, all shards are targeted -AND- 'shardVersion' is set to IGNORED on all
            // endpoints. Currently there are two cases where this block of code is reached:
            //   1) multi:true updates/upserts/deletes outside of transaction (where
            //      'isTimeseriesRetryableUpdateOp' and 'enableMultiWriteBlockingMigrations' are
            //      both false)
            //   2) non-retryable or sessionless multi:false non-upsert updates/deletes
            //      that have an _id equality outside of a transaction (where
            //      'isTimeseriesRetryableUpdateOp' is false)
            //
            // TODO SPM-1153: Implement a new approach for multi:true updates/upserts/deletes that
            // does not need set 'shardVersion' to IGNORED and that can target only the relevant
            // shards when 'multipleEndpoints' is true (instead of targeting all shards).
            //
            // TODO SPM-3673: For non-retryable/sessionless multi:false non-upsert updates/deletes
            // that have an _id equality, implement a different approach that doesn't need to set
            // 'shardVersion' to IGNORED and that can target only the relevant shards when
            // 'multipleEndpoints' is true (instead of targeting all shards).
            endpoints = targeter.targetAllShards(opCtx);

            for (auto& endpoint : endpoints) {
                endpoint.shardVersion->setPlacementVersionIgnored();
            }
        }
    }

    // Remove shards from 'endpoints' where the operation was already successful.
    if (!_successfulShardSet.empty()) {
        std::erase_if(endpoints, [&](auto&& e) { return _successfulShardSet.count(e.shardName); });
    }

    TargetWritesResult result;
    result.writeType = writeType;

    // If all operations currently targeted were already successful, then that means that
    // the operation is finished.
    if (endpoints.empty()) {
        _state = WriteOpState_Completed;
        return result;
    }

    const auto targetedSampleId =
        analyze_shard_key::tryGenerateTargetedSampleId(opCtx, targeter.getNS(), opType, endpoints);

    for (auto&& endpoint : endpoints) {
        ItemIndexChildIndexPair ref(_itemRef.getItemIndex(), _childOps.size());

        const auto sampleId = targetedSampleId && targetedSampleId->isFor(endpoint)
            ? boost::make_optional(targetedSampleId->getId())
            : boost::none;

        result.writes.push_back(
            std::make_unique<TargetedWrite>(std::move(endpoint), ref, std::move(sampleId)));

        ChildWriteOp childOp(this);
        childOp.pendingWrite = result.writes.back().get();
        childOp.state = WriteOpState_Pending;

        _childOps.emplace_back(std::move(childOp));
    }

    _state = WriteOpState_Pending;

    return result;
}

size_t WriteOp::getNumTargeted() {
    return _childOps.size();
}

bool WriteOp::hasPendingChildOps() const {
    for (const auto& childOp : _childOps) {
        if (childOp.state == WriteOpState_Ready || childOp.state == WriteOpState_Pending) {
            return true;
        }
    }
    return false;
}

/**
 * This is the core function which aggregates all the results of a write operation on multiple
 * shards and updates the write operation's state.
 */
void WriteOp::_updateOpState(OperationContext* opCtx,
                             boost::optional<bool> markWriteWithoutShardKeyWithIdComplete) {
    std::vector<ChildWriteOp const*> childErrors;
    std::vector<BulkWriteReplyItem const*> childSuccesses;
    // Stores the result of a child update/delete that is in _Deferred state.
    // While we could have many of these, they will always be identical (indicating an update/
    // delete that matched and updated/deleted 0 documents) and thus as an optimization we
    // only save off and use the first one.
    boost::optional<BulkWriteReplyItem const*> deferredChildSuccess;

    bool isRetryError = !_inTxn;
    bool hasErrorThatAbortsTransaction = false;
    bool hasPendingChild = false;
    for (const auto& childOp : _childOps) {
        // If we're not in a transaction, don't do anything until we have all the info.
        if (childOp.state < WriteOpState_Deferred) {
            hasPendingChild = true;

            if (!_inTxn) {
                return;
            }
        }

        if (childOp.state == WriteOpState_Error) {
            childErrors.push_back(&childOp);

            if (!write_op_helpers::isRetryErrCode(childOp.error->getStatus().code())) {
                isRetryError = false;
            }
            if (_inTxn && childOp.error->getStatus().code() != ErrorCodes::WouldChangeOwningShard) {
                hasErrorThatAbortsTransaction = true;
            }
        }

        if (childOp.state == WriteOpState_Completed && childOp.bulkWriteReplyItem.has_value()) {
            childSuccesses.push_back(&childOp.bulkWriteReplyItem.value());
        } else if (childOp.state == WriteOpState_Deferred && !deferredChildSuccess &&
                   childOp.bulkWriteReplyItem.has_value()) {
            deferredChildSuccess = &childOp.bulkWriteReplyItem.value();
        }
    }

    // If there are still pending ops that have not finished yet, don't do anything unless we're
    // running in a transaction and there's an error that requires an immediate state change to
    // facilitate aborting the transaction.
    if (hasPendingChild && !hasErrorThatAbortsTransaction) {
        return;
    }

    // If we already combined replies from a previous round of targeting, we need to make sure to
    // combine that partial result with any new ones. _bulkWriteReplyItem will be overwritten
    // below with a new merged reply combining all of the values in childSuccesses, and so we need
    // to add our existing partial result to childSuccesses to get it merged in too.
    // TODO (SERVER-87809): Remove this condition by de-duplicating the calls to _updateOpState()
    // for WWSKWID writes.
    if (_bulkWriteReplyItem && _writeType != WriteType::WithoutShardKeyWithId) {
        childSuccesses.push_back(&_bulkWriteReplyItem.value());
    }

    if (!childErrors.empty() && isRetryError) {
        if (!childSuccesses.empty()) {
            // Some child operations were successful on some of the shards. We must remember the
            // previous replies before we retry targeting this operation. This is because it is
            // possible to only target shards in _successfulShardSet on retry and as a result, we
            // may transition to Completed immediately after that.
            // Note we do *not* include childDeferredSuccesses here, because staleness errors
            // invalidate previous deferred responses.
            _bulkWriteReplyItem = combineBulkWriteReplyItems(childSuccesses);
        }
        if (_writeType == WriteType::WithoutShardKeyWithId) {
            _incWriteWithoutShardKeyWithIdMetrics(opCtx);
        }
        _state = WriteOpState_Ready;
        _childOps.clear();
    } else if (!childErrors.empty()) {
        _error = combineOpErrors(childErrors);
        if (!childSuccesses.empty()) {
            _bulkWriteReplyItem = combineBulkWriteReplyItems(childSuccesses);
        }

        bool isTargetingAllShardsWithSVIgnored =
            childErrors.front()
                ->endpoint->shardVersion
                .map([&](const auto& sv) { return ShardVersion::isPlacementVersionIgnored(sv); })
                .get_value_or(false);
        // There are errors that are safe to ignore if they were correctly applied to other shards
        // and we're using ShardVersion::IGNORED. They are safe to ignore as they can be interpreted
        // as no-ops if the shard response had been instead a successful result since they wouldn't
        // have modified any data. As a result, we can swallow the errors and treat them as a
        // successful operation.
        if (isTargetingAllShardsWithSVIgnored && isSafeToIgnoreErrorInPartiallyAppliedOp(*_error) &&
            !_successfulShardSet.empty()) {
            if (!hasPendingChild) {
                _error.reset();
                _state = WriteOpState_Completed;
            } else {
                // As this error is acceptable we wait until all other operations finish to take a
                // decision.
                return;
            }
        } else {
            _state = WriteOpState_Error;
        }
    } else {
        // If we made it here, we finished all the child ops and thus this deferred
        // response is now a final response.
        if (markWriteWithoutShardKeyWithIdComplete.value()) {
            if (deferredChildSuccess) {
                childSuccesses.push_back(deferredChildSuccess.value());
            }
            _bulkWriteReplyItem = combineBulkWriteReplyItems(childSuccesses);
            _state = WriteOpState_Completed;
        } else {
            _state = WriteOpState_Deferred;
        }
    }

    invariant(_state != WriteOpState_Pending);
}

void WriteOp::_noteWriteWithoutShardKeyWithIdBatchResponseWithSingleWrite(
    OperationContext* opCtx,
    const TargetedWrite& targetedWrite,
    int n,
    boost::optional<const BulkWriteReplyItem&> bulkWriteReplyItem) {
    const ItemIndexChildIndexPair& ref = targetedWrite.writeOpRef;
    auto& currentChildOp = _childOps[ref.second];
    dassert(n == 0 || n == 1);
    if (n == 0) {
        // Defer the completion of this child WriteOp until later when we are sure that we do not
        // need to retry them due to StaleConfig or StaleDBVersion.
        currentChildOp.state = WriteOpState_Deferred;
        currentChildOp.bulkWriteReplyItem = bulkWriteReplyItem;
        _updateOpState(opCtx);
    } else {
        for (auto& childOp : _childOps) {
            dassert(childOp.parentOp->_writeType == WriteType::WithoutShardKeyWithId);
            if (childOp.state == WriteOpState_Pending &&
                childOp.pendingWrite->writeOpRef != targetedWrite.writeOpRef) {
                childOp.state = WriteOpState_NoOp;
            } else if (childOp.state == WriteOpState_Error) {
                // When n equals 1, the write operation without a shard key with _id
                // equality has successfully occurred on the intended shard. In this case, any
                // errors from other shards can be safely disregarded. These errors will not
                // impact the parent write operation for us or the user.
                LOGV2_DEBUG(8083900,
                            4,
                            "Ignoring write without shard key with id child op error.",
                            "error"_attr = redact(childOp.error->serialize()));
                childOp.state = WriteOpState_Completed;
                childOp.error = boost::none;
            }
        }
        noteWriteComplete(opCtx, targetedWrite, bulkWriteReplyItem);
        if (MONGO_unlikely(hangAfterCompletingWriteWithoutShardKeyWithId.shouldFail())) {
            hangAfterCompletingWriteWithoutShardKeyWithId.pauseWhileSet();
        }
    }
}

void WriteOp::_incWriteWithoutShardKeyWithIdMetrics(OperationContext* opCtx) {
    invariant(_writeType == WriteType::WithoutShardKeyWithId);
    if (_itemRef.getOpType() == BatchedCommandRequest::BatchType_Update) {
        getQueryCounters(opCtx).updateOneWithoutShardKeyWithIdRetryCount.increment(1);
    } else if (_itemRef.getOpType() == BatchedCommandRequest::BatchType_Delete) {
        getQueryCounters(opCtx).deleteOneWithoutShardKeyWithIdRetryCount.increment(1);
    } else {
        MONGO_UNREACHABLE;
    }
}

void WriteOp::resetWriteToReady(OperationContext* opCtx) {
    if (_writeType == WriteType::WithoutShardKeyWithId) {
        // It is possible that one of the child write op received a non-retryable error marking the
        // write op state as WriteOpState_Error. We reset it to ready if we find some other child
        // write op in the same batch returns a retryable error.
        invariant(_state < WriteOpState_Completed || _state == WriteOpState_Error);
        _incWriteWithoutShardKeyWithIdMetrics(opCtx);
    } else {
        invariant(_state == WriteOpState_Pending || _state == WriteOpState_Ready);
    }

    _state = WriteOpState_Ready;
    _childOps.clear();
}

void WriteOp::noteWriteComplete(OperationContext* opCtx,
                                const TargetedWrite& targetedWrite,
                                boost::optional<const BulkWriteReplyItem&> bulkWriteReplyItem) {
    const ItemIndexChildIndexPair& ref = targetedWrite.writeOpRef;
    auto& childOp = _childOps[ref.second];

    _successfulShardSet.emplace(targetedWrite.endpoint.shardName);
    childOp.pendingWrite = nullptr;
    childOp.endpoint.reset(new ShardEndpoint(targetedWrite.endpoint));
    childOp.bulkWriteReplyItem = bulkWriteReplyItem;
    childOp.state = WriteOpState_Completed;
    _updateOpState(opCtx);
}

void WriteOp::noteWriteError(OperationContext* opCtx,
                             const TargetedWrite& targetedWrite,
                             const write_ops::WriteError& error) {
    const ItemIndexChildIndexPair& ref = targetedWrite.writeOpRef;
    auto& childOp = _childOps[ref.second];

    childOp.pendingWrite = nullptr;
    childOp.endpoint.reset(new ShardEndpoint(targetedWrite.endpoint));
    childOp.error = error;
    dassert(ref.first == _itemRef.getItemIndex());
    childOp.error->setIndex(_itemRef.getItemIndex());
    childOp.state = WriteOpState_Error;
    _updateOpState(opCtx);
}

void WriteOp::noteWriteWithoutShardKeyWithIdResponse(
    OperationContext* opCtx,
    const TargetedWrite& targetedWrite,
    int n,
    int batchSize,
    boost::optional<const BulkWriteReplyItem&> bulkWriteReplyItem) {

    if (batchSize == 1) {
        tassert(8346300,
                "BulkWriteReplyItem 'n' value does not match supplied 'n' value",
                !bulkWriteReplyItem || bulkWriteReplyItem->getN() == n);
        _noteWriteWithoutShardKeyWithIdBatchResponseWithSingleWrite(
            opCtx, targetedWrite, n, bulkWriteReplyItem);
        return;
    }
    const ItemIndexChildIndexPair& ref = targetedWrite.writeOpRef;
    auto& currentChildOp = _childOps[ref.second];
    if (_state == WriteOpState::WriteOpState_Deferred ||
        _state == WriteOpState::WriteOpState_Completed) {
        noteWriteComplete(opCtx, targetedWrite, bulkWriteReplyItem);
    } else if (_state == WriteOpState::WriteOpState_Pending) {
        currentChildOp.state = WriteOpState_Deferred;
        currentChildOp.bulkWriteReplyItem = bulkWriteReplyItem;
        _updateOpState(opCtx, false);
        return;
    } else {
        MONGO_UNREACHABLE;
    }

    if (MONGO_unlikely(hangAfterCompletingWriteWithoutShardKeyWithId.shouldFail())) {
        hangAfterCompletingWriteWithoutShardKeyWithId.pauseWhileSet();
    }
}

void WriteOp::setOpComplete(boost::optional<BulkWriteReplyItem> bulkWriteReplyItem) {
    dassert(_state == WriteOpState_Ready);
    _bulkWriteReplyItem = std::move(bulkWriteReplyItem);
    if (_bulkWriteReplyItem) {
        // The reply item will currently have the index for the batch it was sent to a shard with,
        // rather than its index in the client request, so we need to correct it.
        _bulkWriteReplyItem->setIdx(getWriteItem().getItemIndex());
    }
    _state = WriteOpState_Completed;
    // No need to updateOpState, set directly
}

void WriteOp::setOpError(const write_ops::WriteError& error) {
    dassert(_state == WriteOpState_Ready);
    _error = error;
    _error->setIndex(_itemRef.getItemIndex());
    _state = WriteOpState_Error;
    // No need to updateOpState, set directly
}

void WriteOp::setWriteType(WriteType writeType) {
    _writeType = writeType;
}

WriteType WriteOp::getWriteType() const {
    return _writeType;
}

const std::vector<ChildWriteOp>& WriteOp::getChildWriteOps_forTest() const {
    return _childOps;
}

boost::optional<BulkWriteReplyItem> WriteOp::combineBulkWriteReplyItems(
    std::vector<BulkWriteReplyItem const*> replies) {
    if (replies.empty()) {
        return boost::none;
    }

    BulkWriteReplyItem combinedReply;
    combinedReply.setOk(1);
    combinedReply.setIdx(getWriteItem().getItemIndex());

    for (auto reply : replies) {
        if (auto n = reply->getN(); n.has_value()) {
            combinedReply.setN(combinedReply.getN().get_value_or(0) + n.value());
        }
        if (auto nModified = reply->getNModified(); nModified.has_value()) {
            combinedReply.setNModified(combinedReply.getNModified().get_value_or(0) +
                                       nModified.value());
        }

        if (auto upserted = reply->getUpserted(); upserted.has_value()) {
            tassert(7700400,
                    "Unexpectedly got bulkWrite upserted replies from multiple shards for a "
                    "single update operation",
                    !combinedReply.getUpserted().has_value());
            combinedReply.setUpserted(reply->getUpserted());
        }
    }

    return combinedReply;
}

void TargetedWriteBatch::addWrite(std::unique_ptr<TargetedWrite> targetedWrite, int estWriteSize) {
    _writes.push_back(std::move(targetedWrite));
    _estimatedSizeBytes += estWriteSize;
}

}  // namespace mongo
