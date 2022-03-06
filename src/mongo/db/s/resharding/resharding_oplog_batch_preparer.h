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

#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <vector>

#include "mongo/db/repl/oplog_entry.h"

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
    ReshardingOplogBatchPreparer(std::unique_ptr<CollatorInterface> defaultCollator);

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
     * The returned writer vectors refer to memory owned by `batch`. The caller must take care to
     * ensure `batch` outlives the writer vectors all being applied and must take care not to modify
     * `batch` until after the writer vectors have all been applied.
     */
    WriterVectors makeSessionOpWriterVectors(const OplogBatchToPrepare& batch) const;

    static void throwIfUnsupportedCommandOp(const OplogEntry& op);

private:
    WriterVectors _makeEmptyWriterVectors() const;

    void _appendCrudOpToWriterVector(const OplogEntry* op, WriterVectors& writerVectors) const;

    void _appendSessionOpToWriterVector(const LogicalSessionId& lsid,
                                        const OplogEntry* op,
                                        WriterVectors& writerVectors) const;

    void _appendOpToWriterVector(std::uint32_t hash,
                                 const OplogEntry* op,
                                 WriterVectors& writerVectors) const;

    const std::unique_ptr<CollatorInterface> _defaultCollator;
};

}  // namespace mongo
