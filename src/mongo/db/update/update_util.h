/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/explain_options.h"
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

typedef stdx::unordered_set<RecordId, RecordId::Hasher> RecordIdSet;

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
}  // namespace update
}  // namespace mongo
