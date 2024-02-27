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
#include "mongo/db/commands/write_commands_common.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/cursor_server_params_gen.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/initialize_operation_session_info.h"
#include "mongo/db/introspect.h"
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
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/process_interface/replica_set_node_process_interface.h"
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
#include "mongo/db/s/analyze_shard_key_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/stats/server_write_concern_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_resources.h"
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
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/grid.h"
#include "mongo/s/query_analysis_sampler_util.h"
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
                    opCtx, writes.results[i].getStatus(), idx, _summaryFields.nErrors)) {
                auto replyItem = BulkWriteReplyItem(idx, error.get().getStatus());
                _addReply(replyItem);
                _summaryFields.nErrors++;
            } else {
                auto replyItem = BulkWriteReplyItem(idx);
                replyItem.setN(writes.results[i].getValue().getN());
                _summaryFields.nInserted += *replyItem.getN();
                _addReply(replyItem);
            }
        }
    }

    void addUpdateReply(OperationContext* opCtx,
                        size_t currentOpIdx,
                        write_ops_exec::WriteResult& writeResult) {
        invariant(writeResult.results.size() == 1);

        // Copy over retriedStmtIds.
        for (auto& stmtId : writeResult.retriedStmtIds) {
            _retriedStmtIds.emplace_back(stmtId);
        }

        if (auto error = write_ops_exec::generateError(
                opCtx, writeResult.results[0].getStatus(), currentOpIdx, _summaryFields.nErrors)) {
            auto replyItem = BulkWriteReplyItem(currentOpIdx, error.get().getStatus());
            _addReply(replyItem);
            _summaryFields.nErrors++;
        } else {
            auto replyItem = BulkWriteReplyItem(currentOpIdx);
            replyItem.setN(writeResult.results[0].getValue().getN());
            replyItem.setNModified(writeResult.results[0].getValue().getNModified());
            _summaryFields.nModified += *replyItem.getNModified();
            if (auto idElement = writeResult.results[0].getValue().getUpsertedId().firstElement()) {
                replyItem.setUpserted(write_ops::Upserted(0, idElement));
                _summaryFields.nUpserted += 1;
            } else {
                _summaryFields.nMatched += *replyItem.getN();
            }
            _addReply(replyItem);
        }
    }

    void addUpdateReply(size_t currentOpIdx,
                        int numMatched,
                        int numDocsModified,
                        const boost::optional<mongo::write_ops::Upserted>& upserted,
                        const boost::optional<int32_t>& stmtId) {
        auto replyItem = BulkWriteReplyItem(currentOpIdx);
        replyItem.setNModified(numDocsModified);
        _summaryFields.nModified += numDocsModified;
        if (upserted.has_value()) {
            replyItem.setUpserted(upserted);
            replyItem.setN(1);
            _summaryFields.nUpserted += 1;
        } else {
            replyItem.setN(numMatched);
            _summaryFields.nMatched += numMatched;
        }

        if (stmtId) {
            _retriedStmtIds.emplace_back(*stmtId);
        }

        _addReply(replyItem);
    }

    void addUpdateReply(size_t currentOpIdx,
                        int numMatched,
                        int numDocsModified,
                        const boost::optional<IDLAnyTypeOwned>& upsertedAnyType,
                        const boost::optional<int32_t>& stmtId) {

        boost::optional<mongo::write_ops::Upserted> upserted;
        if (upsertedAnyType.has_value()) {
            upserted = write_ops::Upserted(0, upsertedAnyType.value());
        }

        addUpdateReply(currentOpIdx, numMatched, numDocsModified, upserted, stmtId);
    }

    void addUpdateReply(size_t currentOpIdx,
                        const UpdateResult& result,
                        const boost::optional<int32_t>& stmtId) {
        boost::optional<IDLAnyTypeOwned> upserted;
        if (!result.upsertedId.isEmpty()) {
            upserted = IDLAnyTypeOwned(result.upsertedId.firstElement());
        }
        addUpdateReply(currentOpIdx, result.numMatched, result.numDocsModified, upserted, stmtId);
    }


    void addDeleteReply(size_t currentOpIdx,
                        long long nDeleted,
                        const boost::optional<int32_t>& stmtId) {
        auto replyItem = BulkWriteReplyItem(currentOpIdx);
        replyItem.setN(nDeleted);
        _summaryFields.nDeleted += nDeleted;

        if (stmtId) {
            _retriedStmtIds.emplace_back(*stmtId);
        }

        _addReply(replyItem);
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
        auto error = write_ops_exec::generateError(
            opCtx, status, replyItem.getIdx(), _summaryFields.nErrors);
        invariant(error);
        replyItem.setStatus(error.get().getStatus());
        replyItem.setOk(status.isOK() ? 1.0 : 0.0);
        replyItem.setN(0);
        _addReply(replyItem);
        _summaryFields.nErrors++;
    }

    const std::vector<BulkWriteReplyItem>& getReplies() const {
        return _replies;
    }

    const std::vector<int>& getRetriedStmtIds() const {
        return _retriedStmtIds;
    }

    bulk_write::SummaryFields getSummaryFields() const {
        return _summaryFields;
    }

    // Approximate Size in bytes.
    int32_t getApproximateSize() const {
        return _approximateSize;
    }

private:
    const BulkWriteCommandRequest& _req;
    std::vector<BulkWriteReplyItem> _replies;
    std::vector<int32_t> _retriedStmtIds;
    bulk_write::SummaryFields _summaryFields;
    int32_t _approximateSize = 0;  // Only accounting for _replies.

    // Helper to keep _approximateSize up to date when appending to _replies.
    void _addReply(const BulkWriteReplyItem& replyItem) {
        bool isError = replyItem.getOk() == 0;

        // if the response is not an error and we want to only return errors
        // then we should not store this reply.
        if (!isError && _req.getErrorsOnly()) {
            return;
        }

        _approximateSize += replyItem.getApproximateSize();
        _replies.emplace_back(replyItem);
    }
};

bool aboveBulkWriteRepliesMaxSize(OperationContext* opCtx,
                                  size_t idx,
                                  BulkWriteReplies& responses) {
    auto bulkWriteRepliesMaxSize = gBulkWriteMaxRepliesSize.loadRelaxed();
    if (responses.getApproximateSize() >= bulkWriteRepliesMaxSize) {
        Status status{ErrorCodes::ExceededMemoryLimit,
                      fmt::format("BulkWrite response size exceeded limit ({} bytes)",
                                  bulkWriteRepliesMaxSize)};
        responses.addErrorReply(opCtx, idx, status);
        return true;
    }
    return false;
}

/*
 * InsertGrouper is a helper class to group consecutive insert operations for the same namespace in
 * a bulkWrite command.
 */
class InsertGrouper {
public:
    InsertGrouper() = delete;
    InsertGrouper(const BulkWriteCommandRequest& request) : _req(request) {}

    bool isEmpty() const {
        return !_firstOpIdx.has_value();
    }

    /*
     * Return true if the insert op is successfully grouped.
     */
    bool group(const BulkWriteInsertOp* op, size_t currentOpIdx) {
        const auto& nsInfo = _req.getNsInfo();
        auto nsIdx = op->getInsert();

        if (!_firstOpIdx.has_value()) {
            // First op in this group.
            invariant(_numOps == 0);
            _firstOpIdx = currentOpIdx;
            _currentNs = nsInfo[nsIdx];
            _numOps = 1;
            return true;
        }

        if (_isDifferentFromSavedNamespace(nsInfo[nsIdx])) {
            // This should be in a new group after flush.
            return false;
        }

        _numOps += 1;
        return true;
    }

