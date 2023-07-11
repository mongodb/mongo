/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/preprocessor/control/iif.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/client/read_preference.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_operation_source.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/bulk_write.h"
#include "mongo/db/commands/bulk_write_common.h"
#include "mongo/db/commands/bulk_write_crud_op.h"
#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/db/commands/bulk_write_parser.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_writes_common.h"
#include "mongo/db/ops/single_write_result_gen.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/update_result.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_exec_util.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeBulkWritePerformsUpdate);
MONGO_FAIL_POINT_DEFINE(hangBetweenProcessingBulkWriteOps);

/**
 * BulkWriteReplies maintains the BulkWriteReplyItems and provides an interface to add either
 * Insert or Update/Delete replies.
 */
class BulkWriteReplies {
public:
    BulkWriteReplies() = delete;
    BulkWriteReplies(const BulkWriteCommandRequest& request, int capacity)
        : _req(request), _replies() {
        _replies.reserve(capacity);
    }

    void addInsertReplies(OperationContext* opCtx,
                          size_t firstOpIdx,
                          write_ops_exec::WriteResult& writes) {
        invariant(!writes.results.empty());

        // Copy over retriedStmtIds.
        for (auto& stmtId : writes.retriedStmtIds) {
            _retriedStmtIds.emplace_back(stmtId);
        }

        for (size_t i = 0; i < writes.results.size(); ++i) {
            auto idx = firstOpIdx + i;
            if (auto error = write_ops_exec::generateError(
                    opCtx, writes.results[i].getStatus(), idx, _numErrors)) {
                auto replyItem = BulkWriteReplyItem(idx, error.get().getStatus());
                _replies.emplace_back(replyItem);
                _numErrors++;
            } else {
                auto replyItem = BulkWriteReplyItem(idx);
                replyItem.setN(writes.results[i].getValue().getN());
                _replies.emplace_back(replyItem);
            }
        }
    }

    void addUpdateReply(size_t currentOpIdx,
                        int numMatched,
                        int numDocsModified,
                        const boost::optional<IDLAnyTypeOwned>& upserted,
                        const boost::optional<BSONObj>& value,
                        const boost::optional<int32_t>& stmtId) {
        auto replyItem = BulkWriteReplyItem(currentOpIdx);
        replyItem.setNModified(numDocsModified);
        if (upserted.has_value()) {
            replyItem.setUpserted(write_ops::Upserted(0, upserted.value()));
            replyItem.setN(1);
        } else {
            replyItem.setN(numMatched);
        }

        if (value) {
            replyItem.setValue(value);
        }

        if (stmtId) {
            _retriedStmtIds.emplace_back(*stmtId);
        }

        _replies.emplace_back(replyItem);
    }

    void addUpdateReply(size_t currentOpIdx,
                        const UpdateResult& result,
                        const boost::optional<BSONObj>& value,
                        const boost::optional<int32_t>& stmtId) {
        boost::optional<IDLAnyTypeOwned> upserted;
        if (!result.upsertedId.isEmpty()) {
            upserted = IDLAnyTypeOwned(result.upsertedId.firstElement());
        }
        addUpdateReply(
            currentOpIdx, result.numMatched, result.numDocsModified, upserted, value, stmtId);
    }


    void addDeleteReply(size_t currentOpIdx,
                        long long nDeleted,
                        const boost::optional<BSONObj>& value,
                        const boost::optional<int32_t>& stmtId) {
        auto replyItem = BulkWriteReplyItem(currentOpIdx);
        replyItem.setN(nDeleted);

        if (value) {
            replyItem.setValue(value);
        }

        if (stmtId) {
            _retriedStmtIds.emplace_back(*stmtId);
        }

        _replies.emplace_back(replyItem);
    }

    void addUpdateErrorReply(OperationContext* opCtx, size_t currentOpIdx, const Status& status) {
        auto replyItem = BulkWriteReplyItem(currentOpIdx);
        replyItem.setNModified(0);
        addErrorReply(opCtx, replyItem, status);
    }

    void addErrorReply(OperationContext* opCtx, size_t currentOpIdx, const Status& status) {
        auto replyItem = BulkWriteReplyItem(currentOpIdx);
        addErrorReply(opCtx, replyItem, status);
    }

    void addErrorReply(OperationContext* opCtx,
                       BulkWriteReplyItem& replyItem,
                       const Status& status) {
        auto error = write_ops_exec::generateError(opCtx, status, replyItem.getIdx(), _numErrors);
        invariant(error);
        replyItem.setStatus(error.get().getStatus());
        replyItem.setOk(status.isOK() ? 1.0 : 0.0);
        replyItem.setN(0);
        _replies.emplace_back(replyItem);
        _numErrors++;
    }

    std::vector<BulkWriteReplyItem>& getReplies() {
        return _replies;
    }

    std::vector<int>& getRetriedStmtIds() {
        return _retriedStmtIds;
    }

    int getNumErrors() {
        return _numErrors;
    }

private:
    const BulkWriteCommandRequest& _req;
    std::vector<BulkWriteReplyItem> _replies;
    std::vector<int32_t> _retriedStmtIds;
    /// The number of error replies contained in _replies.
    int _numErrors = 0;
};

