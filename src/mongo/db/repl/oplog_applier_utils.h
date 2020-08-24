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

#include "mongo/db/repl/insert_group.h"

namespace mongo {
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
        const CollatorInterface* collator = nullptr;
    };

    CollectionProperties getCollectionProperties(OperationContext* opCtx,
                                                 const StringMapHashedKey& ns);

private:
    CollectionProperties getCollectionPropertiesImpl(OperationContext* opCtx,
                                                     const NamespaceString& nss);

    StringMap<CollectionProperties> _cache;
};

/**
 * This class contains some static methods common to ordinary oplog application and oplog
 * application as part of tenant migration.
 */
class OplogApplierUtils {
public:
    /**
     * Specially sort collections that are $cmd first, before everything else.  This will
     * move commands with the special $cmd collection name to the beginning, rather than sorting
     * them potentially in the middle of the sorted vector of insert/update/delete ops.
     * This special sort behavior is required because DDL operations need to run before
     * create/update/delete operations in a multi-doc transaction.
     */
    static void stableSortByNamespace(std::vector<const OplogEntry*>* oplogEntryPointers);

    /**
     * Updates a CRUD op's hash and isForCappedCollection field if necessary.
     */
    static void processCrudOp(OperationContext* opCtx,
                              OplogEntry* op,
                              uint32_t* hash,
                              StringMapHashedKey* hashedNs,
                              CachedCollectionProperties* collPropertiesCache);


    /**
     * Adds a single oplog entry to the appropriate writer vector.  Returns the index of the
     * writer vector the entry was written to.
     */
    static uint32_t addToWriterVector(OperationContext* opCtx,
                                      OplogEntry* op,
                                      std::vector<std::vector<const OplogEntry*>>* writerVectors,
                                      CachedCollectionProperties* collPropertiesCache,
                                      boost::optional<uint32_t> forceWriterId = boost::none);
    /**
     * Adds a set of derivedOps to writerVectors.
     * If `serial` is true, assign all derived operations to the writer vector corresponding to the
     * hash of the first operation in `derivedOps`.
     */
    static void addDerivedOps(OperationContext* opCtx,
                              std::vector<OplogEntry>* derivedOps,
                              std::vector<std::vector<const OplogEntry*>>* writerVectors,
                              CachedCollectionProperties* collPropertiesCache,
                              bool serial);

    /**
     * Returns the namespace string for this oplogEntry; if it has a UUID it looks up the
     * corresponding namespace and returns it, otherwise it returns the oplog entry 'nss'.  If there
     * is a UUID and no namespace with that ID is found, throws NamespaceNotFound.
     */
    static NamespaceString parseUUIDOrNs(OperationContext* opCtx, const OplogEntry& oplogEntry);

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
        IncrementOpsAppliedStatsFn incrementOpsAppliedStats,
        OpCounters* opCounters);

    /**
     * The logic for oplog batch application which is shared between standard and tenant oplog
     * application.
     */
    static Status applyOplogBatchCommon(
        OperationContext* opCtx,
        std::vector<const OplogEntry*>* ops,
        OplogApplication::Mode oplogApplicationMode,
        bool allowNamespaceNotFoundErrorsOnCrudOps,
        InsertGroup::ApplyFunc applyOplogEntryOrGroupedInserts) noexcept;
};

}  // namespace repl
}  // namespace mongo
