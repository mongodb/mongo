/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/insert_group.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace MONGO_MOD_PUB mongo {
class CollatorInterface;

class OpCounters;

namespace repl {

/**
 * Caches per-collection properties which are relevant for oplog application, so that they don't
 * have to be retrieved repeatedly for each op.
 */
class CachedCollectionProperties {
public:
    struct CollectionProperties {
        bool isCapped = false;
        bool isClustered = false;
        const CollatorInterface* collator = nullptr;
    };

    CollectionProperties getCollectionProperties(OperationContext* opCtx,
                                                 const NamespaceString& nss);

private:
    stdx::unordered_map<NamespaceString, CollectionProperties> _cache;
};

/**
 * This class contains some static methods common to ordinary oplog application.
 */
class OplogApplierUtils {
public:
    /*
     * Returns the hash of the oplog entry based on the namespace string (and document
     * key if exists), which is used to determine the thread to apply the op.
     */
    static uint32_t getOplogEntryHash(OperationContext* opCtx,
                                      OplogEntry* op,
                                      CachedCollectionProperties* collPropertiesCache);

    /**
     * Sort operations by their namespaces. There are some special rules for sorting:
     *
     * 1. Prepared transaction commands (prepare/commit/abort) act as delimiters, which
     *    creates partitions in the ops vector so that any reordering can only happen
     *    within each partition but not across partitions.
     * 2. Within each partition, DDL ops (with the '$cmd' collection name) are ordered
     *    before all other CRUD ops since DDL ops need to be run before CRUD ops.
     *
     * As an example, the right side below is a possible output after calling this:
     * [                                         [
     *     insert(ns2, docA),                        create(ns1),          -
     *     create(ns1),                              insert(ns1, docB),    |
     *     insert(ns1, docB),          sort          insert(ns1, docC),    |- sorted
     *     update(ns2, docA),       ==========>      insert(ns2, docA),    |
     *     insert(ns1, docC),                        update(ns2, docA),    -
     *     prepare(sess1, txn1),                     prepare(sess1, txn1),
     *     insert(ns1, docD),                        insert(ns1, docD),    -
     *     delete(ns2, docA),                        update(ns1, docB),    |- sorted
     *     update(ns1, docB),                        delete(ns2, docA),    -
     * ]                                         ]
     */
    static void stableSortByNamespace(std::vector<ApplierOperation>* ops);

    /**
     * Adds a single oplog entry to the appropriate writer vector. Returns the index of the
     * writer vector the entry was written to.
     */
    static uint32_t addToWriterVector(OperationContext* opCtx,
                                      OplogEntry* op,
                                      std::vector<std::vector<ApplierOperation>>* writerVectors,
                                      CachedCollectionProperties* collPropertiesCache,
                                      boost::optional<uint32_t> forceWriterId = boost::none);

    /**
     * Adds a set of derivedOps to writerVectors. For ops derived from prepared transactions, the
     * addDerivedPrepares() variant should be used.
     *
     * If `serial` is true, assign all derived operations to the writer vector corresponding to the
     * hash of the first operation in `derivedOps`.
     */
    static void addDerivedOps(OperationContext* opCtx,
                              std::vector<OplogEntry>* derivedOps,
                              std::vector<std::vector<ApplierOperation>>* writerVectors,
                              CachedCollectionProperties* collPropertiesCache,
                              bool serial);

    /**
     * Adds a set of derived prepared transaction operations to writerVectors.
     *
     * If `serial` is true, assign all derived operations to the writer vector corresponding to the
     * hash of the first operation in `derivedOps`.
     *
     * The prepareOp and derivedOps are inputs that we use to generate ApplierOperation's to be
     * added to the writerVectors. The derivedOps contains all the CRUD ops inside the applyOps
     * part of the prepareOp. When this function finishes the writerVectors may look like this:
     *
     * ========================== for non-empty prepared transaction ==========================
     * writer vector 1: [ ]
     * writer vector 2: [
     *     ApplierOperation{
     *         op: prepareOp,
     *         instruction: applySplitPreparedTxnOp,
     *         subSession: <split_session_id_1>,
     *         splitPrepareOps: [ crud_op_1, crud_op_3 ],
     *     },
     * ]
     * writer vector 3: [
     *     // This op should already exist in the writerVector prior to this function call.
     *     ApplierOperation{ op: <config.transaction_update_op>, instruction: applyOplogEntry },
     * ]
     * writer vector 4: [
     *     ApplierOperation{
     *         op: prepareOp,
     *         instruction: applySplitPreparedTxnOp,
     *         subSession: <split_session_id_2>,
     *         splitPrepareOps: [ crud_op_2, crud_op_4 ],
     *     },
     * ]
     * ============================ for empty prepared transaction ============================
     * writer vector 1: [ ]
     * writer vector 2: [
     *     // This op should already exist in the writerVector prior to this function call.
     *     ApplierOperation{ op: <config.transaction_update_op>, instruction: applyOplogEntry },
     * ]
     * writer vector 3: [ ]
     * writer vector 4: [
     *     // The splitPrepareOps list is made empty, which we can still correctly apply.
     *     ApplierOperation{
     *         op: <original_prepare_op>,
     *         instruction: applySplitPreparedTxnOp,
     *         subSession: <split_session_id_1>,
     *         splitPrepareOps: [ ],
     *     },
     * ]
     */
    static void addDerivedPrepares(OperationContext* opCtx,
                                   OplogEntry* prepareOp,
                                   std::vector<OplogEntry>* derivedOps,
                                   std::vector<std::vector<ApplierOperation>>* writerVectors,
                                   CachedCollectionProperties* collPropertiesCache,
                                   bool shouldSerialize);

    /**
     * Adds commit or abort transaction operations to the writerVectors.
     *
     * The commitOrAbortOp is the input that we use to generate ApplierOperation's to be added
     * to those writerVectors that previously got assigned the prepare ops. When this function
     * finishes the writerVectors may look like this:
     *
     * writer vector 1: [ ]
     * writer vector 2: [
     *     ApplierOperation{
     *         op: commitOrAbortOp,
     *         instruction: applySplitPreparedTxnOp,
     *         subSession: <split_session_id_1>,
     *         splitPrepareOps: [ ],
     *     },
     * ]
     * writer vector 3: [
     *     // This op should already exist in the writerVector prior to this function call.
     *     ApplierOperation{ op: <config.transaction_update_op>, instruction: applyOplogEntry },
     * ]
     * writer vector 4: [
     *     ApplierOperation{
     *         op: commitOrAbortOp,
     *         instruction: applySplitPreparedTxnOp,
     *         subSession: <split_session_id_2>,
     *         splitPrepareOps: [ ],
     *     },
     * ]
     */
    static void addDerivedCommitsOrAborts(OperationContext* opCtx,
                                          OplogEntry* commitOrAbortOp,
                                          std::vector<std::vector<ApplierOperation>>* writerVectors,
                                          CachedCollectionProperties* collPropertiesCache);

    /**
     * If the oplog entry has a UUID, returns the UUID with the database from 'nss'.  Otherwise
     * returns 'nss'
     */
    static NamespaceStringOrUUID getNsOrUUID(const NamespaceString& nss, const OplogEntry& op);

    /**
     * The logic for oplog entry application which is shared between standard and tenant oplog
     * application.
     */
    static Status applyOplogEntryOrGroupedInsertsCommon(
        OperationContext* opCtx,
        const OplogEntryOrGroupedInserts& entryOrGroupedInserts,
        OplogApplication::Mode oplogApplicationMode,
        bool isDataConsistent,
        IncrementOpsAppliedStatsFn incrementOpsAppliedStats,
        OpCounters* opCounters);

    /**
     * The logic for oplog batch application which is shared between standard and tenant oplog
     * application.
     */
    static Status applyOplogBatchCommon(
        OperationContext* opCtx,
        std::vector<ApplierOperation>* ops,
        OplogApplication::Mode oplogApplicationMode,
        bool allowNamespaceNotFoundErrorsOnCrudOps,
        bool isDataConsistent,
        InsertGroup::ApplyFunc applyOplogEntryOrGroupedInserts) noexcept;
};

}  // namespace repl
}  // namespace MONGO_MOD_PUB mongo