/**
 * Class representing an InsertBatch. Maintains a reference to the request and a callback function
 * which gets passed the replies from the insert statements being executed.
 */
class InsertBatch {
public:
    InsertBatch() = delete;
    InsertBatch(const BulkWriteCommandRequest& request,
                int capacity,
                BulkWriteReplies& responses,
                write_ops_exec::LastOpFixer& lastOpFixer)
        : _req(request),
          _responses(responses),
          _lastOpFixer(lastOpFixer),
          _currentNs(),
          _batch(),
          _firstOpIdx() {
        _batch.reserve(capacity);
    }

    bool empty() const {
        return _batch.empty();
    }

    void addRetryableWriteResult(OperationContext* opCtx, size_t idx, int32_t stmtId) {
        write_ops_exec::WriteResult out;
        SingleWriteResult res;
        res.setN(1);
        res.setNModified(0);
        out.retriedStmtIds.push_back(stmtId);
        out.results.emplace_back(res);

        _responses.addInsertReplies(opCtx, idx, out);
    }

    // Return true if the insert was done by FLE.
    // FLE skips inserts with no encrypted fields, in which case the caller of this method
    // is expected to fallback to its non-FLE code path.
    bool attemptProcessFLEInsert(OperationContext* opCtx, write_ops_exec::WriteResult& out) {
        CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation = true;

        // For BulkWrite, re-entry is un-expected.
        invariant(!_currentNs.getEncryptionInformation()->getCrudProcessed().value_or(false));

        std::vector<mongo::BSONObj> documents;
        std::transform(_batch.cbegin(),
                       _batch.cend(),
                       std::back_inserter(documents),
                       [](const InsertStatement& insert) { return insert.doc; });

        write_ops::InsertCommandRequest request(_currentNs.getNs(), documents);
        auto& requestBase = request.getWriteCommandRequestBase();
        requestBase.setEncryptionInformation(_currentNs.getEncryptionInformation());
        requestBase.setOrdered(_req.getOrdered());

        write_ops::InsertCommandReply insertReply;

        FLEBatchResult batchResult = processFLEInsert(opCtx, request, &insertReply);

        if (batchResult == FLEBatchResult::kProcessed) {
            size_t inserted = static_cast<size_t>(insertReply.getN());

            SingleWriteResult result;
            result.setN(1);

            if (documents.size() == inserted) {
                invariant(!insertReply.getWriteErrors().has_value());
                out.results.reserve(inserted);
                std::fill_n(std::back_inserter(out.results), inserted, std::move(result));
            } else {
                invariant(insertReply.getWriteErrors().has_value());
                const auto& errors = insertReply.getWriteErrors().value();

                out.results.reserve(inserted + errors.size());
                std::fill_n(
                    std::back_inserter(out.results), inserted + errors.size(), std::move(result));

                for (const auto& error : errors) {
                    out.results[error.getIndex()] = error.getStatus();
                }

                if (_req.getOrdered()) {
                    out.canContinue = false;
                }
            }

            if (insertReply.getRetriedStmtIds().has_value()) {
                out.retriedStmtIds = insertReply.getRetriedStmtIds().value();
            }
            return true;
        }
        return false;
    }

    // Returns true if the bulkWrite operation can continue and false if it should stop.
    bool flush(OperationContext* opCtx) {
        if (empty()) {
            return true;
        }

        invariant(_firstOpIdx);
        invariant(_isDifferentFromSavedNamespace(NamespaceInfoEntry()));

        write_ops_exec::WriteResult out;
        auto size = _batch.size();
        out.results.reserve(size);

        bool insertedByFLE = false;
        if (_currentNs.getEncryptionInformation().has_value()) {
            insertedByFLE = attemptProcessFLEInsert(opCtx, out);

            if (!insertedByFLE) {
                // It is unexpected for processFLEInsert (inside attemptProcessFLEInsert)
                // to return kNotProcessed for multiple documents. In the case of retyrable write
                // with FLE, we have to fallthrough to our normal code path below
                // on !insertedByFLE, but we are past the point where that code path normally checks
                // for checkStatementExecutedNoOplogEntryFetch (in handleInsertOp).
                invariant(_batch.size() == 1);

                auto txnParticipant = TransactionParticipant::get(opCtx);
                invariant(_batch[0].stmtIds.size() == 1);
                if (opCtx->isRetryableWrite() &&
                    txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx,
                                                                           _batch[0].stmtIds[0])) {
                    RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
                    addRetryableWriteResult(opCtx, _firstOpIdx.get(), _batch[0].stmtIds[0]);
                    _batch.clear();
                    _currentNs = NamespaceInfoEntry();
                    _firstOpIdx = boost::none;
                    return out.canContinue;
                }
            }
        }

        if (!insertedByFLE) {
            out.canContinue =
                write_ops_exec::insertBatchAndHandleErrors(opCtx,
                                                           _currentNs.getNs(),
                                                           _currentNs.getCollectionUUID(),
                                                           _req.getOrdered(),
                                                           _batch,
                                                           &_lastOpFixer,
                                                           &out,
                                                           OperationSource::kStandard);
        }

