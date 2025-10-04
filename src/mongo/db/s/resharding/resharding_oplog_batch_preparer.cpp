/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_oplog_batch_preparer.h"

#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/write_ops/write_ops_retryability.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

/**
 * Return true if we need to update config.transactions collection for this oplog entry.
 */
bool shouldUpdateTxnTable(const repl::OplogEntry& op) {
    if (op.getCommandType() == repl::OplogEntry::CommandType::kAbortTransaction) {
        return true;
    }

    if (!op.getSessionId()) {
        return false;
    }

    if (!op.getTxnNumber()) {
        return false;
    }

    if (op.getCommandType() == repl::OplogEntry::CommandType::kApplyOps) {
        // This applyOps oplog entry is guaranteed to correspond to a committed transaction since
        // the resharding aggregation pipeline does not output applyOps oplog entries for aborted
        // transactions (i.e. it only outputs the abortTransaction oplog entry).

        if (isInternalSessionForRetryableWrite(*op.getSessionId())) {
            // For a retryable internal transaction, we need to update the config.transactions
            // collection upon writing the noop oplog entries for retryable operations contained
            // within each applyOps oplog entry.
            return true;
        }

        // The resharding aggregation pipeline also does not output the commitTransaction oplog
        // entry so for a non-retryable transaction, we need to the update to the
        // config.transactions collection upon seeing the final applyOps oplog entry.
        return !op.isPartialTransaction();
    }

    return false;
}

}  // anonymous namespace

using WriterVectors = ReshardingOplogBatchPreparer::WriterVectors;

ReshardingOplogBatchPreparer::ReshardingOplogBatchPreparer(
    std::size_t oplogBatchTaskCount,
    std::unique_ptr<CollatorInterface> defaultCollator,
    bool isCapped)
    : _oplogBatchTaskCount(oplogBatchTaskCount),
      _defaultCollator(std::move(defaultCollator)),
      _isCapped(isCapped) {}

void ReshardingOplogBatchPreparer::throwIfUnsupportedCommandOp(const OplogEntry& op) {
    invariant(op.isCommand());
    switch (op.getCommandType()) {
        case OplogEntry::CommandType::kApplyOps:
        case OplogEntry::CommandType::kCommitTransaction:
        case OplogEntry::CommandType::kAbortTransaction:
            return;

        case OplogEntry::CommandType::kDrop:
            uasserted(ErrorCodes::OplogOperationUnsupported,
                      str::stream() << "Received drop command for resharding's source collection: "
                                    << redact(op.toBSONForLogging()));

        default:
            uasserted(ErrorCodes::OplogOperationUnsupported,
                      str::stream() << "Command not supported during resharding: "
                                    << redact(op.toBSONForLogging()));
    }
}