    /*
     * Return (firstOpIdx, numOps) for the current insert group.
     * This function should only be called when the InsertGrouper is not empty.
     */
    std::pair<size_t, size_t> getGroupedInsertsAndReset() {
        invariant(_firstOpIdx.has_value());
        size_t firstOpIdx = _firstOpIdx.value();
        size_t numOps = _numOps;

        _currentNs = NamespaceInfoEntry();
        _numOps = 0;
        _firstOpIdx = boost::none;

        return std::make_pair(firstOpIdx, numOps);
    }

private:
    const BulkWriteCommandRequest& _req;
    NamespaceInfoEntry _currentNs;
    boost::optional<size_t> _firstOpIdx = boost::none;
    size_t _numOps = 0;

    bool _isDifferentFromSavedNamespace(const NamespaceInfoEntry& newNs) const {
        if (newNs.getNs() == _currentNs.getNs()) {
            return newNs.getCollectionUUID() != _currentNs.getCollectionUUID();
        }
        return true;
    }
};

// We set logicalOp in CurOp to be 'bulkWrite' so that the 'op' field in the profile output is
// 'bulkWrite' instead of 'insert/update/delete' as normal writes, but for the 'top' command,
// we need to pass in 'insert/update/delete' since 'top' needs to aggregate the usage for each
// write type, hence we need to pass in the 'logicalOp' parameter.
void finishCurOp(OperationContext* opCtx, CurOp* curOp, LogicalOp logicalOp) {
    try {
        curOp->done();
        auto executionTimeMicros = duration_cast<Microseconds>(curOp->elapsedTimeExcludingPauses());
        curOp->debug().additiveMetrics.executionTime = executionTimeMicros;

        recordCurOpMetrics(opCtx);
        Top::get(opCtx->getServiceContext())
            .record(opCtx,
                    curOp->getNSS(),
                    logicalOp,
                    Top::LockType::WriteLocked,
                    durationCount<Microseconds>(curOp->elapsedTimeExcludingPauses()),
                    curOp->isCommand(),
                    curOp->getReadWriteType());

        if (!curOp->debug().errInfo.isOK()) {
            LOGV2_DEBUG(7276600,
                        3,
                        "Caught Assertion in bulkWrite finishCurOp",
                        "operation"_attr = redact(logicalOpToString(curOp->getLogicalOp())),
                        "error"_attr = curOp->debug().errInfo.toString());
        }

        // Mark the op as complete, log it and profile if the op should be sampled for profiling.
        write_ops_exec::logOperationAndProfileIfNeeded(opCtx, curOp);

    } catch (const DBException& ex) {
        // We need to ignore all errors here. We don't want a successful op to fail because of a
        // failure to record stats. We also don't want to replace the error reported for an op that
        // is failing.
        LOGV2(7276601, "Ignoring error from bulkWrite finishCurOp", "error"_attr = redact(ex));
    }
}

BSONObj getInsertOpDesc(const std::vector<BSONObj>& docs, std::int32_t nsIdx) {
    BSONObjBuilder builder;

    builder.append("insert", nsIdx);
    builder.append("documents", docs);

    return builder.obj();
}

void setCurOpInfoAndEnsureStarted(OperationContext* opCtx,
                                  CurOp* curOp,
                                  LogicalOp logicalOp,
                                  const NamespaceInfoEntry& nsEntry,
                                  const BSONObj& opDescription) {
    stdx::lock_guard<Client> lk(*opCtx->getClient());

    // For timeseries operations with the 'isTimeseriesNamespace' field set (i.e. sent from mongos),
    // use the view namespace for logging/profiling purposes.
    curOp->setNS_inlock(nsEntry.getIsTimeseriesNamespace()
                            ? nsEntry.getNs().getTimeseriesViewNamespace()
                            : nsEntry.getNs());
    curOp->setNetworkOp_inlock(NetworkOp::dbBulkWrite);
    curOp->setLogicalOp_inlock(LogicalOp::opBulkWrite);
    curOp->setOpDescription_inlock(opDescription);
    curOp->ensureStarted();

    if (logicalOp == LogicalOp::opInsert) {
        curOp->debug().additiveMetrics.ninserted = 0;
    }
}

void validateNamespaceForWrites(OperationContext* opCtx,
                                int idx,
                                const NamespaceString& ns,
                                std::vector<int>& validatedNamespaces) {
    if (validatedNamespaces[idx] == 1) {
        // Already checked this namespace.
        return;
    }

    uassertStatusOK(userAllowedWriteNS(opCtx, ns));
    doTransactionValidationForWrites(opCtx, ns);
    validatedNamespaces[idx] = 1;
}

std::tuple<int /*numMatched*/, int /*numDocsModified*/, boost::optional<IDLAnyTypeOwned>>
getRetryResultForUpdate(OperationContext* opCtx,
                        const NamespaceString& nsString,
                        const BulkWriteUpdateOp* op,
                        const boost::optional<repl::OplogEntry>& entry) {
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
    return std::make_tuple(writeResult.getN(), writeResult.getNModified(), upserted);
}

std::vector<BSONObj> getConsecutiveInsertDocuments(const BulkWriteCommandRequest& req,
                                                   size_t firstOpIdx,
                                                   size_t numOps) {
    std::vector<BSONObj> documents;
    documents.reserve(numOps);
    const auto& ops = req.getOps();

    for (size_t i = 0; i < numOps; i++) {
        auto idx = firstOpIdx + i;
        auto op = BulkWriteCRUDOp(ops[idx]);
        auto insertOp = op.getInsert();
        invariant(insertOp);
        documents.push_back(insertOp->getDocument());
    }

    return documents;
}

/*
 * Helper function to build an InsertCommandRequest for 'numOps' consecutive insert operations
 * starting from the 'firstOpIdx'-th operation in the bulkWrite request.
 */
write_ops::InsertCommandRequest getConsecutiveInsertRequest(const BulkWriteCommandRequest& req,
                                                            size_t firstOpIdx,
                                                            const std::vector<BSONObj>& docs,
                                                            const NamespaceInfoEntry& nsInfoEntry) {
    size_t numOps = docs.size();

    std::vector<std::int32_t> stmtIds;
    stmtIds.reserve(numOps);
    for (size_t i = 0; i < numOps; i++) {
        auto idx = firstOpIdx + i;
        stmtIds.push_back(bulk_write_common::getStatementId(req, idx));
    }

    write_ops::InsertCommandRequest request =
        bulk_write_common::makeInsertCommandRequestForFLE(docs, req, nsInfoEntry);
    auto& requestBase = request.getWriteCommandRequestBase();
    requestBase.setStmtIds(stmtIds);

    return request;
}

/*
 * Helper function to convert the InsertCommandReply of an insert batch to a WriteResult.
 */
void populateWriteResultWithInsertReply(size_t nDocsToInsert,
                                        bool isOrdered,
                                        const write_ops::InsertCommandReply& insertReply,
                                        write_ops_exec::WriteResult& out) {
    size_t inserted = static_cast<size_t>(insertReply.getN());

    SingleWriteResult result;
    result.setN(1);

    if (nDocsToInsert == inserted) {
        invariant(!insertReply.getWriteErrors().has_value());
        out.results.reserve(inserted);
        std::fill_n(std::back_inserter(out.results), inserted, std::move(result));
    } else {
        invariant(insertReply.getWriteErrors().has_value());
        const auto& errors = insertReply.getWriteErrors().value();

        out.results.reserve(inserted + errors.size());
        std::fill_n(std::back_inserter(out.results), inserted + errors.size(), std::move(result));

        for (const auto& error : errors) {
            out.results[error.getIndex()] = error.getStatus();
        }

        if (isOrdered) {
            out.canContinue = false;
        }
    }

    if (insertReply.getRetriedStmtIds().has_value()) {
        out.retriedStmtIds = insertReply.getRetriedStmtIds().value();
    }
}