        _batch.clear();
        _responses.addInsertReplies(opCtx, _firstOpIdx.get(), out);
        _currentNs = NamespaceInfoEntry();
        _firstOpIdx = boost::none;

        return out.canContinue;
    }

    // Returns true if add was successful and did not encounter errors. Any responses
    // (including errors) are handled by this function and do not need to be explicitly written
    // by the caller.
    bool addToBatch(OperationContext* opCtx,
                    size_t currentOpIdx,
                    int32_t stmtId,
                    const NamespaceInfoEntry& nsInfo,
                    const BSONObj& op) {
        // If this is a different namespace we have to flush the current batch.
        if (_isDifferentFromSavedNamespace(nsInfo)) {
            // Write the current batch since we have a different namespace to process.
            if (!flush(opCtx)) {
                return false;
            }
            invariant(empty());
            _currentNs = nsInfo;
            _firstOpIdx = currentOpIdx;
        }

        if (_addInsertToBatch(opCtx, stmtId, op)) {
            if (!flush(opCtx)) {
                return false;
            }
        }
        return true;
    }

private:
    const BulkWriteCommandRequest& _req;
    BulkWriteReplies& _responses;
    write_ops_exec::LastOpFixer& _lastOpFixer;
    NamespaceInfoEntry _currentNs;
    std::vector<InsertStatement> _batch;
    boost::optional<int> _firstOpIdx;

    // Return true when the batch is at maximum capacity and should be flushed.
    bool _addInsertToBatch(OperationContext* opCtx, const int stmtId, const BSONObj& toInsert) {
        _batch.emplace_back(stmtId, toInsert);

        return _batch.size() == _batch.capacity();
    }

    bool _isDifferentFromSavedNamespace(const NamespaceInfoEntry& newNs) const {
        if (newNs.getNs() == _currentNs.getNs()) {
            return newNs.getCollectionUUID() != _currentNs.getCollectionUUID();
        }
        return true;
    }
};

void finishCurOp(OperationContext* opCtx, CurOp* curOp) {
    try {
        curOp->done();
        auto executionTimeMicros = duration_cast<Microseconds>(curOp->elapsedTimeExcludingPauses());
        curOp->debug().additiveMetrics.executionTime = executionTimeMicros;

        recordCurOpMetrics(opCtx);
        Top::get(opCtx->getServiceContext())
            .record(opCtx,
                    curOp->getNSS(),
                    curOp->getLogicalOp(),
                    Top::LockType::WriteLocked,
                    durationCount<Microseconds>(curOp->elapsedTimeExcludingPauses()),
                    curOp->isCommand(),
                    curOp->getReadWriteType());

        if (!curOp->debug().errInfo.isOK()) {
            LOGV2_DEBUG(
                7276600,
                3,
                "Caught Assertion in bulkWrite finishCurOp. Op: {operation}, error: {error}",
                "Caught Assertion in bulkWrite finishCurOp",
                "operation"_attr = redact(logicalOpToString(curOp->getLogicalOp())),
                "error"_attr = curOp->debug().errInfo.toString());
        }

        // Mark the op as complete, and log it if appropriate.
        curOp->completeAndLogOperation(MONGO_LOGV2_DEFAULT_COMPONENT,
                                       CollectionCatalog::get(opCtx)
                                           ->getDatabaseProfileSettings(curOp->getNSS().dbName())
                                           .filter);
    } catch (const DBException& ex) {
        // We need to ignore all errors here. We don't want a successful op to fail because of a
        // failure to record stats. We also don't want to replace the error reported for an op that
        // is failing.
        LOGV2(7276601,
              "Ignoring error from bulkWrite finishCurOp: {error}",
              "Ignoring error from bulkWrite finishCurOp",
              "error"_attr = redact(ex));
    }
}

std::tuple<long long, boost::optional<BSONObj>> getRetryResultForDelete(
    OperationContext* opCtx,
    const NamespaceString& nsString,
    const boost::optional<repl::OplogEntry>& entry) {
    // Use a SideTransactionBlock since 'parseOplogEntryForFindAndModify' might need
    // to fetch a pre/post image from the oplog and if this is a retry inside an
    // in-progress retryable internal transaction, this 'opCtx' would have an active
    // WriteUnitOfWork and it is illegal to read the the oplog when there is an
    // active WriteUnitOfWork.
    TransactionParticipant::SideTransactionBlock sideTxn(opCtx);

    // Need to create a dummy FindAndModifyRequest to use to parse the oplog entry
    // using existing helpers.
    // The helper only checks a couple of booleans for validation so we do not need
    // to copy over all fields.
    auto findAndModifyReq = write_ops::FindAndModifyCommandRequest(nsString);
    findAndModifyReq.setRemove(true);
    findAndModifyReq.setNew(false);

    auto findAndModifyReply = parseOplogEntryForFindAndModify(opCtx, findAndModifyReq, *entry);

    return std::make_tuple(findAndModifyReply.getLastErrorObject().getNumDocs(),
                           findAndModifyReply.getValue());
}