WriterVectors ReshardingOplogBatchPreparer::makeCrudOpWriterVectors(
    const OplogBatchToPrepare& batch, std::list<OplogEntry>& derivedOps) const {
    invariant(derivedOps.empty());

    auto writerVectors = _makeEmptyWriterVectors();

    for (const auto& op : batch) {
        if (op.isCrudOpType()) {
            _appendCrudOpToWriterVector(&op, writerVectors);
        } else if (op.isCommand()) {
            throwIfUnsupportedCommandOp(op);

            if (op.getCommandType() != OplogEntry::CommandType::kApplyOps) {
                continue;
            }

            auto applyOpsInfo = repl::ApplyOpsCommandInfo::parse(op.getObject());
            uassert(
                ErrorCodes::OplogOperationUnsupported,
                str::stream() << "Commands within applyOps are not supported during resharding: "
                              << redact(op.toBSONForLogging()),
                applyOpsInfo.areOpsCrudOnly());

            auto unrolledOp =
                uassertStatusOK(repl::MutableOplogEntry::parse(op.getEntry().toBSON()));

            for (const auto& innerOp : applyOpsInfo.getOperations()) {
                unrolledOp.setDurableReplOperation(repl::DurableReplOperation::parse(
                    innerOp,
                    IDLParserContext{
                        "ReshardingOplogBatchPreparer::makeCrudOpWriterVectors innerOp"}));

                if (isWouldChangeOwningShardSentinelOplogEntry(unrolledOp)) {
                    continue;
                }

                // There isn't a direct way to convert from a MutableOplogEntry to a
                // DurableOplogEntry or OplogEntry. We serialize the unrolledOp to have it get
                // re-parsed into an OplogEntry.
                auto& derivedOp = derivedOps.emplace_back(unrolledOp.toBSON());
                invariant(derivedOp.isCrudOpType());

                // `&derivedOp` is guaranteed to remain stable while we append more derived oplog
                // entries because `derivedOps` is a std::list.
                _appendCrudOpToWriterVector(&derivedOp, writerVectors);
            }
        } else if (resharding::isProgressMarkOplogAfterOplogApplicationStarted(op)) {
            // This is a progress mark oplog entry created after resharding oplog application has
            // started. The oplog entry does not need to be applied but is used for calculating the
            // average time to apply oplog entries. So if
            // 'reshardingRemainingTimeEstimateBasedOnMovingAverage' is enabled, add the oplog entry
            // to a random writer.
            if (resharding::gReshardingRemainingTimeEstimateBasedOnMovingAverage.load()) {
                _appendOpToWriterVector(absl::HashOf(UUID::gen()), &op, writerVectors);
            }
        } else {
            invariant(repl::OpTypeEnum::kNoop == op.getOpType());
        }
    }

    return writerVectors;
}

struct SessionOpsList {
    TxnNumber txnNum = kUninitializedTxnNumber;
    std::vector<const repl::OplogEntry*> ops;
};

void updateSessionTracker(LogicalSessionIdMap<SessionOpsList>& sessionTracker,
                          const repl::OplogEntry* op) {
    uassert(9572401,
            str::stream() << "Missing sessionId for oplog entry: "
                          << redact(op->toBSONForLogging()),
            op->getSessionId());
    uassert(4990700,
            str::stream() << "Missing txnNumber for oplog entry with lsid: "
                          << redact(op->toBSONForLogging()),
            op->getTxnNumber());

    const auto& lsid = *op->getSessionId();
    auto txnNumber = *op->getTxnNumber();

    auto& retryableOpList = sessionTracker[lsid];
    if (txnNumber == retryableOpList.txnNum) {
        retryableOpList.ops.emplace_back(op);
    } else if (txnNumber > retryableOpList.txnNum) {
        retryableOpList.ops = {op};
        retryableOpList.txnNum = txnNumber;
    } else {
        uasserted(4990401,
                  str::stream() << "Encountered out of order txnNumbers; batch had "
                                << redact(op->toBSONForLogging()) << " after "
                                << redact(retryableOpList.ops.back()->toBSONForLogging()));
    }
}

void unrollApplyOpsAndUpdateSessionTracker(LogicalSessionIdMap<SessionOpsList>& sessionTracker,
                                           std::list<repl::OplogEntry>& derivedOps,
                                           const repl::OplogEntry& op,
                                           const LogicalSessionId& lsid,
                                           TxnNumber txnNumber) {
    auto applyOpsInfo = repl::ApplyOpsCommandInfo::parse(op.getObject());
    uassert(ErrorCodes::OplogOperationUnsupported,
            str::stream() << "Commands within applyOps are not supported during resharding: "
                          << redact(op.toBSONForLogging()),
            applyOpsInfo.areOpsCrudOnly());

    auto unrolledOp = uassertStatusOK(repl::MutableOplogEntry::parse(op.getEntry().toBSON()));
    unrolledOp.setSessionId(lsid);
    unrolledOp.setTxnNumber(txnNumber);
    unrolledOp.setMultiOpType(boost::none);

    for (const auto& innerOp : applyOpsInfo.getOperations()) {
        auto replOp = repl::ReplOperation::parse(
            innerOp, IDLParserContext{"unrollApplyOpsAndUpdateSessionTracker innerOp"});
        if (replOp.getStatementIds().empty()) {
            // Skip this operation since it is not retryable.
            continue;
        }
        unrolledOp.setDurableReplOperation(replOp);

        // There isn't a direct way to convert from a MutableOplogEntry to a
        // DurableOplogEntry or OplogEntry. We serialize the unrolledOp to have it get
        // re-parsed into an OplogEntry.
        auto& derivedOp = derivedOps.emplace_back(unrolledOp.toBSON());
        invariant(derivedOp.isCrudOpType() ||
                  isWouldChangeOwningShardSentinelOplogEntry(unrolledOp));

        // `&derivedOp` is guaranteed to remain stable while we append more derived
        // oplog entries because `derivedOps` is a std::list.
        updateSessionTracker(sessionTracker, &derivedOp);
    }
}

