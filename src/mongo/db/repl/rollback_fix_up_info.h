/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <utility>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/uuid.h"

namespace mongo {

class BSONElement;
class OperationContext;

namespace repl {

class StorageInterface;

/**
 * RollbackFixUpInfo represents information derived from processing local oplog entries that have to
 * be rolled back. This information may be supplemented by requesting additional data from the sync
 * source if the local oplog entry is not reversible on its own. For example, when rolling back a
 * document deletion, the oplog entry contains the _id of the deleted document but not the complete
 * document that was removed. We have to request the most recent copy of the document by collection
 * UUID and document _id in order to restore the deleted document.
 *
 * This rollback fix-up information is persisted in "local.system.rollback.*" collections. See
 * description of "kRollback*Namespace" constants.
 */
class RollbackFixUpInfo {
private:
    MONGO_DISALLOW_COPYING(RollbackFixUpInfo);

public:
    /**
     * Contains documents affected by rolling back CRUD operations on documents.
     */
    static const NamespaceString kRollbackDocsNamespace;

    /**
     * Contains mappings of collection UUID -> namespace.
     * This collection is used to roll back create, drop and rename operations.
     * For drops and renames, the namespace in the mapping will be the full namespace to restore
     * in the database catalog.
     * For collection creation, the namespace will be set to empty to indicate that the collection
     * should be dropped.
     */
    static const NamespaceString kRollbackCollectionUuidNamespace;

    /**
     * Creates an instance of RollbackFixUpInfo.
     */
    explicit RollbackFixUpInfo(StorageInterface* storageInterface);

    /**
     * Processes an oplog entry representing an insert/delete/update operation on a single document
     * in a collection. Stores information about this operation into "kRollbackDocsNamespace".
     * This allows us to roll back the operation later.
     * For delete and update operations, we will also fetch a more recent copy of the document
     * affected by this operation and store it in the same collection.
     *
     * "docId" is the _id field of the modified document.
     *
     * For index creation operations, which are represented in the oplog as insert operations in
     * "*.system.indexes", use processCreateIndexOplogEntry() instead.
     */
    enum class SingleDocumentOpType { kInsert, kDelete, kUpdate };
    class SingleDocumentOperationDescription;
    Status processSingleDocumentOplogEntry(OperationContext* opCtx,
                                           const UUID& collectionUuid,
                                           const BSONElement& docId,
                                           SingleDocumentOpType opType);

    /**
     * Processes an oplog entry representing a create collection command. Stores information about
     * this operation into "kRollbackCollectionUuidNamespace" to allow us to roll back this
     * operation later by dropping the collection from the catalog by UUID.
     *
     * The mapping in the "kRollbackCollectionUuidNamespace" collection will contain the
     * empty namespace.
     */
    class CollectionUuidDescription;
    Status processCreateCollectionOplogEntry(OperationContext* opCtx, const UUID& collectionUuid);

    /**
     * Processes an oplog entry representing a drop collection command. Stores information about
     * this operation into "kRollbackCollectionUuidNamespace" to allow us to roll back this
     * operation later by restoring the UUID mapping in the catalog.
     */
    Status processDropCollectionOplogEntry(OperationContext* opCtx,
                                           const UUID& collectionUuid,
                                           const NamespaceString& nss);

    /**
     * Processes an oplog entry representing a rename collection command. Stores information about
     * this operation into "kRollbackCollectionUuidNamespace" to allow us to roll back this
     * operation later by restoring the UUID mapping(s) in the catalog.
     *
     * "targetCollectionUuidAndNss" is derived from the "dropTarget" and "to" fields of the
     * renameCollection oplogEntry.
     *
     * If the target collection did not exist when the rename operation was applied
     * (targetCollectionUuidAndNss is valid), this function will insert one document into
     * "kRollbackCollectionUuidNamespace":
     *     source UUID -> namespace mapping.
     *
     * If the target collection was dropped when the rename operation was applied
     * (targetCollectionUuidAndNss is valid), this function will insert two documents into
     * "kRollbackCollectionUuidNamespace":
     *     source UUID -> namespace mapping; and
     *     dropped target UUID -> namespace mapping
     */
    using CollectionUuidAndNss = std::pair<UUID, NamespaceString>;
    Status processRenameCollectionOplogEntry(
        OperationContext* opCtx,
        const UUID& sourceCollectionUuid,
        const NamespaceString& sourceNss,
        boost::optional<CollectionUuidAndNss> targetCollectionUuidAndNss);

private:
    /**
     * Upserts a single document using the _id field of the document in "update".
     */
    Status _upsertById(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& update);

    StorageInterface* const _storageInterface;
};

}  // namespace repl
}  // namespace mongo