std::tuple<int /*numMatched*/,
           int /*numDocsModified*/,
           boost::optional<IDLAnyTypeOwned>,
           boost::optional<BSONObj>>
getRetryResultForUpdate(OperationContext* opCtx,
                        const NamespaceString& nsString,
                        const BulkWriteUpdateOp* op,
                        const boost::optional<repl::OplogEntry>& entry) {
    // If 'return' is not specified then fetch this statement using the normal update
    // helpers. If 'return' is specified we need to use the findAndModify helpers.
    // findAndModify helpers do not support Updates executed with a none return so this
    // split is necessary.
    if (!op->getReturn()) {
        auto writeResult = parseOplogEntryForUpdate(*entry);

        // Since multi cannot be true for retryable writes numDocsModified + upserted should be 1
        tassert(ErrorCodes::BadValue,
                "bulkWrite retryable update must only modify one document",
                writeResult.getNModified() + (writeResult.getUpsertedId().isEmpty() ? 0 : 1) == 1);

        boost::optional<IDLAnyTypeOwned> upserted;
        if (!writeResult.getUpsertedId().isEmpty()) {
            upserted = IDLAnyTypeOwned(writeResult.getUpsertedId().firstElement());
        }

        // We only care about the values of numDocsModified and upserted from the Update
        // result.
        return std::make_tuple(
            writeResult.getN(), writeResult.getNModified(), upserted, boost::none);
    }

    // Use a SideTransactionBlock since 'parseOplogEntryForFindAndModify' might need
    // to fetch a pre/post image from the oplog and if this is a retry inside an
    // in-progress retryable internal transaction, this 'opCtx' would have an active
    // WriteUnitOfWork and it is illegal to read the the oplog when there is an
    // active WriteUnitOfWork.
    TransactionParticipant::SideTransactionBlock sideTxn(opCtx);

    // Need to create a dummy FindAndModifyRequest to use to parse the oplog entry
    // using existing helpers.
    // The helper only checks a couple of booleans for validation so we do not need
    // to copy over all fields.
    auto findAndModifyReq = write_ops::FindAndModifyCommandRequest(nsString);
    findAndModifyReq.setUpsert(op->getUpsert());
    findAndModifyReq.setRemove(false);
    if (op->getReturn() && op->getReturn().get() == "post") {
        findAndModifyReq.setNew(true);
    }

    auto findAndModifyReply = parseOplogEntryForFindAndModify(opCtx, findAndModifyReq, *entry);

    int numDocsModified = findAndModifyReply.getLastErrorObject().getNumDocs();

    boost::optional<IDLAnyTypeOwned> upserted =
        findAndModifyReply.getLastErrorObject().getUpserted();
    if (upserted.has_value()) {
        // An 'upserted' doc does not count as a modified doc but counts in the
        // numDocs total. Since numDocs is either 1 or 0 it should be 0 here.
        numDocsModified = 0;
    }

    // Since multi cannot be true for retryable writes numDocsModified + upserted should be 1
    tassert(ErrorCodes::BadValue,
            "bulkWrite retryable update must only modify one document",
            numDocsModified + (upserted.has_value() ? 1 : 0) == 1);

    // We only care about the values of numDocsModified and upserted from the Update
    // result.
    return std::make_tuple(findAndModifyReply.getLastErrorObject().getNumDocs(),
                           numDocsModified,
                           upserted,
                           findAndModifyReply.getValue());
}

bool handleInsertOp(OperationContext* opCtx,
                    const BulkWriteInsertOp* op,
                    const BulkWriteCommandRequest& req,
                    size_t currentOpIdx,
                    BulkWriteReplies& responses,
                    InsertBatch& batch) {
    const auto& nsInfo = req.getNsInfo();
    auto idx = op->getInsert();

    auto stmtId = opCtx->isRetryableWrite() ? bulk_write_common::getStatementId(req, currentOpIdx)
                                            : kUninitializedStmtId;

    auto txnParticipant = TransactionParticipant::get(opCtx);

    // For FLE + RetryableWrite, we let FLE handle stmtIds and retryability, so we skip
    // checkStatementExecutedNoOplogEntryFetch here.
    if (!nsInfo[idx].getEncryptionInformation().has_value() && opCtx->isRetryableWrite() &&
        txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx, stmtId)) {
        if (!batch.flush(opCtx)) {
            return false;
        }

        RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
        batch.addRetryableWriteResult(opCtx, currentOpIdx, stmtId);
        return true;
    }

    bool containsDotsAndDollarsField = false;
    auto fixedDoc = fixDocumentForInsert(opCtx, op->getDocument(), &containsDotsAndDollarsField);

    if (!fixedDoc.isOK()) {
        if (!batch.flush(opCtx)) {
            return false;
        }

        // Convert status to DBException to pass to handleError.
        try {
            uassertStatusOK(fixedDoc.getStatus());
            MONGO_UNREACHABLE;
        } catch (const DBException& ex) {
            responses.addErrorReply(opCtx, currentOpIdx, ex.toStatus());
            write_ops_exec::WriteResult out;
            // fixDocumentForInsert can only fail for validation reasons, we only use handleError
            // here to tell us if we are able to continue processing further ops or not.
            return write_ops_exec::handleError(opCtx,
                                               ex,
                                               nsInfo[idx].getNs(),
                                               req.getOrdered(),
                                               false /* isMultiUpdate */,
                                               boost::none /* sampleId */,
                                               &out);
        }
    }

    BSONObj toInsert =
        fixedDoc.getValue().isEmpty() ? op->getDocument() : std::move(fixedDoc.getValue());

    // Normal insert op, add to the batch.
    return batch.addToBatch(opCtx, currentOpIdx, stmtId, nsInfo[idx], toInsert);
}