WriterVectors ReshardingOplogBatchPreparer::makeSessionOpWriterVectors(
    const OplogBatchToPrepare& batch, std::list<OplogEntry>& derivedOps) const {
    auto writerVectors = _makeEmptyWriterVectors();

    LogicalSessionIdMap<SessionOpsList> sessionTracker;

    for (auto& op : batch) {
        if (op.isCrudOpType()) {
            if (op.getSessionId()) {
                updateSessionTracker(sessionTracker, &op);
            }
        } else if (op.isCommand()) {
            throwIfUnsupportedCommandOp(op);

            if (!shouldUpdateTxnTable(op)) {
                continue;
            }

            const auto& sessionId = *op.getSessionId();

            if (op.getMultiOpType() == repl::MultiOplogEntryType::kApplyOpsAppliedSeparately) {
                unrollApplyOpsAndUpdateSessionTracker(
                    sessionTracker, derivedOps, op, sessionId, *op.getTxnNumber());
            } else if (isInternalSessionForRetryableWrite(sessionId) &&
                       op.getCommandType() == OplogEntry::CommandType::kApplyOps) {
                unrollApplyOpsAndUpdateSessionTracker(sessionTracker,
                                                      derivedOps,
                                                      op,
                                                      *getParentSessionId(sessionId),
                                                      *sessionId.getTxnNumber());
            } else {
                updateSessionTracker(sessionTracker, &op);
            }
        } else {
            invariant(repl::OpTypeEnum::kNoop == op.getOpType());
        }
    }

    for (auto& [lsid, opList] : sessionTracker) {
        for (auto& op : opList.ops) {
            _appendSessionOpToWriterVector(lsid, op, writerVectors);
        }
    }

    return writerVectors;
}

WriterVectors ReshardingOplogBatchPreparer::_makeEmptyWriterVectors() const {
    return WriterVectors(_oplogBatchTaskCount);
}

void ReshardingOplogBatchPreparer::_appendCrudOpToWriterVector(const OplogEntry* op,
                                                               WriterVectors& writerVectors) const {
    BSONElementComparator elementHasher{BSONElementComparator::FieldNamesMode::kIgnore,
                                        _defaultCollator.get()};
    if (_isCapped) {
        _appendOpToWriterVector(absl::HashOf(op->getNss()), op, writerVectors);
    } else {
        const auto idHash = elementHasher.hash(op->getIdElement());
        _appendOpToWriterVector(absl::HashOf(idHash), op, writerVectors);
    }
}

void ReshardingOplogBatchPreparer::_appendSessionOpToWriterVector(
    const LogicalSessionId& lsid, const OplogEntry* op, WriterVectors& writerVectors) const {
    LogicalSessionIdHash lsidHasher;
    _appendOpToWriterVector(lsidHasher(lsid), op, writerVectors);
}

void ReshardingOplogBatchPreparer::_appendOpToWriterVector(size_t hash,
                                                           const OplogEntry* op,
                                                           WriterVectors& writerVectors) const {
    auto& writer = writerVectors[hash % writerVectors.size()];
    if (writer.empty()) {
        // Skip a few growth rounds in anticipation that we'll be appending more.
        writer.reserve(8U);
    }
    writer.emplace_back(op);
}

}  // namespace mongo