/*
 * Helper function to flush FLE insert ops grouped by the insertGrouper.
 * Return true if all insert ops are processed by FLE.
 */
bool attemptGroupedFLEInserts(OperationContext* opCtx,
                              const BulkWriteCommandRequest& req,
                              size_t firstOpIdx,
                              const std::vector<BSONObj>& docs,
                              const NamespaceInfoEntry& nsInfoEntry,
                              write_ops_exec::WriteResult& out) {
    size_t numOps = docs.size();

    // For BulkWrite, re-entry is un-expected.
    invariant(!nsInfoEntry.getEncryptionInformation()->getCrudProcessed().value_or(false));

    auto request = getConsecutiveInsertRequest(req, firstOpIdx, docs, nsInfoEntry);
    write_ops::InsertCommandReply insertReply;

    FLEBatchResult batchResult = processFLEInsert(opCtx, request, &insertReply);

    if (batchResult == FLEBatchResult::kProcessed) {
        populateWriteResultWithInsertReply(numOps, req.getOrdered(), insertReply, out);
        return true;
    }
    return false;
}

// A class that meets the type requirements for timeseries::isTimeseriesViewRequest.
class TimeseriesBucketNamespace {
public:
    TimeseriesBucketNamespace() = delete;
    TimeseriesBucketNamespace(const NamespaceString& ns, const OptionalBool& isTimeseriesNamespace)
        : _ns(ns), _isTimeseriesNamespace(isTimeseriesNamespace) {}

    const NamespaceString& getNamespace() const {
        return _ns;
    }

    const OptionalBool& getIsTimeseriesNamespace() const {
        return _isTimeseriesNamespace;
    }

private:
    NamespaceString _ns;
    OptionalBool _isTimeseriesNamespace{OptionalBool()};
};

/*
 * Helper function to flush timeseries insert ops grouped by the insertGrouper.
 */
void handleGroupedTimeseriesInserts(OperationContext* opCtx,
                                    const BulkWriteCommandRequest& req,
                                    size_t firstOpIdx,
                                    const std::vector<BSONObj>& docs,
                                    const NamespaceInfoEntry& nsInfoEntry,
                                    CurOp* curOp,
                                    write_ops_exec::WriteResult& out) {
    size_t numOps = docs.size();
    auto request = getConsecutiveInsertRequest(req, firstOpIdx, docs, nsInfoEntry);
    auto insertReply = write_ops_exec::performTimeseriesWrites(opCtx, request, curOp);
    populateWriteResultWithInsertReply(numOps, req.getOrdered(), insertReply, out);
}

/*
 * Helper function to flush insert ops grouped by the insertGrouper.
 * Return true if we can continue with the rest of operations in the bulkWrite request.
 */
bool handleGroupedInserts(OperationContext* opCtx,
                          const BulkWriteCommandRequest& req,
                          InsertGrouper& insertGrouper,
                          write_ops_exec::LastOpFixer& lastOpFixer,
                          BulkWriteReplies& responses) {
    if (insertGrouper.isEmpty()) {
        return true;
    }
    auto [firstOpIdx, numOps] = insertGrouper.getGroupedInsertsAndReset();

    const auto& nsInfo = req.getNsInfo();
    const auto& ops = req.getOps();

    auto firstInsert = BulkWriteCRUDOp(ops[firstOpIdx]).getInsert();
    invariant(firstInsert);

    const auto nsIdx = firstInsert->getInsert();
    const auto& nsEntry = nsInfo[nsIdx];
    const auto& nsString = nsEntry.getNs();

    write_ops_exec::WriteResult out;
    out.results.reserve(numOps);

    auto insertDocs = getConsecutiveInsertDocuments(req, firstOpIdx, numOps);
    invariant(insertDocs.size() == numOps);

    // Handle FLE inserts.
    if (nsEntry.getEncryptionInformation().has_value()) {
        {
            // Flag set here and in fle_crud.cpp since this only executes on a mongod.
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
        }

        auto processed = attemptGroupedFLEInserts(opCtx, req, firstOpIdx, insertDocs, nsEntry, out);
        if (processed) {
            responses.addInsertReplies(opCtx, firstOpIdx, out);
            return out.canContinue;
        }
        // Fallthrough to standard inserts.
    }

    // Create nested CurOp for insert.
    auto& parentCurOp = *CurOp::get(opCtx);
    const Command* cmd = parentCurOp.getCommand();
    CurOp curOp(cmd);
    curOp.push(opCtx);
    ON_BLOCK_EXIT([&] { finishCurOp(opCtx, &curOp, LogicalOp::opInsert); });

    // If we are using document sequences for documents that total >16MB then getInsertOpDesc can
    // fail with BSONObjectTooLarge. If this happens we want to proceed with an empty BSONObj.
    BSONObj insertDocsObj;
    try {
        insertDocsObj = getInsertOpDesc(insertDocs, nsIdx);
    } catch (ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
        insertDocsObj = BSONObj();
    }

    // Initialize curOp information.
    setCurOpInfoAndEnsureStarted(opCtx, &curOp, LogicalOp::opInsert, nsEntry, insertDocsObj);

    // Handle timeseries inserts.
    TimeseriesBucketNamespace tsNs(nsString, nsEntry.getIsTimeseriesNamespace());
    if (auto [isTimeseriesViewRequest, _] = timeseries::isTimeseriesViewRequest(opCtx, tsNs);
        isTimeseriesViewRequest) {
        try {
            handleGroupedTimeseriesInserts(
                opCtx, req, firstOpIdx, insertDocs, nsEntry, &curOp, out);
            responses.addInsertReplies(opCtx, firstOpIdx, out);
            return out.canContinue;
        } catch (DBException& ex) {
            // Re-throw timeseries insert exceptions to be consistent with the insert command.
            ex.addContext(str::stream() << "time-series insert in bulkWrite failed: "
                                        << nsString.toStringForErrorMsg());
            throw;
        }
    }

    boost::optional<ScopedAdmissionPriority> priority;
    if (nsString == NamespaceString::kConfigSampledQueriesNamespace ||
        nsString == NamespaceString::kConfigSampledQueriesDiffNamespace) {
        priority.emplace(opCtx, AdmissionContext::Priority::kLow);
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);

    size_t bytesInBatch = 0;
    std::vector<InsertStatement> batch;
    const size_t maxBatchSize = internalInsertMaxBatchSize.load();
    const size_t maxBatchBytes = write_ops::insertVectorMaxBytes;
    batch.reserve(std::min(numOps, maxBatchSize));

    for (size_t i = 0; i < numOps; i++) {
        const bool isLastDoc = (i == numOps - 1);

        auto idx = firstOpIdx + i;
        auto& doc = insertDocs[i];
        bool containsDotsAndDollarsField = false;
        auto fixedDoc = fixDocumentForInsert(opCtx, doc, &containsDotsAndDollarsField);

        auto stmtId = opCtx->isRetryableWrite() ? bulk_write_common::getStatementId(req, idx)
                                                : kUninitializedStmtId;
        const bool wasAlreadyExecuted = opCtx->isRetryableWrite() &&
            txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx, stmtId);

        if (!fixedDoc.isOK()) {
            // Handled after we insert anything in the batch to be sure we report errors in the
            // correct order. In an ordered insert, if one of the docs ahead of us fails, we should
            // behave as-if we never got to this document.
        } else if (wasAlreadyExecuted) {
            // Similarly, if the insert was already executed as part of a retryable write, flush the
            // current batch to preserve the error results order.
        } else {
            BSONObj toInsert = fixedDoc.getValue().isEmpty() ? doc : std::move(fixedDoc.getValue());
            if (containsDotsAndDollarsField)
                dotsAndDollarsFieldsCounters.inserts.increment();
            batch.emplace_back(stmtId, toInsert);
            bytesInBatch += batch.back().doc.objsize();
            if (!isLastDoc && batch.size() < maxBatchSize && bytesInBatch < maxBatchBytes)
                continue;  // Add more to batch before inserting.
        }

        out.canContinue = write_ops_exec::insertBatchAndHandleErrors(opCtx,
                                                                     nsString,
                                                                     nsEntry.getCollectionUUID(),
                                                                     req.getOrdered(),
                                                                     batch,
                                                                     &lastOpFixer,
                                                                     &out,
                                                                     OperationSource::kStandard);

        batch.clear();
        bytesInBatch = 0;

        // If the batch had an error and decides to not continue, do not process a current doc that
        // was unsuccessfully "fixed" or an already executed retryable write.
        if (!out.canContinue) {
            break;
        }

        // Revisit any conditions that may have caused the batch to be flushed. In those cases,
        // append the appropriate result to the output.
        if (!fixedDoc.isOK()) {
            globalOpCounters.gotInsert();
            try {
                uassertStatusOK(fixedDoc.getStatus());
                MONGO_UNREACHABLE;
            } catch (const DBException& ex) {
                out.canContinue = write_ops_exec::handleError(opCtx,
                                                              ex,
                                                              nsString,
                                                              req.getOrdered(),
                                                              false /* isMultiUpdate */,
                                                              boost::none /* sampleId */,
                                                              &out);
            }
            if (!out.canContinue) {
                break;
            }
        } else if (wasAlreadyExecuted) {
            RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();

            SingleWriteResult res;
            res.setN(1);
            res.setNModified(0);
            out.retriedStmtIds.push_back(stmtId);
            out.results.emplace_back(res);
        }
    }

    invariant(batch.empty() && bytesInBatch == 0);
    responses.addInsertReplies(opCtx, firstOpIdx, out);
    return out.canContinue;
}