bool handleUpdateOp(OperationContext* opCtx,
                    CurOp* curOp,
                    const BulkWriteUpdateOp* op,
                    const BulkWriteCommandRequest& req,
                    size_t currentOpIdx,
                    write_ops_exec::LastOpFixer& lastOpFixer,
                    BulkWriteReplies& responses) {
    const auto& nsInfo = req.getNsInfo();
    auto idx = op->getUpdate();
    try {
        if (op->getMulti()) {
            uassert(ErrorCodes::InvalidOptions,
                    "May not specify both multi and return in bulkWrite command.",
                    !op->getReturn());

            uassert(ErrorCodes::InvalidOptions,
                    "Cannot use retryable writes with multi=true",
                    !opCtx->isRetryableWrite());
        }

        if (op->getReturnFields()) {
            uassert(ErrorCodes::InvalidOptions,
                    "Must specify return if returnFields is provided in bulkWrite command.",
                    op->getReturn());
        }

        const NamespaceString& nsString = nsInfo[idx].getNs();
        uassertStatusOK(userAllowedWriteNS(opCtx, nsString));
        OpDebug* opDebug = &curOp->debug();

        doTransactionValidationForWrites(opCtx, nsString);

        auto stmtId = opCtx->isRetryableWrite()
            ? bulk_write_common::getStatementId(req, currentOpIdx)
            : kUninitializedStmtId;
        if (opCtx->isRetryableWrite()) {
            const auto txnParticipant = TransactionParticipant::get(opCtx);
            if (auto entry = txnParticipant.checkStatementExecuted(opCtx, stmtId)) {
                RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();

                auto [numMatched, numDocsModified, upserted, image] =
                    getRetryResultForUpdate(opCtx, nsString, op, entry);

                responses.addUpdateReply(
                    currentOpIdx, numMatched, numDocsModified, upserted, image, stmtId);

                return true;
            }
        }

        const bool inTransaction = opCtx->inMultiDocumentTransaction();

        auto updateRequest = UpdateRequest();
        updateRequest.setNamespaceString(nsString);
        updateRequest.setQuery(op->getFilter());
        updateRequest.setProj(op->getReturnFields().value_or(BSONObj()));
        updateRequest.setUpdateModification(op->getUpdateMods());
        updateRequest.setLegacyRuntimeConstants(Variables::generateRuntimeConstants(opCtx));
        updateRequest.setUpdateConstants(op->getConstants());
        updateRequest.setLetParameters(req.getLet());
        updateRequest.setSort(op->getSort().value_or(BSONObj()));
        updateRequest.setHint(op->getHint());
        updateRequest.setCollation(op->getCollation().value_or(BSONObj()));
        updateRequest.setArrayFilters(op->getArrayFilters().value_or(std::vector<BSONObj>()));
        updateRequest.setUpsert(op->getUpsert());
        if (op->getReturn()) {
            updateRequest.setReturnDocs((op->getReturn().get() == "pre")
                                            ? UpdateRequest::RETURN_OLD
                                            : UpdateRequest::RETURN_NEW);
        } else {
            updateRequest.setReturnDocs(UpdateRequest::RETURN_NONE);
        }
        updateRequest.setMulti(op->getMulti());

        updateRequest.setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);

        // We only execute one update op at a time.
        updateRequest.setStmtIds({stmtId});

        // Although usually the PlanExecutor handles WCE internally, it will throw WCEs when it
        // is executing an update. This is done to ensure that we can always match,
        // modify, and return the document under concurrency, if a matching document exists.
        lastOpFixer.startingOp(nsString);
        return writeConflictRetry(opCtx, "bulkWriteUpdate", nsString, [&] {
            if (MONGO_unlikely(hangBeforeBulkWritePerformsUpdate.shouldFail())) {
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &hangBeforeBulkWritePerformsUpdate, opCtx, "hangBeforeBulkWritePerformsUpdate");
            }

            // Nested retry loop to handle concurrent conflicting upserts with equality
            // match.
            int retryAttempts = 0;
            for (;;) {
                try {
                    boost::optional<BSONObj> docFound;
                    auto result = write_ops_exec::writeConflictRetryUpsert(opCtx,
                                                                           nsString,
                                                                           curOp,
                                                                           opDebug,
                                                                           inTransaction,
                                                                           false,
                                                                           updateRequest.isUpsert(),
                                                                           docFound,
                                                                           updateRequest);
                    lastOpFixer.finishedOpSuccessfully();
                    responses.addUpdateReply(currentOpIdx, result, docFound, boost::none);
                    return true;
                } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
                    auto cq = uassertStatusOK(
                        parseWriteQueryToCQ(opCtx, nullptr /* expCtx */, updateRequest));
                    if (!write_ops_exec::shouldRetryDuplicateKeyException(
                            updateRequest, *cq, *ex.extraInfo<DuplicateKeyErrorInfo>())) {
                        throw;
                    }

                    ++retryAttempts;
                    logAndBackoff(7276500,
                                  ::mongo::logv2::LogComponent::kWrite,
                                  logv2::LogSeverity::Debug(1),
                                  retryAttempts,
                                  "Caught DuplicateKey exception during bulkWrite update",
                                  logAttrs(updateRequest.getNamespaceString()));
                }
            }
        });
    } catch (const DBException& ex) {
        // IncompleteTrasactionHistory should always be command fatal.
        if (ex.code() == ErrorCodes::IncompleteTransactionHistory) {
            throw;
        }
        responses.addUpdateErrorReply(opCtx, currentOpIdx, ex.toStatus());
        write_ops_exec::WriteResult out;
        return write_ops_exec::handleError(
            opCtx, ex, nsInfo[idx].getNs(), req.getOrdered(), op->getMulti(), boost::none, &out);
    }
}

