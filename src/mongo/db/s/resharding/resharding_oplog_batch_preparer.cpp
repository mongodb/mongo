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

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_oplog_batch_preparer.h"

#include <third_party/murmurhash3/MurmurHash3.h>

#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

using WriterVectors = ReshardingOplogBatchPreparer::WriterVectors;

ReshardingOplogBatchPreparer::ReshardingOplogBatchPreparer(
    std::unique_ptr<CollatorInterface> defaultCollator)
    : _defaultCollator(std::move(defaultCollator)) {}

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
        invariant(!op.isForReshardingSessionApplication());
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
                    {"ReshardingOplogBatchPreparer::makeCrudOpWriterVectors innerOp"}, innerOp));

                // There isn't a direct way to convert from a MutableOplogEntry to a
                // DurableOplogEntry or OplogEntry. We serialize the unrolledOp to have it get
                // re-parsed into an OplogEntry.
                auto& derivedOp = derivedOps.emplace_back(unrolledOp.toBSON());
                invariant(derivedOp.isCrudOpType());

                // `&derivedOp` is guaranteed to remain stable while we append more derived oplog
                // entries because `derivedOps` is a std::list.
                _appendCrudOpToWriterVector(&derivedOp, writerVectors);
            }
        } else {
            invariant(repl::OpTypeEnum::kNoop == op.getOpType());
        }
    }

    return writerVectors;
}

WriterVectors ReshardingOplogBatchPreparer::makeSessionOpWriterVectors(
    OplogBatchToPrepare& batch) const {
    auto writerVectors = _makeEmptyWriterVectors();

    struct SessionOpsList {
        TxnNumber txnNum = kUninitializedTxnNumber;
        std::vector<OplogEntry*> ops;
    };

    LogicalSessionIdMap<SessionOpsList> sessionTracker;

    for (auto& op : batch) {
        if (op.isCrudOpType()) {
            if (const auto& lsid = op.getSessionId()) {
                uassert(4990700,
                        str::stream() << "Missing txnNumber for oplog entry with lsid: "
                                      << redact(op.toBSONForLogging()),
                        op.getTxnNumber());

                auto txnNumber = *op.getTxnNumber();

                auto& retryableOpList = sessionTracker[*lsid];
                if (txnNumber == retryableOpList.txnNum) {
                    retryableOpList.ops.emplace_back(&op);
                } else if (txnNumber > retryableOpList.txnNum) {
                    retryableOpList.ops = {&op};
                    retryableOpList.txnNum = txnNumber;
                } else {
                    uasserted(4990401,
                              str::stream()
                                  << "Encountered out of order txnNumbers; batch had "
                                  << redact(op.toBSONForLogging()) << " after "
                                  << redact(retryableOpList.ops.back()->toBSONForLogging()));
                }
            }
        } else if (op.isCommand()) {
            throwIfUnsupportedCommandOp(op);

            // TODO SERVER-49905: Replace ops and update txnNum for the following cases:
            //   - a non-partialTxn:true, non-prepare:true applyOps entry = the final applyOps entry
            //     from a non-prepared transaction,
            //   - a commitTransaction oplog entry, or
            //   - an abortTransaction oplog entry.
        } else {
            invariant(repl::OpTypeEnum::kNoop == op.getOpType());
        }
    }

    for (auto& [lsid, opList] : sessionTracker) {
        for (auto& op : opList.ops) {
            op->setIsForReshardingSessionApplication();
            _appendSessionOpToWriterVector(lsid, op, writerVectors);
        }
    }

    return writerVectors;
}

WriterVectors ReshardingOplogBatchPreparer::_makeEmptyWriterVectors() const {
    return WriterVectors(size_t(resharding::gReshardingWriterThreadCount));
}

void ReshardingOplogBatchPreparer::_appendCrudOpToWriterVector(const OplogEntry* op,
                                                               WriterVectors& writerVectors) const {
    BSONElementComparator elementHasher{BSONElementComparator::FieldNamesMode::kIgnore,
                                        _defaultCollator.get()};

    const size_t idHash = elementHasher.hash(op->getIdElement());

    uint32_t hash = 0;
    MurmurHash3_x86_32(&idHash, sizeof(idHash), hash, &hash);

    _appendOpToWriterVector(hash, op, writerVectors);
}

void ReshardingOplogBatchPreparer::_appendSessionOpToWriterVector(
    const LogicalSessionId& lsid, const OplogEntry* op, WriterVectors& writerVectors) const {
    LogicalSessionIdHash lsidHasher;
    _appendOpToWriterVector(lsidHasher(lsid), op, writerVectors);
}

void ReshardingOplogBatchPreparer::_appendOpToWriterVector(std::uint32_t hash,
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