bool handleInsertOp(OperationContext* opCtx,
                    const BulkWriteInsertOp* op,
                    const BulkWriteCommandRequest& req,
                    size_t currentOpIdx,
                    write_ops_exec::LastOpFixer& lastOpFixer,
                    std::vector<int>& validatedNamespaces,
                    BulkWriteReplies& responses,
                    InsertGrouper& insertGrouper) {
    if (aboveBulkWriteRepliesMaxSize(opCtx, currentOpIdx, responses)) {
        return false;
    }
    const auto& nsInfo = req.getNsInfo();
    auto idx = op->getInsert();
    const auto& ns = nsInfo[idx].getNs();

    validateNamespaceForWrites(opCtx, idx, ns, validatedNamespaces);

    if (insertGrouper.group(op, currentOpIdx)) {
        return true;
    }

    // Not able to group this insert op, flush existing group first.
    auto canContinue = handleGroupedInserts(opCtx, req, insertGrouper, lastOpFixer, responses);
    if (!canContinue) {
        return false;
    }

    auto grouped = insertGrouper.group(op, currentOpIdx);
    invariant(grouped);
    return true;
}

// Unlike attemptGroupedFLEInserts, no fallback to non-FLE path is needed,
// returning false only indicate an error occurred.
bool attemptProcessFLEUpdate(OperationContext* opCtx,
                             const BulkWriteUpdateOp* op,
                             const BulkWriteCommandRequest& req,
                             size_t currentOpIdx,
                             BulkWriteReplies& responses,
                             const mongo::NamespaceInfoEntry& nsInfoEntry) {
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
    }

    write_ops::UpdateCommandRequest updateCommand =
        bulk_write_common::makeUpdateCommandRequestFromUpdateOp(op, req, currentOpIdx);
    write_ops::UpdateCommandReply updateReply = processFLEUpdate(opCtx, updateCommand);

    if (updateReply.getWriteErrors()) {
        const auto& errors = updateReply.getWriteErrors().get();
        invariant(errors.size() == 1);
        responses.addUpdateErrorReply(opCtx, currentOpIdx, errors[0].getStatus());
        return false;
    } else {
        boost::optional<int32_t> stmtId;
        if (updateReply.getRetriedStmtIds()) {
            const auto& retriedStmtIds = updateReply.getRetriedStmtIds().get();
            invariant(retriedStmtIds.size() == 1);
            stmtId = retriedStmtIds[0];
        }

        boost::optional<mongo::write_ops::Upserted> upserted;
        if (updateReply.getUpserted()) {
            const auto& upsertedDocuments = updateReply.getUpserted().get();
            invariant(upsertedDocuments.size() == 1);
            upserted = upsertedDocuments[0];
        }

        responses.addUpdateReply(
            currentOpIdx, updateReply.getN(), updateReply.getNModified(), upserted, stmtId);

        return true;
    }
}

// Unlike attemptGroupedFLEInserts, no fallback to non-FLE path is needed,
// returning false only indicate an error occurred.
bool attemptProcessFLEDelete(OperationContext* opCtx,
                             const BulkWriteDeleteOp* op,
                             const BulkWriteCommandRequest& req,
                             size_t currentOpIdx,
                             BulkWriteReplies& responses,
                             const mongo::NamespaceInfoEntry& nsInfoEntry) {
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
    }

    write_ops::DeleteCommandRequest deleteRequest =
        bulk_write_common::makeDeleteCommandRequestForFLE(opCtx, op, req, nsInfoEntry);
    write_ops::DeleteCommandReply deleteReply = processFLEDelete(opCtx, deleteRequest);

    if (deleteReply.getWriteErrors()) {
        const auto& errors = deleteReply.getWriteErrors().get();
        invariant(errors.size() == 1);
        auto replyItem = BulkWriteReplyItem(currentOpIdx);
        responses.addErrorReply(opCtx, replyItem, errors[0].getStatus());

        return false;
    } else {
        boost::optional<int32_t> stmtId;
        if (deleteReply.getRetriedStmtIds()) {
            const auto& retriedStmtIds = deleteReply.getRetriedStmtIds().get();
            invariant(retriedStmtIds.size() == 1);
            stmtId = retriedStmtIds[0];
        }

        responses.addDeleteReply(currentOpIdx, deleteReply.getN(), stmtId);
        return true;
    }
}