bool handleDeleteOp(OperationContext* opCtx,
                    CurOp* curOp,
                    const BulkWriteDeleteOp* op,
                    const BulkWriteCommandRequest& req,
                    size_t currentOpIdx,
                    write_ops_exec::LastOpFixer& lastOpFixer,
                    BulkWriteReplies& responses) {
    const auto& nsInfo = req.getNsInfo();
    auto idx = op->getDeleteCommand();
    try {
        if (op->getMulti()) {
            uassert(ErrorCodes::InvalidOptions,
                    "May not specify both multi and return in bulkWrite command.",
                    !op->getReturn());

            uassert(ErrorCodes::InvalidOptions,
                    "Cannot use retryable writes with multi=true",
                    !opCtx->isRetryableWrite());
        }

        if (op->getReturnFields()) {
            uassert(ErrorCodes::InvalidOptions,
                    "Must specify return if returnFields is provided in bulkWrite command.",
                    op->getReturn());
        }

        const NamespaceString& nsString = nsInfo[idx].getNs();
        uassertStatusOK(userAllowedWriteNS(opCtx, nsString));
        OpDebug* opDebug = &curOp->debug();

        doTransactionValidationForWrites(opCtx, nsString);

        auto stmtId = opCtx->isRetryableWrite()
            ? bulk_write_common::getStatementId(req, currentOpIdx)
            : kUninitializedStmtId;
        if (opCtx->isRetryableWrite()) {
            const auto txnParticipant = TransactionParticipant::get(opCtx);
            // If 'return' is not specified then we do not need to parse the statement. Since
            // multi:true is not allowed with retryable writes if the statement was executed
            // there will always be 1 document deleted.
            if (!op->getReturn()) {
                if (txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx, stmtId)) {
                    RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
                    responses.addDeleteReply(currentOpIdx, 1, boost::none, stmtId);
                    return true;
                }
            } else {
                if (auto entry = txnParticipant.checkStatementExecuted(opCtx, stmtId)) {
                    RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
                    auto [numDocs, image] = getRetryResultForDelete(opCtx, nsString, entry);
                    responses.addDeleteReply(currentOpIdx, numDocs, image, stmtId);
                    return true;
                }
            }
        }

        auto deleteRequest = DeleteRequest();
        deleteRequest.setNsString(nsString);
        deleteRequest.setQuery(op->getFilter());
        deleteRequest.setProj(op->getReturnFields().value_or(BSONObj()));
        deleteRequest.setLegacyRuntimeConstants(Variables::generateRuntimeConstants(opCtx));
        deleteRequest.setLet(req.getLet());
        deleteRequest.setSort(op->getSort().value_or(BSONObj()));
        deleteRequest.setHint(op->getHint());
        deleteRequest.setCollation(op->getCollation().value_or(BSONObj()));
        deleteRequest.setMulti(op->getMulti());
        deleteRequest.setReturnDeleted(op->getReturn());
        deleteRequest.setIsExplain(false);

        deleteRequest.setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);

        deleteRequest.setStmtId(stmtId);

        const bool inTransaction = opCtx->inMultiDocumentTransaction();
        lastOpFixer.startingOp(nsString);
        return writeConflictRetry(opCtx, "bulkWriteDelete", nsString, [&] {
            boost::optional<BSONObj> docFound;
            auto nDeleted = write_ops_exec::writeConflictRetryRemove(
                opCtx, nsString, deleteRequest, curOp, opDebug, inTransaction, docFound);
            lastOpFixer.finishedOpSuccessfully();
            responses.addDeleteReply(currentOpIdx, nDeleted, docFound, boost::none);
            return true;
        });
    } catch (const DBException& ex) {
        // IncompleteTrasactionHistory should always be command fatal.
        if (ex.code() == ErrorCodes::IncompleteTransactionHistory) {
            throw;
        }
        responses.addErrorReply(opCtx, currentOpIdx, ex.toStatus());
        write_ops_exec::WriteResult out;
        return write_ops_exec::handleError(
            opCtx, ex, nsInfo[idx].getNs(), req.getOrdered(), false, boost::none, &out);
    }
}

