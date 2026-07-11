// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/update_result.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

#include <memory>

namespace [[MONGO_MOD_PUBLIC]] mongo {
/**
 * db helpers are helper functions and classes that let us easily manipulate the local
 * database instance in-proc.
 *
 * all helpers assume locking is handled above them
 */
struct Helpers {
    /**
     * Executes the given match expression ('query') and returns true if there is at least one
     * matching document. The first found matching document is returned via the 'result' output
     * parameter.
     *
     * Performs the read successfully regardless of a replica set node's state, meaning that the
     * node does not need to be primary or secondary.
     */
    static bool findOne(OperationContext* opCtx,
                        const CollectionAcquisition& collection,
                        const BSONObj& query,
                        BSONObj& result);

    /**
     * If `invariantOnError` is true, an error (e.g: no document found) will crash the process.
     * Otherwise the empty BSONObj will be returned.
     */
    static BSONObj findOneForTesting(OperationContext* opCtx,
                                     const CollectionAcquisition& collection,
                                     const BSONObj& query,
                                     bool invariantOnError = true);

    /**
     * Similar to the 'findOne()' overload above, except returns the RecordId of the first matching
     * document, or a null RecordId if no such document exists.
     */
    static RecordId findOne(OperationContext* opCtx,
                            const CollectionAcquisition& collection,
                            const BSONObj& query);
    static RecordId findOne(OperationContext* opCtx,
                            const CollectionAcquisition& collection,
                            std::unique_ptr<FindCommandRequest> qr);

    /**
     * Returns true if a matching document was found.
     */
    static bool findById(OperationContext* opCtx,
                         const NamespaceString& nss,
                         BSONObj query,
                         BSONObj& result);

    /* TODO: should this move into Collection?
     * uasserts if no _id index.
     * @return null loc if not found */
    static RecordId findById(OperationContext* opCtx,
                             const CollectionPtr& collection,
                             const BSONObj& query);

    /**
     * Get the first object generated from a forward natural-order scan on "ns".  Callers do not
     * have to lock "ns".
     *
     * Returns true if there is such an object.  An owned copy of the object is placed into the
     * out-argument "result".
     *
     * Returns false if there is no such object.
     */
    static bool getSingleton(OperationContext* opCtx, const NamespaceString& nss, BSONObj& result);

    /**
     * Same as getSingleton, but with a reverse natural-order scan on "ns".
     */
    static bool getLast(OperationContext* opCtx, const NamespaceString& nss, BSONObj& result);

    /**
     * Performs an upsert of "obj" into the collection "ns", with an empty update predicate.
     * Callers must have "ns" locked.
     */
    static void putSingleton(OperationContext* opCtx, CollectionAcquisition& coll, BSONObj obj);

    /**
     * Callers are expected to hold the collection lock.
     * you do not have to have Context set
     * o has to have an _id field or will assert
     */
    static UpdateResult upsert(OperationContext* opCtx,
                               CollectionAcquisition& coll,
                               const BSONObj& o,
                               bool fromMigrate = false);

    /**
     * Performs an upsert of 'updateMod' if we don't match the given 'filter'.
     * Callers are expected to hold the collection lock.
     * Note: Query yielding is turned off, so both read and writes are performed
     * on the same storage snapshot.
     */
    static UpdateResult upsert(OperationContext* opCtx,
                               CollectionAcquisition& coll,
                               const BSONObj& filter,
                               const BSONObj& updateMod,
                               bool fromMigrate = false);

    /**
     * Performs an update of 'updateMod' for the entry matching the given 'filter'.
     * Callers are expected to hold the collection lock.
     * Note: Query yielding is turned off, so both read and writes are performed
     * on the same storage snapshot.
     */
    static void update(OperationContext* opCtx,
                       CollectionAcquisition& coll,
                       const BSONObj& filter,
                       const BSONObj& updateMod,
                       bool fromMigrate = false);

    /**
     * Inserts document 'doc' into collection 'coll'. Does not validate or modify the document in
     * any way, and it is the caller's responsibility to handle things like adding the _id field.
     */
    static Status insert(OperationContext* opCtx, const CollectionPtr& coll, const BSONObj& doc);

    /**
     * Inserts a batch of documents 'docs' into collection 'coll'. Does not validate or modify the
     * documents in any way, and it is the caller's responsibility to handle things like adding the
     * _id field.
     */
    static Status insert(OperationContext* opCtx,
                         const CollectionPtr& coll,
                         std::span<const BSONObj> docs);

    /**
     * Deletes document from collection 'coll' via RecordId 'rid'.
     */
    static void deleteByRid(OperationContext* opCtx,
                            const CollectionAcquisition& coll,
                            RecordId rid);

    // TODO: this should be somewhere else probably
    /* Takes object o, and returns a new object with the
     * same field elements but the names stripped out.
     * Example:
     *    o = {a : 5 , b : 6} --> {"" : 5, "" : 6}
     */
    static BSONObj toKeyFormat(const BSONObj& o);

    /* Takes object o, and infers an ascending keyPattern with the same fields as o
     * Example:
     *    o = {a : 5 , b : 6} --> {a : 1 , b : 1 }
     */
    static BSONObj inferKeyPattern(const BSONObj& o);

    /**
     * Remove all documents from a collection.
     * You do not need to set the database before calling.
     * Does not oplog the operation.
     */
    static void emptyCollection(OperationContext* opCtx, const CollectionAcquisition& coll);

    /*
     * Finds the doc and then runs a no-op update by running an update using the doc just read. Used
     * in order to force a conflict if a concurrent storage transaction writes to the doc we're
     * reading.
     *
     * This no-op update has no associated timestamp. This results in a mixed-mode update chain
     * within WT that is problematic with durable history. Callers must not commit the
     * `WriteUnitOfWork` when there are no other writes that set the timestamp.
     *
     * Callers must hold the collection lock in MODE_IX.
     * Uasserts if no _id index.
     * Returns true if object found
     */
    static bool findByIdAndNoopUpdate(OperationContext* opCtx,
                                      const CollectionPtr& collection,
                                      const BSONObj& idQuery,
                                      BSONObj& result);
};

}  // namespace mongo