BSONObj makeSingleOpSampledBulkWriteCommandRequest(OperationContext* opCtx,
                                                   const BulkWriteCommandRequest& req,
                                                   size_t opIdx) {
    if (req.getOriginalQuery() || req.getOriginalCollation()) {
        tassert(7787101,
                "Found a _clusterWithoutShardKey command with bulkWrite ops size > 1",
                req.getOps().size() == 1);
        uassert(ErrorCodes::InvalidOptions,
                "Cannot specify '$_originalQuery' or '$_originalCollation' since they are internal "
                "fields",
                analyze_shard_key::isInternalClient(opCtx));
    }

    auto op = BulkWriteCRUDOp(req.getOps()[opIdx]);

    // Make a copy of the nsEntry for the op at opIdx.
    NamespaceInfoEntry newNsEntry = req.getNsInfo()[op.getNsInfoIdx()];

    // Make a copy of the operation and adjust its namespace index to 0.
    auto newOp = req.getOps()[opIdx];
    visit(OverloadedVisitor{
              [&](mongo::BulkWriteInsertOp& op) { MONGO_UNREACHABLE },
              [&](mongo::BulkWriteUpdateOp& op) {
                  op.setUpdate(0);
                  if (req.getOriginalQuery() || req.getOriginalCollation()) {
                      op.setFilter(req.getOriginalQuery().get_value_or({}));
                      op.setCollation(req.getOriginalCollation());
                  }
              },
              [&](mongo::BulkWriteDeleteOp& op) {
                  op.setDeleteCommand(0);
                  if (req.getOriginalQuery() || req.getOriginalCollation()) {
                      op.setFilter(req.getOriginalQuery().get_value_or({}));
                      op.setCollation(req.getOriginalCollation());
                  }
              },
          },
          newOp);

    BulkWriteCommandRequest singleOpRequest;
    singleOpRequest.setOps({newOp});
    singleOpRequest.setNsInfo({newNsEntry});
    singleOpRequest.setBypassDocumentValidation(req.getBypassDocumentValidation());
    singleOpRequest.setLet(req.getLet());
    singleOpRequest.setStmtId(bulk_write_common::getStatementId(req, opIdx));
    singleOpRequest.setDbName(DatabaseName::kAdmin);

    return singleOpRequest.toBSON(
        BSON(BulkWriteCommandRequest::kDbNameFieldName << DatabaseNameUtil::serialize(
                 DatabaseName::kAdmin, SerializationContext::stateCommandRequest())));
}

bool handleDeleteOp(OperationContext* opCtx,
                    const BulkWriteDeleteOp* op,
                    const BulkWriteCommandRequest& req,
                    size_t currentOpIdx,
                    write_ops_exec::LastOpFixer& lastOpFixer,
                    std::vector<int>& validatedNamespaces,
                    BulkWriteReplies& responses) {
    if (aboveBulkWriteRepliesMaxSize(opCtx, currentOpIdx, responses)) {
        return false;
    }

    const auto& nsInfo = req.getNsInfo();
    auto idx = op->getDeleteCommand();
    auto& nsEntry = nsInfo.at(idx);
    try {
        if (op->getMulti()) {
            uassert(ErrorCodes::InvalidOptions,
                    "Cannot use retryable writes with multi=true",
                    !opCtx->isRetryableWrite());
        }

        const NamespaceString& nsString = nsEntry.getNs();
        validateNamespaceForWrites(opCtx, idx, nsString, validatedNamespaces);

        // Handle FLE deletes.
        if (nsEntry.getEncryptionInformation().has_value()) {
            return attemptProcessFLEDelete(opCtx, op, req, currentOpIdx, responses, nsEntry);
        }

        // Non-FLE deletes (including timeseries deletes) will be handled by
        // write_ops_exec::performDelete.

        auto stmtId = opCtx->isRetryableWrite()
            ? bulk_write_common::getStatementId(req, currentOpIdx)
            : kUninitializedStmtId;
        if (opCtx->isRetryableWrite()) {
            const auto txnParticipant = TransactionParticipant::get(opCtx);
            if (txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx, stmtId)) {
                RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
                // Since multi:true is not allowed with retryable writes if the statement was
                // executed there will always be 1 document deleted.
                responses.addDeleteReply(currentOpIdx, 1, stmtId);
                return true;
            }
        }

        // Create nested CurOp for delete.
        auto& parentCurOp = *CurOp::get(opCtx);
        const Command* cmd = parentCurOp.getCommand();
        CurOp curOp(cmd);
        curOp.push(opCtx);
        ON_BLOCK_EXIT([&] { finishCurOp(opCtx, &curOp, LogicalOp::opDelete); });

        // Initialize curOp information.
        setCurOpInfoAndEnsureStarted(opCtx, &curOp, LogicalOp::opDelete, nsEntry, op->toBSON());

        auto deleteRequest = bulk_write_common::makeDeleteRequestFromDeleteOp(
            opCtx, nsEntry, op, stmtId, req.getLet());

        if (auto sampleId = analyze_shard_key::getOrGenerateSampleId(
                opCtx,
                nsString,
                analyze_shard_key::SampledCommandNameEnum::kBulkWrite,
                deleteRequest)) {
            analyze_shard_key::QueryAnalysisWriter::get(opCtx)
                ->addDeleteQuery(
                    analyze_shard_key::SampledCommandNameEnum::kBulkWrite,
                    {sampleId.value(),
                     nsString,
                     makeSingleOpSampledBulkWriteCommandRequest(opCtx, req, currentOpIdx)})
                .getAsync([](auto) {});
        }

        const bool inTransaction = opCtx->inMultiDocumentTransaction();
        lastOpFixer.startingOp(nsString);
        return writeConflictRetry(opCtx, "bulkWriteDelete", nsString, [&] {
            boost::optional<BSONObj> docFound;
            globalOpCounters.gotDelete();
            ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForDelete(
                opCtx->getWriteConcern());
            auto nDeleted = write_ops_exec::performDelete(opCtx,
                                                          nsString,
                                                          &deleteRequest,
                                                          &curOp,
                                                          inTransaction,
                                                          nsEntry.getCollectionUUID(),
                                                          docFound);
            lastOpFixer.finishedOpSuccessfully();
            responses.addDeleteReply(currentOpIdx, nDeleted, boost::none);
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
            opCtx, ex, nsEntry.getNs(), req.getOrdered(), false, boost::none, &out);
    }
}

template <typename BulkWriteUpdateOrDeleteOp>
BSONObj getQueryForExplain(OperationContext* opCtx,
                           const BulkWriteCommandRequest& req,
                           const BulkWriteUpdateOrDeleteOp* op,
                           const NamespaceInfoEntry& nsEntry) {
    if (shouldDoFLERewrite(nsEntry)) {
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
        }

        if (!nsEntry.getEncryptionInformation()->getCrudProcessed().value_or(false)) {
            return processFLEWriteExplainD(opCtx,
                                           op->getCollation().value_or(BSONObj()),
                                           nsEntry.getNs(),
                                           nsEntry.getEncryptionInformation().get(),
                                           boost::none, /* runtimeConstants */
                                           req.getLet(),
                                           op->getFilter());
        }
    }

    return op->getFilter();
}

void explainUpdateOp(OperationContext* opCtx,
                     const BulkWriteCommandRequest& req,
                     const mongo::BulkWriteUpdateOp* op,
                     const NamespaceInfoEntry& nsEntry,
                     const BSONObj& command,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) {
    invariant(op);
    TimeseriesBucketNamespace tsNs(nsEntry.getNs(), nsEntry.getIsTimeseriesNamespace());
    auto [isTimeseriesViewRequest, nss] = timeseries::isTimeseriesViewRequest(opCtx, tsNs);

    auto updateRequest = UpdateRequest();

    updateRequest.setNamespaceString(nss);
    updateRequest.setQuery(getQueryForExplain(opCtx, req, op, nsEntry));
    updateRequest.setProj(BSONObj());
    updateRequest.setUpdateModification(op->getUpdateMods());
    updateRequest.setLegacyRuntimeConstants(Variables::generateRuntimeConstants(opCtx));
    updateRequest.setUpdateConstants(op->getConstants());
    updateRequest.setLetParameters(req.getLet());
    updateRequest.setHint(op->getHint());
    updateRequest.setCollation(op->getCollation().value_or(BSONObj()));
    updateRequest.setArrayFilters(op->getArrayFilters().value_or(std::vector<BSONObj>()));
    updateRequest.setUpsert(op->getUpsert());
    updateRequest.setUpsertSuppliedDocument(op->getUpsertSupplied().value_or(false));
    updateRequest.setReturnDocs(UpdateRequest::RETURN_NONE);
    updateRequest.setMulti(op->getMulti());
    updateRequest.setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
    updateRequest.setExplain(verbosity);

    write_ops_exec::explainUpdate(opCtx,
                                  updateRequest,
                                  isTimeseriesViewRequest,
                                  req.getSerializationContext(),
                                  command,
                                  verbosity,
                                  result);
}