class BulkWriteCmd : public BulkWriteCmdVersion1Gen<BulkWriteCmd> {
public:
    bool adminOnly() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    ReadWriteType getReadWriteType() const final {
        return Command::ReadWriteType::kWrite;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    bool shouldAffectCommandCounter() const final {
        return false;
    }

    std::string help() const override {
        return "command to apply inserts, updates and deletes in bulk";
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        Reply typedRun(OperationContext* opCtx) final {
            uassert(
                ErrorCodes::CommandNotSupported,
                "BulkWrite may not be run without featureFlagBulkWriteCommand enabled",
                gFeatureFlagBulkWriteCommand.isEnabled(serverGlobalParams.featureCompatibility));

            auto& req = request();

            bulk_write_common::validateRequest(req, opCtx->isRetryableWrite());

            // Apply all of the write operations.
            auto [replies, retriedStmtIds, numErrors] = bulk_write::performWrites(opCtx, req);

            return _populateCursorReply(
                opCtx, req, std::move(replies), std::move(retriedStmtIds), numErrors);
        }

        void doCheckAuthorization(OperationContext* opCtx) const final try {
            auto session = AuthorizationSession::get(opCtx->getClient());
            auto privileges = bulk_write_common::getPrivileges(request());

            // Make sure all privileges are authorized.
            uassert(ErrorCodes::Unauthorized,
                    "unauthorized",
                    session->isAuthorizedForPrivileges(privileges));
        } catch (const DBException& ex) {
            NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(ex.code());
            throw;
        }

    private:
        Reply _populateCursorReply(OperationContext* opCtx,
                                   const BulkWriteCommandRequest& req,
                                   bulk_write::BulkWriteReplyItems replies,
                                   bulk_write::RetriedStmtIds retriedStmtIds,
                                   int numErrors) {
            auto reqObj = unparsedRequest().body;
            const NamespaceString cursorNss =
                NamespaceString::makeBulkWriteNSS(req.getDollarTenant());
            auto expCtx = make_intrusive<ExpressionContext>(
                opCtx, std::unique_ptr<CollatorInterface>(nullptr), ns());

            std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
            auto ws = std::make_unique<WorkingSet>();
            auto root = std::make_unique<QueuedDataStage>(expCtx.get(), ws.get());

            for (auto& reply : replies) {
                WorkingSetID id = ws->allocate();
                WorkingSetMember* member = ws->get(id);
                member->keyData.clear();
                member->recordId = RecordId();
                member->resetDocument(SnapshotId(), reply.toBSON());
                member->transitionToOwnedObj();
                root->pushBack(id);
            }

            exec = uassertStatusOK(
                plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(root),
                                            &CollectionPtr::null,
                                            PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                            false, /* whether owned BSON must be returned */
                                            cursorNss));


            long long batchSize = std::numeric_limits<long long>::max();
            if (req.getCursor() && req.getCursor()->getBatchSize()) {
                batchSize = *req.getCursor()->getBatchSize();
            }

            size_t numRepliesInFirstBatch = 0;
            FindCommon::BSONArrayResponseSizeTracker responseSizeTracker;
            for (long long objCount = 0; objCount < batchSize; objCount++) {
                BSONObj nextDoc;
                PlanExecutor::ExecState state = exec->getNext(&nextDoc, nullptr);
                if (state == PlanExecutor::IS_EOF) {
                    break;
                }
                invariant(state == PlanExecutor::ADVANCED);

                // If we can't fit this result inside the current batch, then we stash it for
                // later.
                if (!responseSizeTracker.haveSpaceForNext(nextDoc)) {
                    exec->stashResult(nextDoc);
                    break;
                }

                numRepliesInFirstBatch++;
                responseSizeTracker.add(nextDoc);
            }
            CurOp::get(opCtx)->setEndOfOpMetrics(numRepliesInFirstBatch);
            if (exec->isEOF()) {
                invariant(numRepliesInFirstBatch == replies.size());
                auto reply = BulkWriteCommandReply(
                    BulkWriteCommandResponseCursor(
                        0, std::vector<BulkWriteReplyItem>(std::move(replies))),
                    numErrors);
                if (!retriedStmtIds.empty()) {
                    reply.setRetriedStmtIds(std::move(retriedStmtIds));
                }

                setElectionIdandOpTime(opCtx, reply);

                return reply;
            }

            exec->saveState();
            exec->detachFromOperationContext();

            auto pinnedCursor = CursorManager::get(opCtx)->registerCursor(
                opCtx,
                {std::move(exec),
                 cursorNss,
                 AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
                 APIParameters::get(opCtx),
                 opCtx->getWriteConcern(),
                 repl::ReadConcernArgs::get(opCtx),
                 ReadPreferenceSetting::get(opCtx),
                 reqObj,
                 bulk_write_common::getPrivileges(req)});
            auto cursorId = pinnedCursor.getCursor()->cursorid();

            pinnedCursor->incNBatches();
            pinnedCursor->incNReturnedSoFar(replies.size());

            replies.resize(numRepliesInFirstBatch);
            auto reply = BulkWriteCommandReply(
                BulkWriteCommandResponseCursor(cursorId,
                                               std::vector<BulkWriteReplyItem>(std::move(replies))),
                numErrors);
            if (!retriedStmtIds.empty()) {
                reply.setRetriedStmtIds(std::move(retriedStmtIds));
            }

            setElectionIdandOpTime(opCtx, reply);

            return reply;
        }

        void setElectionIdandOpTime(OperationContext* opCtx, BulkWriteCommandReply& reply) {
            // Undocumented repl fields that mongos depends on.
            auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
            const auto replMode = replCoord->getReplicationMode();
            if (replMode != repl::ReplicationCoordinator::modeNone) {
                reply.setOpTime(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp());
                reply.setElectionId(replCoord->getElectionId());
            }
        }
    };

} bulkWriteCmd;

}  // namespace

