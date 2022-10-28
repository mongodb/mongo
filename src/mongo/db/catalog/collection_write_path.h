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

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"

namespace mongo {
namespace collection_internal {

using OnRecordInsertedFn = std::function<Status(const RecordId& loc)>;

/**
 * Inserts a document into the record store for a bulk loader that manages the index building. The
 * bulk loader is notified with the RecordId of the document inserted into the RecordStore through
 * the 'OnRecordInsertedFn' callback.
 *
 * NOTE: It is up to caller to commit the indexes.
 */
Status insertDocumentForBulkLoader(OperationContext* opCtx,
                                   const CollectionPtr& collection,
                                   const BSONObj& doc,
                                   const OnRecordInsertedFn& onRecordInserted);

/**
 * Inserts all documents inside one WUOW.
 * Caller should ensure vector is appropriately sized for this.
 * If any errors occur (including WCE), caller should retry documents individually.
 *
 * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
 */
Status insertDocuments(OperationContext* opCtx,
                       const CollectionPtr& collection,
                       std::vector<InsertStatement>::const_iterator begin,
                       std::vector<InsertStatement>::const_iterator end,
                       OpDebug* opDebug,
                       bool fromMigrate = false);

/**
 * Does NOT modify the doc before inserting (i.e. will not add an _id field for documents that are
 * missing it)
 *
 * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
 */
Status insertDocument(OperationContext* opCtx,
                      const CollectionPtr& collection,
                      const InsertStatement& doc,
                      OpDebug* opDebug,
                      bool fromMigrate = false);

/**
 * Checks the 'failCollectionInserts' fail point at the beginning of an insert operation to see if
 * the insert should fail. Returns Status::OK if The function should proceed with the insertion.
 * Otherwise, the function should fail and return early with the error Status.
 */
Status checkFailCollectionInsertsFailPoint(const NamespaceString& ns, const BSONObj& firstDoc);

/**
 * Updates the document @ oldLocation with newDoc.
 *
 * If the document fits in the old space, it is put there; if not, it is moved.
 * Sets 'args.updatedDoc' to the updated version of the document with damages applied, on
 * success.
 * 'opDebug' Optional argument. When not null, will be used to record operation statistics.
 * @return the post update location of the doc (may or may not be the same as oldLocation)
 */
RecordId updateDocument(OperationContext* opCtx,
                        const CollectionPtr& collection,
                        const RecordId& oldLocation,
                        const Snapshotted<BSONObj>& oldDoc,
                        const BSONObj& newDoc,
                        bool indexesAffected,
                        OpDebug* opDebug,
                        CollectionUpdateArgs* args);

/**
 * Illegal to call if collection->updateWithDamagesSupported() returns false.
 * Sets 'args.updatedDoc' to the updated version of the document with damages applied, on
 * success.
 * Returns the contents of the updated document.
 */
StatusWith<BSONObj> updateDocumentWithDamages(OperationContext* opCtx,
                                              const CollectionPtr& collection,
                                              const RecordId& loc,
                                              const Snapshotted<BSONObj>& oldDoc,
                                              const char* damageSource,
                                              const mutablebson::DamageVector& damages,
                                              bool indexesAffected,
                                              OpDebug* opDebug,
                                              CollectionUpdateArgs* args);

}  // namespace collection_internal
}  // namespace mongo
