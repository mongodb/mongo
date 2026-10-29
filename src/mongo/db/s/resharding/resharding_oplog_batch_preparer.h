// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <list>
#include <memory>
#include <vector>

namespace mongo {

class CollatorInterface;
class LogicalSessionId;

/**
 * Converts a batch of oplog entries to be applied into multiple batches of oplog entries that may
 * be applied concurrently by different threads.
 *
 * Instances of this class are thread-safe.
 */
class ReshardingOplogBatchPreparer {
private:
    using OplogEntry = repl::OplogEntry;

public:
    ReshardingOplogBatchPreparer(std::size_t oplogBatchTaskCount,
                                 std::unique_ptr<CollatorInterface> defaultCollator,
                                 bool isCapped = false);

    using OplogBatchToPrepare = std::vector<OplogEntry>;
    using OplogBatchToApply = std::vector<const OplogEntry*>;
    using WriterVectors = std::vector<OplogBatchToApply>;

    /**
     * Prepares a batch of oplog entries for CRUD application by multiple threads concurrently.
     *
     * The returned writer vectors guarantee that modifications to the same document (as identified
     * by its _id) will be in the same writer vector and will appear in their corresponding `batch`
     * order.
     *
     * The returned writer vectors refer to memory owned by `batch` and `derivedOps`. The caller
     * must take care to ensure both `batch` and `derivedOps` outlive the writer vectors all being
     * applied and must take care not to modify `batch` or `derivedOps` until after the writer
     * vectors have all been applied. In particular, `makeSessionOpWriterVectors(batch)` must not be
     * called until after the returned writer vectors have all been applied.
     */
    WriterVectors makeCrudOpWriterVectors(const OplogBatchToPrepare& batch,
                                          std::list<OplogEntry>& derivedOps) const;

    /**
     * Prepares a batch of oplog entries for session application by multiple threads concurrently.
     *
     * The returned writer vectors guarantee that modifications to the same config.transactions
     * records (as identified by its lsid) will be in the same writer vector. Additionally, updates
     * to the config.transactions record for a higher txnNumber will cause any updates in `batch`
     * for lower txnNumbers to be elided.
     *
     * The returned writer vectors refer to memory owned by `batch` and `derivedOps`. The caller
     * must take care to ensure both `batch` and `derivedOps` outlive the writer vectors all being
     * applied and must take care not to modify `batch` or `derivedOps` until after the writer
     * vectors have all been applied.
     */
    WriterVectors makeSessionOpWriterVectors(const OplogBatchToPrepare& batch,
                                             std::list<OplogEntry>& derivedOps) const;

    static void throwIfUnsupportedCommandOp(const OplogEntry& op);

private:
    WriterVectors _makeEmptyWriterVectors() const;

    void _appendCrudOpToWriterVector(const OplogEntry* op, WriterVectors& writerVectors) const;

    void _appendSessionOpToWriterVector(const LogicalSessionId& lsid,
                                        const OplogEntry* op,
                                        WriterVectors& writerVectors) const;

    void _appendOpToWriterVector(size_t hash,
                                 const OplogEntry* op,
                                 WriterVectors& writerVectors) const;

    const std::size_t _oplogBatchTaskCount;
    const std::unique_ptr<CollatorInterface> _defaultCollator;
    const bool _isCapped;
};

}  // namespace mongo