namespace bulk_write {

BulkWriteReply performWrites(OperationContext* opCtx, const BulkWriteCommandRequest& req) {
    const auto& ops = req.getOps();
    const auto& bypassDocumentValidation = req.getBypassDocumentValidation();

    DisableDocumentSchemaValidationIfTrue docSchemaValidationDisabler(opCtx,
                                                                      bypassDocumentValidation);

    DisableSafeContentValidationIfTrue safeContentValidationDisabler(
        opCtx, bypassDocumentValidation, false);

    auto responses = BulkWriteReplies(req, ops.size());

    // Create a current insert batch.
    const size_t maxBatchSize = internalInsertMaxBatchSize.load();
    write_ops_exec::LastOpFixer lastOpFixer(opCtx);
    auto batch = InsertBatch(req, std::min(ops.size(), maxBatchSize), responses, lastOpFixer);

    size_t idx = 0;

    auto curOp = CurOp::get(opCtx);

    ON_BLOCK_EXIT([&] {
        if (curOp) {
            finishCurOp(opCtx, &*curOp);
        }

        const auto& retriedStmtIds = responses.getRetriedStmtIds();
        // If any statements were retried then incremement command counter.
        if (!retriedStmtIds.empty()) {
            RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
        }
    });

    bool hasEncryptionInformation = false;

    // Tell mongod what the shard and database versions are. This will cause writes to fail in
    // case there is a mismatch in the mongos request provided versions and the local (shard's)
    // understanding of the version.
    for (const auto& nsInfo : req.getNsInfo()) {
        // TODO (SERVER-72767, SERVER-72804, SERVER-72805): Support timeseries collections.
        OperationShardingState::setShardRole(
            opCtx, nsInfo.getNs(), nsInfo.getShardVersion(), nsInfo.getDatabaseVersion());

        if (nsInfo.getEncryptionInformation().has_value()) {
            hasEncryptionInformation = true;
        }
    }

    if (hasEncryptionInformation) {
        uassert(ErrorCodes::BadValue,
                "BulkWrite with Queryable Encryption supports only a single namespace.",
                req.getNsInfo().size() == 1);
    }

    for (; idx < ops.size(); ++idx) {
        if (MONGO_unlikely(hangBetweenProcessingBulkWriteOps.shouldFail())) {
            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &hangBetweenProcessingBulkWriteOps, opCtx, "hangBetweenProcessingBulkWriteOps");
        }

        auto op = BulkWriteCRUDOp(ops[idx]);
        auto opType = op.getType();

        if (opType == BulkWriteCRUDOp::kInsert) {
            if (!handleInsertOp(opCtx, op.getInsert(), req, idx, responses, batch)) {
                // Insert write failed can no longer continue.
                break;
            }
        } else if (opType == BulkWriteCRUDOp::kUpdate) {
            // Flush insert ops before handling update ops.
            if (!batch.flush(opCtx)) {
                break;
            }
            if (hasEncryptionInformation) {
                uassert(
                    ErrorCodes::InvalidOptions,
                    "BulkWrite update with Queryable Encryption supports only a single operation.",
                    ops.size() == 1);
            }
            if (!handleUpdateOp(opCtx, curOp, op.getUpdate(), req, idx, lastOpFixer, responses)) {
                // Update write failed can no longer continue.
                break;
            }
        } else {
            // Flush insert ops before handling delete ops.
            if (!batch.flush(opCtx)) {
                break;
            }
            if (hasEncryptionInformation) {
                uassert(
                    ErrorCodes::InvalidOptions,
                    "BulkWrite delete with Queryable Encryption supports only a single operation.",
                    ops.size() == 1);
            }
            if (!handleDeleteOp(opCtx, curOp, op.getDelete(), req, idx, lastOpFixer, responses)) {
                // Delete write failed can no longer continue.
                break;
            }
        }
    }

    // It does not matter if this final flush had errors or not since we finished processing
    // the last op already.
    batch.flush(opCtx);

    invariant(batch.empty());

    return make_tuple(
        responses.getReplies(), responses.getRetriedStmtIds(), responses.getNumErrors());
}

}  // namespace bulk_write
}  // namespace mongo
