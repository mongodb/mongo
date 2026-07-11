// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/recordid_deduplicator.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace update {

/**
 * Generate a new document based on an update modification using an UpdateDriver.
 */
void generateNewDocumentFromUpdateOp(OperationContext* opCtx,
                                     const FieldRefSet& immutablePaths,
                                     UpdateDriver* driver,
                                     mutablebson::Document& document);

/**
 * Generate a new document based on the supplied upsert document.
 */
void generateNewDocumentFromSuppliedDoc(OperationContext* opCtx,
                                        const FieldRefSet& immutablePaths,
                                        const UpdateRequest* request,
                                        mutablebson::Document& document);

/**
 * Use an UpdateDriver and UpdateRequest to produce the document to insert.
 **/
void produceDocumentForUpsert(OperationContext* opCtx,
                              const UpdateRequest* request,
                              UpdateDriver* driver,
                              const CanonicalQuery* cq,
                              const FieldRefSet& immutablePaths,
                              mutablebson::Document& doc);

void ensureIdFieldIsFirst(mutablebson::Document* doc, bool generateOIDIfMissing);
void assertPathsNotArray(const mutablebson::Document& document, const FieldRefSet& paths);

/**
 * Parse FindAndModify update command request into an updateRequest.
 */
void makeUpdateRequest(OperationContext* opCtx,
                       const write_ops::FindAndModifyCommandRequest& request,
                       boost::optional<ExplainOptions::Verbosity> explain,
                       UpdateRequest* requestOut);

class ShardingChecksForUpdate {
public:
    ShardingChecksForUpdate(const CollectionAcquisition& collAcq,
                            OptionalBool allowShardKeyUpdatesWithoutFullKey,
                            bool isMulti,
                            CanonicalQuery* cq)
        : _collAcq(collAcq),
          _allowShardKeyUpdatesWithoutFullShardKeyInQuery(allowShardKeyUpdatesWithoutFullKey),
          _isMulti(isMulti),
          _canonicalQuery(cq) {};

    /**
     * Performs checks on whether the existing or new shard key fields would change the owning
     * shard, including whether the owning shard under the current key pattern would change as a
     * result of the update, or if the destined recipient under the new shard key pattern from
     * resharding would change as a result of the update.
     *
     * Throws if the updated document does not have all of the shard key fields or no longer belongs
     * to this shard.
     *
     * Accepting a 'newObjCopy' parameter is a performance enhancement for updates which weren't
     * performed in-place to avoid rendering a full copy of the updated document multiple times.
     */
    void checkUpdateChangesShardKeyFields(OperationContext* opCtx,
                                          const mutablebson::Document& newDoc,
                                          const boost::optional<BSONObj>& newObjCopy,
                                          const Snapshotted<BSONObj>& oldObj);

private:
    /**
     * Checks that the updated doc has all required shard key fields and throws if it does not.
     *
     * Also checks if the updated doc still belongs to this node and throws if it does not. If the
     * doc no longer belongs to this shard, this means that one or more shard key field values have
     * been updated to a value belonging to a chunk that is not owned by this shard. We cannot apply
     * this update atomically.
     */
    void checkUpdateChangesExistingShardKey(OperationContext* opCtx,
                                            const mutablebson::Document& newDoc,
                                            const BSONObj& newObj,
                                            const Snapshotted<BSONObj>& oldObj);

    void checkUpdateChangesReshardingKey(OperationContext* opCtx,
                                         const BSONObj& newObj,
                                         const Snapshotted<BSONObj>& oldObj);

    void _checkRestrictionsOnUpdatingShardKeyAreNotViolated(
        OperationContext* opCtx,
        const ScopedCollectionDescription& collDesc,
        const FieldRefSet& shardKeyPaths);


    const CollectionAcquisition& _collAcq;
    const OptionalBool _allowShardKeyUpdatesWithoutFullShardKeyInQuery;
    const bool _isMulti;
    const CanonicalQuery* _canonicalQuery; /* can be nullptr */
};

typedef RecordIdDeduplicator RecordIdSet;

/**
 * Computes the result of applying mods to the document 'oldObj' at RecordId 'recordId' in memory,
 * then commits these changes to the database. Returns a possibly unowned copy of the newly-updated
 * version of the document.
 */
std::pair<BSONObj, bool> transformDocument(OperationContext* opCtx,
                                           const CollectionAcquisition& collection,
                                           const Snapshotted<BSONObj>& oldObj,
                                           mutablebson::Document& doc,
                                           bool isUserInitiatedWrite,
                                           CanonicalQuery* cq,
                                           const RecordId& rid,
                                           UpdateDriver* driver,
                                           const UpdateRequest* request,
                                           bool shouldWriteToOrphan,
                                           RecordIdSet* _updatedRecordIds,
                                           const SeekableRecordCursor* cursor);

// TODO SERVER-118695 Support upsert requests
// Parse an oplog update, perform the transformation, and write the result to storage. This function
// cannot be used for upserts or requests that have a collation set.
// Returns the UpdateResult paired with the byte size of the post-image document. The size is read
// directly from the already-live buffer (zero allocation) and is always the size of the document
// after the update regardless of the ReturnDocOption set on the request.
[[MONGO_MOD_PUBLIC]] std::pair<UpdateResult, int> parseAndTransformOplogUpdate(
    OperationContext* opCtx,
    const CollectionAcquisition& coll,
    const Snapshotted<BSONObj>& oldObj,
    const UpdateRequest& request,
    const RecordId& rid,
    const SeekableRecordCursor* cursor);
}  // namespace update
}  // namespace mongo