void explainDeleteOp(OperationContext* opCtx,
                     const BulkWriteCommandRequest& req,
                     const mongo::BulkWriteDeleteOp* op,
                     const NamespaceInfoEntry& nsEntry,
                     const BSONObj& command,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) {
    invariant(op);
    TimeseriesBucketNamespace tsNs(nsEntry.getNs(), nsEntry.getIsTimeseriesNamespace());
    auto [isTimeseriesViewRequest, nss] = timeseries::isTimeseriesViewRequest(opCtx, tsNs);

    auto deleteRequest = DeleteRequest();

    deleteRequest.setNsString(nss);
    deleteRequest.setQuery(getQueryForExplain(opCtx, req, op, nsEntry));
    deleteRequest.setProj(BSONObj());
    deleteRequest.setLegacyRuntimeConstants(Variables::generateRuntimeConstants(opCtx));
    deleteRequest.setLet(req.getLet());
    deleteRequest.setHint(op->getHint());
    deleteRequest.setCollation(op->getCollation().value_or(BSONObj()));
    deleteRequest.setMulti(op->getMulti());
    deleteRequest.setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
    deleteRequest.setIsExplain(true);

    write_ops_exec::explainDelete(opCtx,
                                  deleteRequest,
                                  isTimeseriesViewRequest,
                                  req.getSerializationContext(),
                                  command,
                                  verbosity,
                                  result);
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

    LogicalOp getLogicalOp() const final {
        return LogicalOp::opBulkWrite;
    }

    std::string help() const override {
        return "command to apply inserts, updates and deletes in bulk";
    }

    class Invocation final : public InvocationBaseGen {
    public:
        Invocation(OperationContext* opCtx,
                   const Command* command,
                   const OpMsgRequest& opMsgRequest)
            : InvocationBaseGen(opCtx, command, opMsgRequest), _commandObj(opMsgRequest.body) {
            // We need to use isEnabledUseLastLTSFCVWhenUninitialized here because a bulk write
            // command could be sent directly to an initial sync node with uninitialized FCV, and
            // creating this command invocation happens before any check that the node is a primary.
            uassert(ErrorCodes::CommandNotSupported,
                    "BulkWrite may not be run without featureFlagBulkWriteCommand enabled",
                    gFeatureFlagBulkWriteCommand.isEnabledUseLastLTSFCVWhenUninitialized(
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

            bulk_write_common::validateRequest(request(), /*isRouter=*/false);

            // Extract and store the first update op for building mirrored read request.
            _extractFirstUpdateOp();
        }

        bool supportsWriteConcern() const final {
            return true;
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        std::vector<NamespaceString> allNamespaces() const final {
            auto nsInfos = request().getNsInfo();
            std::vector<NamespaceString> result(nsInfos.size());

            for (auto& nsInfo : nsInfos) {
                result.emplace_back(nsInfo.getNs());
            }

            return result;
        }

        bool supportsReadMirroring() const final {
            // Only do mirrored read if there exists an update op in bulk write request.
            return _firstUpdateOp;
        }

        DatabaseName getDBForReadMirroring() const final {
            const auto nsIdx = _firstUpdateOp->getUpdate();
            const auto& nsInfo = request().getNsInfo().at(nsIdx);

            return nsInfo.getNs().dbName();
        }

        void appendMirrorableRequest(BSONObjBuilder* bob) const final {
            invariant(_firstUpdateOp);

            const auto& req = request();
            const auto nsIdx = _firstUpdateOp->getUpdate();
            const auto& nsInfo = req.getNsInfo().at(nsIdx);

            bob->append("find", nsInfo.getNs().coll());

            if (!_firstUpdateOp->getFilter().isEmpty()) {
                bob->append("filter", _firstUpdateOp->getFilter());
            }
            if (!_firstUpdateOp->getHint().isEmpty()) {
                bob->append("hint", _firstUpdateOp->getHint());
            }
            if (_firstUpdateOp->getCollation()) {
                bob->append("collation", *_firstUpdateOp->getCollation());
            }

            bob->append("batchSize", 1);
            bob->append("singleBatch", true);

            if (nsInfo.getShardVersion()) {
                nsInfo.getShardVersion()->serialize("shardVersion", bob);
            }
            if (nsInfo.getEncryptionInformation()) {
                bob->append(FindCommandRequest::kEncryptionInformationFieldName,
                            nsInfo.getEncryptionInformation()->toBSON());
            }
            if (nsInfo.getDatabaseVersion()) {
                bob->append("databaseVersion", nsInfo.getDatabaseVersion()->toBSON());
            }
        }

        Reply typedRun(OperationContext* opCtx) final {
            auto& req = request();

            // Apply all of the write operations.
            auto [replies, retriedStmtIds, summaryFields] = bulk_write::performWrites(opCtx, req);

            return _populateCursorReply(
                opCtx, req, std::move(replies), std::move(retriedStmtIds), summaryFields);
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

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) final {
            const auto& req = request();
            const auto& ops = req.getOps();

            uassert(ErrorCodes::InvalidLength,
                    "explained bulkWrite ops must be of size 1",
                    ops.size() == 1);

            const BulkWriteCRUDOp bulkOp(ops[0]);
            const auto opType = bulkOp.getType();
            const auto& nsEntry = req.getNsInfo()[bulkOp.getNsInfoIdx()];

            if (opType == BulkWriteCRUDOp::kInsert) {
                uasserted(ErrorCodes::IllegalOperation, "Cannot explain insert cmd for bulkWrite");
            } else if (opType == BulkWriteCRUDOp::kUpdate) {
                explainUpdateOp(
                    opCtx, req, bulkOp.getUpdate(), nsEntry, _commandObj, verbosity, result);
            } else if (opType == BulkWriteCRUDOp::kDelete) {
                explainDeleteOp(
                    opCtx, req, bulkOp.getDelete(), nsEntry, _commandObj, verbosity, result);
            } else {
                MONGO_UNREACHABLE;
            }
        }

    private:
        Reply _populateCursorReply(OperationContext* opCtx,
                                   const BulkWriteCommandRequest& req,
                                   bulk_write::BulkWriteReplyItems replies,
                                   bulk_write::RetriedStmtIds retriedStmtIds,
                                   bulk_write::SummaryFields summaryFields) {
            auto reqObj = unparsedRequest().body;
            const NamespaceString cursorNss =
                NamespaceString::makeBulkWriteNSS(req.getDollarTenant());

            if (replies.size() == 0 || bulk_write_common::isUnacknowledgedBulkWrite(opCtx)) {
                // Skip cursor creation and return the simplest reply.
                auto reply =
                    BulkWriteCommandReply(BulkWriteCommandResponseCursor(
                                              0 /* cursorId */, {} /* firstBatch */, cursorNss),
                                          summaryFields.nErrors,
                                          summaryFields.nInserted,
                                          summaryFields.nMatched,
                                          summaryFields.nModified,
                                          summaryFields.nUpserted,
                                          summaryFields.nDeleted);

                if (!retriedStmtIds.empty()) {
                    reply.setRetriedStmtIds(std::move(retriedStmtIds));
                }

                _setElectionIdAndOpTime(opCtx, reply);
                return reply;
            }

            // Try and fit all replies into the firstBatch.
            long long batchSize = std::numeric_limits<long long>::max();
            if (req.getCursor() && req.getCursor()->getBatchSize()) {
                batchSize = *req.getCursor()->getBatchSize();
            }

            if (batchSize > (long long)replies.size()) {
                batchSize = replies.size();
            }

            size_t numRepliesInFirstBatch = 0;
            FindCommon::BSONArrayResponseSizeTracker responseSizeTracker;
            for (long long objCount = 0; objCount < batchSize; objCount++) {
                BSONObj nextDoc;
                nextDoc = replies[objCount].toBSON();

                // If we can't fit this result inside the current batch, then we stash it for
                // later.
                if (!responseSizeTracker.haveSpaceForNext(nextDoc)) {
                    break;
                }

                numRepliesInFirstBatch++;
                responseSizeTracker.add(nextDoc);
            }

            long long cursorId = 0;

            // We have replies left that will not make the first batch. Need to construct a cursor.
            if (numRepliesInFirstBatch != replies.size()) {
                auto expCtx = make_intrusive<ExpressionContext>(
                    opCtx, std::unique_ptr<CollatorInterface>(nullptr), ns());

                std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
                auto ws = std::make_unique<WorkingSet>();
                auto root = std::make_unique<QueuedDataStage>(expCtx.get(), ws.get());

                size_t currentIdx = numRepliesInFirstBatch;
                for (; currentIdx < replies.size(); currentIdx++) {
                    WorkingSetID id = ws->allocate();
                    WorkingSetMember* member = ws->get(id);
                    member->keyData.clear();
                    member->recordId = RecordId();
                    member->resetDocument(SnapshotId(), replies[currentIdx].toBSON());
                    member->transitionToOwnedObj();
                    root->pushBack(id);
                }

                exec = uassertStatusOK(
                    plan_executor_factory::make(expCtx,
                                                std::move(ws),
                                                std::move(root),
                                                &CollectionPtr::null,
                                                PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                false, /* whether owned BSON must be returned */
                                                cursorNss));

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
                cursorId = pinnedCursor.getCursor()->cursorid();

                pinnedCursor->incNBatches();
                pinnedCursor->incNReturnedSoFar(replies.size());
            }

            CurOp::get(opCtx)->setEndOfOpMetrics(numRepliesInFirstBatch);

            replies.resize(numRepliesInFirstBatch);
            auto reply = BulkWriteCommandReply(
                BulkWriteCommandResponseCursor(cursorId, std::move(replies), cursorNss),
                summaryFields.nErrors,
                summaryFields.nInserted,
                summaryFields.nMatched,
                summaryFields.nModified,
                summaryFields.nUpserted,
                summaryFields.nDeleted);

            if (!retriedStmtIds.empty()) {
                reply.setRetriedStmtIds(std::move(retriedStmtIds));
            }

            _setElectionIdAndOpTime(opCtx, reply);
            return reply;
        }

        void _setElectionIdAndOpTime(OperationContext* opCtx, BulkWriteCommandReply& reply) {
            // Undocumented repl fields that mongos depends on.
            auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
            if (replCoord->getSettings().isReplSet()) {
                reply.setOpTime(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp());
                reply.setElectionId(replCoord->getElectionId());
            }
        }

        void _extractFirstUpdateOp() {
            const auto& ops = request().getOps();

            auto it = std::find_if(ops.begin(), ops.end(), [](const auto& op) {
                return BulkWriteCRUDOp(op).getType() == BulkWriteCRUDOp::kUpdate;
            });

            if (it != ops.end()) {
                // Current design only uses the first update op for mirrored read.
                _firstUpdateOp = BulkWriteCRUDOp(*it).getUpdate();
                invariant(_firstUpdateOp);
            }
        }

        const BSONObj& _commandObj;
        const mongo::BulkWriteUpdateOp* _firstUpdateOp{nullptr};
    };
};
MONGO_REGISTER_COMMAND(BulkWriteCmd).forShard();

bool handleUpdateOp(OperationContext* opCtx,
                    const BulkWriteUpdateOp* op,
                    const BulkWriteCommandRequest& req,
                    size_t currentOpIdx,
                    write_ops_exec::LastOpFixer& lastOpFixer,
                    std::vector<int>& validatedNamespaces,
                    BulkWriteReplies& responses) {
    if (aboveBulkWriteRepliesMaxSize(opCtx, currentOpIdx, responses)) {
        return false;
    }

    const auto& nsInfo = req.getNsInfo();
    const auto idx = op->getUpdate();
    const auto& nsEntry = nsInfo[idx];

    try {
        if (op->getMulti()) {
            uassert(ErrorCodes::InvalidOptions,
                    "Cannot use retryable writes with multi=true",
                    !opCtx->isRetryableWrite());
        }

        const NamespaceString& nsString = nsEntry.getNs();
        validateNamespaceForWrites(opCtx, idx, nsString, validatedNamespaces);

        // Handle FLE updates.
        if (nsEntry.getEncryptionInformation().has_value()) {
            // For BulkWrite, re-entry is un-expected.
            invariant(!nsEntry.getEncryptionInformation()->getCrudProcessed().value_or(false));

            // Map to processFLEUpdate.
            return attemptProcessFLEUpdate(opCtx, op, req, currentOpIdx, responses, nsEntry);
        }

        auto stmtId = opCtx->isRetryableWrite()
            ? bulk_write_common::getStatementId(req, currentOpIdx)
            : kUninitializedStmtId;

        TimeseriesBucketNamespace tsNs(nsEntry.getNs(), nsEntry.getIsTimeseriesNamespace());
        auto [isTimeseriesViewRequest, bucketNs] = timeseries::isTimeseriesViewRequest(opCtx, tsNs);

        // Handle retryable timeseries updates.
        if (isTimeseriesViewRequest && opCtx->isRetryableWrite() &&
            !opCtx->inMultiDocumentTransaction()) {
            write_ops_exec::WriteResult out;
            auto executor = serverGlobalParams.clusterRole.has(ClusterRole::None)
                ? ReplicaSetNodeProcessInterface::getReplicaSetNodeExecutor(
                      opCtx->getServiceContext())
                : Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
            auto updateRequest =
                bulk_write_common::makeUpdateCommandRequestFromUpdateOp(op, req, currentOpIdx);

            write_ops_exec::runTimeseriesRetryableUpdates(
                opCtx, bucketNs, updateRequest, executor, &out);
            responses.addUpdateReply(opCtx, currentOpIdx, out);

            bulk_write_common::incrementBulkWriteUpdateMetrics(ClusterRole::ShardServer,
                                                               op->getUpdateMods(),
                                                               nsEntry.getNs(),
                                                               op->getArrayFilters());
            return out.canContinue;
        }

        // Handle retryable non-timeseries updates.
        if (opCtx->isRetryableWrite()) {
            const auto txnParticipant = TransactionParticipant::get(opCtx);
            if (auto entry = txnParticipant.checkStatementExecuted(opCtx, stmtId)) {
                RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();

                auto [numMatched, numDocsModified, upserted] =
                    getRetryResultForUpdate(opCtx, nsString, op, entry);

                responses.addUpdateReply(
                    currentOpIdx, numMatched, numDocsModified, upserted, stmtId);

                bulk_write_common::incrementBulkWriteUpdateMetrics(ClusterRole::ShardServer,
                                                                   op->getUpdateMods(),
                                                                   nsEntry.getNs(),
                                                                   op->getArrayFilters());
                return true;
            }
        }

        // Create nested CurOp for update.
        auto& parentCurOp = *CurOp::get(opCtx);
        const Command* cmd = parentCurOp.getCommand();
        CurOp curOp(cmd);
        curOp.push(opCtx);
        ON_BLOCK_EXIT([&] { finishCurOp(opCtx, &curOp, LogicalOp::opUpdate); });

        // Initialize curOp information.
        setCurOpInfoAndEnsureStarted(opCtx, &curOp, LogicalOp::opUpdate, nsEntry, op->toBSON());

        // Handle non-retryable normal and timeseries updates, as well as retryable normal
        // updates that were not already executed.

        auto updateRequest = bulk_write_common::makeUpdateRequestFromUpdateOp(
            opCtx, nsEntry, op, stmtId, req.getLet());

        if (auto sampleId = analyze_shard_key::getOrGenerateSampleId(
                opCtx,
                nsString,
                analyze_shard_key::SampledCommandNameEnum::kBulkWrite,
                updateRequest)) {
            analyze_shard_key::QueryAnalysisWriter::get(opCtx)
                ->addUpdateQuery(
                    analyze_shard_key::SampledCommandNameEnum::kBulkWrite,
                    {sampleId.value(),
                     nsString,
                     makeSingleOpSampledBulkWriteCommandRequest(opCtx, req, currentOpIdx)})
                .getAsync([](auto) {});
            updateRequest.setSampleId(sampleId);
        }

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
                    globalOpCounters.gotUpdate();
                    ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForUpdate(
                        opCtx->getWriteConcern());
                    auto result = write_ops_exec::performUpdate(opCtx,
                                                                nsString,
                                                                &curOp,
                                                                opCtx->inMultiDocumentTransaction(),
                                                                false,
                                                                updateRequest.isUpsert(),
                                                                nsEntry.getCollectionUUID(),
                                                                docFound,
                                                                &updateRequest);
                    lastOpFixer.finishedOpSuccessfully();
                    responses.addUpdateReply(currentOpIdx, result, boost::none);
                    bulk_write_common::incrementBulkWriteUpdateMetrics(ClusterRole::ShardServer,
                                                                       op->getUpdateMods(),
                                                                       nsEntry.getNs(),
                                                                       op->getArrayFilters());
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
            opCtx, ex, nsEntry.getNs(), req.getOrdered(), op->getMulti(), boost::none, &out);
    }
}

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

    write_ops_exec::LastOpFixer lastOpFixer(opCtx);

    std::vector<int> validatedNamespaces = std::vector<int>();
    validatedNamespaces.assign(req.getNsInfo().size(), 0);

    // Create an insertGrouper to group consecutive inserts to the same namespace.
    auto insertGrouper = InsertGrouper(req);

    size_t idx = 0;

    ON_BLOCK_EXIT([&] {
        // If any statements were retried then increment command counter.
        write_ops_exec::updateRetryStats(opCtx, !responses.getRetriedStmtIds().empty());
    });

    bool hasEncryptionInformation = false;

    // Tell mongod what the shard and database versions are. This will cause writes to fail in
    // case there is a mismatch in the mongos request provided versions and the local (shard's)
    // understanding of the version.
    for (const auto& nsInfo : req.getNsInfo()) {
        auto& ns = nsInfo.getNs();
        auto& shardVersion = nsInfo.getShardVersion();
        auto& databaseVersion = nsInfo.getDatabaseVersion();

        if (shardVersion || databaseVersion) {
            // If a timeseries collection is sharded, only the buckets collection would be sharded.
            // We expect all versioned commands to be sent over 'system.buckets' namespace. But it
            // is possible that a stale mongos may send the request over a view namespace. In this
            // case, we initialize the 'OperationShardingState' with buckets namespace. The bucket
            // namespace is used because if the shard recognizes this is a timeseries collection,
            // the timeseries write path will eventually execute on the bucket namespace and locks
            // will be acquired with the bucket namespace. So we must initialize the
            // 'OperationShardingState' with the bucket namespace to trigger the shard version
            // checks.
            TimeseriesBucketNamespace tsNs(ns, nsInfo.getIsTimeseriesNamespace());
            // The returned namespaceForSharding will be the timeseries system bucket collection if
            // the request is made on a timeseries collection. Otherwise, it will stay unchanged
            // (i.e. the namespace from the client request).
            auto [_, namespaceForSharding] = timeseries::isTimeseriesViewRequest(opCtx, tsNs);

            OperationShardingState::setShardRole(
                opCtx, namespaceForSharding, shardVersion, databaseVersion);
        }

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
            // Before we pause processing, flush grouped inserts.
            if (!handleGroupedInserts(opCtx, req, insertGrouper, lastOpFixer, responses)) {
                break;
            }
            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &hangBetweenProcessingBulkWriteOps, opCtx, "hangBetweenProcessingBulkWriteOps");
        }

        auto op = BulkWriteCRUDOp(ops[idx]);
        auto opType = op.getType();

        if (opType == BulkWriteCRUDOp::kInsert) {
            if (!handleInsertOp(opCtx,
                                op.getInsert(),
                                req,
                                idx,
                                lastOpFixer,
                                validatedNamespaces,
                                responses,
                                insertGrouper)) {
                // Insert write failed can no longer continue.
                break;
            }
        } else if (opType == BulkWriteCRUDOp::kUpdate) {
            // Flush grouped insert ops before handling update ops.
            if (!handleGroupedInserts(opCtx, req, insertGrouper, lastOpFixer, responses)) {
                break;
            }

            if (hasEncryptionInformation) {
                uassert(
                    ErrorCodes::InvalidOptions,
                    "BulkWrite update with Queryable Encryption supports only a single operation.",
                    ops.size() == 1);
            }
            if (!handleUpdateOp(
                    opCtx, op.getUpdate(), req, idx, lastOpFixer, validatedNamespaces, responses)) {
                // Update write failed can no longer continue.
                break;
            }
        } else if (opType == BulkWriteCRUDOp::kDelete) {
            // Flush grouped insert ops before handling delete ops.
            if (!handleGroupedInserts(opCtx, req, insertGrouper, lastOpFixer, responses)) {
                break;
            }

            if (hasEncryptionInformation) {
                uassert(
                    ErrorCodes::InvalidOptions,
                    "BulkWrite delete with Queryable Encryption supports only a single operation.",
                    ops.size() == 1);
            }
            if (!handleDeleteOp(
                    opCtx, op.getDelete(), req, idx, lastOpFixer, validatedNamespaces, responses)) {
                // Delete write failed can no longer continue.
                break;
            }
        } else {
            MONGO_UNREACHABLE;
        }
    }

    // It does not matter if this final flush had errors or not since we finished processing
    // the last op already.
    handleGroupedInserts(opCtx, req, insertGrouper, lastOpFixer, responses);
    invariant(insertGrouper.isEmpty());

    return make_tuple(
        responses.getReplies(), responses.getRetriedStmtIds(), responses.getSummaryFields());
}

}  // namespace bulk_write
}  // namespace mongo
