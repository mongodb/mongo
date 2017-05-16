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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rollback_fix_up_info.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/rollback_fix_up_info_descriptions.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace repl {

namespace {

const auto kRollbackNamespacePrefix = "local.system.rollback."_sd;

}  // namespace

const NamespaceString RollbackFixUpInfo::kRollbackDocsNamespace(kRollbackNamespacePrefix + "docs");

const NamespaceString RollbackFixUpInfo::kRollbackCollectionUuidNamespace(kRollbackNamespacePrefix +
                                                                          "collectionUuid");

const NamespaceString RollbackFixUpInfo::kRollbackCollectionOptionsNamespace(
    kRollbackNamespacePrefix + "collectionOptions");

const NamespaceString RollbackFixUpInfo::kRollbackIndexNamespace(kRollbackNamespacePrefix +
                                                                 "indexes");

RollbackFixUpInfo::RollbackFixUpInfo(StorageInterface* storageInterface)
    : _storageInterface(storageInterface) {
    invariant(storageInterface);
}

Status RollbackFixUpInfo::processSingleDocumentOplogEntry(OperationContext* opCtx,
                                                          const UUID& collectionUuid,
                                                          const BSONElement& docId,
                                                          SingleDocumentOpType opType,
                                                          const std::string& dbName) {
    SingleDocumentOperationDescription desc(collectionUuid, docId, opType, dbName);
    auto doc = desc.toBSON();
    if (SingleDocumentOpType::kInsert == opType) {
        // If the existing document (that may or may not exist in the "kRollbackDocsNamespace"
        // collection) has a 'delete' op type, this oplog entry will cancel out the previously
        // processed 'delete" oplog entry. We should remove the existing document from the
        // collection and not insert a new document.
        auto deleteResult =
            _storageInterface->deleteById(opCtx, kRollbackDocsNamespace, doc["_id"]);
        if (deleteResult.isOK()) {
            auto existingDoc = deleteResult.getValue();
            if ("delete" == existingDoc["operationType"].String()) {
                return Status::OK();
            }
            // Fall through and replace the 'update' op type in the existing document with 'insert'
            // so that the document will be dropped when we actually do the rollback.
        }
    } else if (SingleDocumentOpType::kUpdate == opType) {
        // If there is an existing document in the "kRollbackDocsNamespace" collection, it must
        // have either a 'delete' or 'update' op type.
        //
        // For a 'delete' entry, we should not replace it with 'update' so that if we process an
        // oplog entry with an 'insert' op type later, we can cancel out the existing entry with the
        // 'delete' op type.
        //
        // For an 'update' entry, there is nothing further to do because this matches the current op
        // type passed to this function.
        auto findResult = _storageInterface->findById(opCtx, kRollbackDocsNamespace, doc["_id"]);
        if (findResult.isOK()) {
            return Status::OK();
        }
        // No existing document. Insert a new document with 'update' op type.
    }
    return _upsertById(opCtx, kRollbackDocsNamespace, doc);
}

Status RollbackFixUpInfo::processCreateCollectionOplogEntry(OperationContext* opCtx,
                                                            const UUID& collectionUuid) {
    CollectionUuidDescription desc(collectionUuid, {});
    auto status = _upsertById(opCtx, kRollbackCollectionUuidNamespace, desc.toBSON());
    if (!status.isOK()) {
        return status;
    }

    // Remove all references to the collection in the create command from the other rollback
    // fix up collections. The documents to be removed will contain 'collectionUuid' as the _id
    // field or nested inside the _id field with the dotted field name '_id.collectionUuid'.

    // Generate key for nested _id field {_id: {collectionUuid: <collection uuid>, ...}, ...}.
    BSONObjBuilder nestedIdFilterBob;
    collectionUuid.appendToBuilder(&nestedIdFilterBob, "_id.collectionUuid");
    auto nestedIdFilter = nestedIdFilterBob.obj();

    // Remove documents from the "kRollbackDocsNamespace" collection with _id's containing
    // 'collectionUuid':
    //     {_id: {collectionUuid: <collectionUuid>, docId: <modified document _id>}, ...}
    status = _storageInterface->deleteByFilter(opCtx, kRollbackDocsNamespace, nestedIdFilter);
    if (!status.isOK()) {
        return status;
    }

    // Generate key for _id field {_id: <collection uuid>, ...}.
    BSONObjBuilder idFilterBob;
    collectionUuid.appendToBuilder(&idFilterBob, "_id");
    auto idFilter = idFilterBob.obj();

    // Remove documents from the "kRollbackCollectionOptionsNamespace" and "kRollbackIndexNamespace"
    // collections with the _id field set to 'collectionUuid'.
    //     {_id: <collectionUuid>, ...}
    status =
        _storageInterface->deleteByFilter(opCtx, kRollbackCollectionOptionsNamespace, idFilter);
    if (!status.isOK()) {
        return status;
    }

    status = _storageInterface->deleteByFilter(opCtx, kRollbackIndexNamespace, nestedIdFilter);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

Status RollbackFixUpInfo::processDropCollectionOplogEntry(OperationContext* opCtx,
                                                          const UUID& collectionUuid,
                                                          const NamespaceString& nss) {
    CollectionUuidDescription desc(collectionUuid, nss);
    return _upsertById(opCtx, kRollbackCollectionUuidNamespace, desc.toBSON());
}

Status RollbackFixUpInfo::processRenameCollectionOplogEntry(
    OperationContext* opCtx,
    const UUID& sourceCollectionUuid,
    const NamespaceString& sourceNss,
    boost::optional<CollectionUuidAndNss> targetCollectionUuidAndNss) {
    CollectionUuidDescription sourceDesc(sourceCollectionUuid, sourceNss);

    auto status = _upsertById(opCtx, kRollbackCollectionUuidNamespace, sourceDesc.toBSON());
    if (!status.isOK()) {
        return status;
    }

    // If target collection is not dropped during the rename operation, there is nothing further to
    // do.
    if (!targetCollectionUuidAndNss) {
        return Status::OK();
    }

    CollectionUuidDescription targetDesc(targetCollectionUuidAndNss->first,
                                         targetCollectionUuidAndNss->second);
    return _upsertById(opCtx, kRollbackCollectionUuidNamespace, targetDesc.toBSON());
}

Status RollbackFixUpInfo::processCollModOplogEntry(OperationContext* opCtx,
                                                   const UUID& collectionUuid,
                                                   const BSONObj& optionsObj) {
    // If validation is enabled for the collection, the collection options document may contain
    // dollar ($) prefixed field in the "validator" field. Normally, the update operator in the
    // query execution framework disallows such fields (see validateDollarPrefixElement() in
    // exec/update.cpp). To disable this check when upserting the collection options document into
    // the "kRollbackCollectionOptionsNamespace", we have to disable replicated writes in the
    // OperationContext during the update operation.
    UnreplicatedWritesBlock uwb(opCtx);

    CollectionOptionsDescription desc(collectionUuid, optionsObj);
    return _upsertById(opCtx, kRollbackCollectionOptionsNamespace, desc.toBSON());
}

Status RollbackFixUpInfo::processCreateIndexOplogEntry(OperationContext* opCtx,
                                                       const UUID& collectionUuid,
                                                       const std::string& indexName) {
    IndexDescription desc(collectionUuid, indexName, IndexOpType::kCreate, {});

    // If the existing document (that may or may not exist in the "kRollbackIndexNamespace"
    // collection) has a 'drop' op type, this oplog entry will cancel out the previously processed
    // 'dropIndexes" oplog entry. We should remove the existing document from the collection and not
    // insert a new document.
    BSONObjBuilder bob;
    bob.append("_id", desc.makeIdKey());
    auto key = bob.obj();
    auto deleteResult = _storageInterface->deleteById(opCtx, kRollbackIndexNamespace, key["_id"]);
    if (deleteResult.isOK()) {
        auto doc = deleteResult.getValue();
        auto opTypeResult = IndexDescription::parseOpType(doc);
        if (!opTypeResult.isOK()) {
            invariant(ErrorCodes::FailedToParse == opTypeResult.getStatus());
            warning() << "While processing createIndex oplog entry for index " << indexName
                      << " in collection with UUID " << collectionUuid.toString()
                      << ", found existing entry in rollback collection " << kRollbackIndexNamespace
                      << " with unrecognized operation type:" << doc
                      << ". Replacing existing entry.";
            return _upsertIndexDescription(opCtx, desc);
        } else if (IndexOpType::kDrop == opTypeResult.getValue()) {
            return Status::OK();
        }
        // Fall through and replace existing document.
    }

    return _upsertIndexDescription(opCtx, desc);
}

Status RollbackFixUpInfo::processUpdateIndexTTLOplogEntry(OperationContext* opCtx,
                                                          const UUID& collectionUuid,
                                                          const std::string& indexName,
                                                          Seconds expireAfterSeconds) {
    IndexDescription desc(collectionUuid, indexName, expireAfterSeconds);
    return _upsertIndexDescription(opCtx, desc);
}

Status RollbackFixUpInfo::processDropIndexOplogEntry(OperationContext* opCtx,
                                                     const UUID& collectionUuid,
                                                     const std::string& indexName,
                                                     const BSONObj& infoObj) {
    IndexDescription desc(collectionUuid, indexName, IndexOpType::kDrop, infoObj);
    return _upsertIndexDescription(opCtx, desc);
}

Status RollbackFixUpInfo::_upsertById(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const BSONObj& update) {
    auto key = update["_id"];
    invariant(!key.eoo());
    return _storageInterface->upsertById(opCtx, nss, key, update);
}

Status RollbackFixUpInfo::_upsertIndexDescription(OperationContext* opCtx,
                                                  const IndexDescription& description) {
    switch (description.getOpType()) {
        case IndexOpType::kCreate:
        case IndexOpType::kDrop:
            return _upsertById(opCtx, kRollbackIndexNamespace, description.toBSON());
        case IndexOpType::kUpdateTTL: {
            // For updateTTL, if there is an existing document in the collection with a "drop" op
            // type, we should update the index info obj in the existing document and leave the op
            // type unchanged. Otherwise, we assume that the existing document has an op type of
            // "updateTTL"; or is not present in the collection. Therefore, it is safe to overwrite
            // any existing data.
            //
            // It's not possible for the existing document to have a "create" op type while
            // processing a collMod (updateTTL) because this implies the follow sequence of
            // of operations in the oplog:
            //    ..., collMod, ..., createIndex, ...
            // (createIndex gets processed before collMod)
            // This is illegal because there's a missing dropIndex oplog entry between the collMod
            // and createIndex oplog entries.
            auto expireAfterSeconds = description.getExpireAfterSeconds();
            invariant(expireAfterSeconds);
            BSONObjBuilder updateBob;
            {
                BSONObjBuilder setOnInsertBob(updateBob.subobjStart("$setOnInsert"));
                setOnInsertBob.append("operationType", description.getOpTypeAsString());
            }
            {
                BSONObjBuilder setBob(updateBob.subobjStart("$set"));
                setBob.append("infoObj.expireAfterSeconds",
                              durationCount<Seconds>(*expireAfterSeconds));
            }
            auto updateDoc = updateBob.obj();

            BSONObjBuilder bob;
            bob.append("_id", description.makeIdKey());
            auto key = bob.obj();
            return _storageInterface->upsertById(
                opCtx, kRollbackIndexNamespace, key.firstElement(), updateDoc);
        }
    }
    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo
