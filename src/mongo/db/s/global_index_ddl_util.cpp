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

#include "mongo/db/s/global_index_ddl_util.h"

#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
/**
 * Remove all indexes by uuid.
 */
void deleteGlobalIndexes(OperationContext* opCtx,
                         const CollectionPtr& collection,
                         const UUID& uuid) {
    mongo::deleteObjects(opCtx,
                         collection,
                         NamespaceString::kShardIndexCatalogNamespace,
                         BSON(IndexCatalogType::kCollectionUUIDFieldName << uuid),
                         false);
}
}  // namespace

void renameGlobalIndexesMetadata(OperationContext* opCtx,
                                 const NamespaceString& fromNss,
                                 const NamespaceString& toNss,
                                 const Timestamp& indexVersion) {
    writeConflictRetry(
        opCtx,
        "RenameGlobalIndexesMetadata",
        NamespaceString::kShardIndexCatalogNamespace.ns(),
        [&]() {
            boost::optional<UUID> toUuid;
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection collsColl(
                opCtx, NamespaceString::kShardCollectionCatalogNamespace, MODE_IX);
            {
                // First get the document to check the index version if the document already exists
                const auto queryTo = BSON(CollectionType::kNssFieldName << toNss.ns());
                BSONObj collectionDoc;
                bool docExists =
                    Helpers::findOne(opCtx, collsColl.getCollection(), queryTo, collectionDoc);
                if (docExists &&
                    indexVersion <=
                        collectionDoc[CollectionType::kIndexVersionFieldName].timestamp()) {
                    LOGV2_DEBUG(
                        7079500,
                        1,
                        "renameGlobalIndexesMetadata has index version older than current "
                        "collection index version",
                        "collectionIndexVersion"_attr =
                            collectionDoc[CollectionType::kIndexVersionFieldName].timestamp(),
                        "expectedIndexVersion"_attr = indexVersion);
                    return;
                }
                // Update the document (or create it) with the new index version
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                // Save uuid to remove the 'to' indexes later on.
                if (docExists) {
                    toUuid =
                        uassertStatusOK(UUID::parse(collectionDoc[CollectionType::kUuidFieldName]));
                    // Remove the 'to' entry.
                    mongo::deleteObjects(opCtx,
                                         collsColl.getCollection(),
                                         NamespaceString::kShardCollectionCatalogNamespace,
                                         queryTo,
                                         true);
                }
                // Replace the _id in the 'From' entry.
                BSONObj collectionFromDoc;
                auto queryFrom = BSON(CollectionType::kNssFieldName << fromNss.ns());
                fassert(7082801,
                        Helpers::findOne(
                            opCtx, collsColl.getCollection(), queryFrom, collectionFromDoc));
                auto finalDoc = collectionFromDoc.addField(
                    BSON(CollectionType::kNssFieldName << toNss.ns()).firstElement());

                mongo::deleteObjects(opCtx,
                                     collsColl.getCollection(),
                                     NamespaceString::kShardCollectionCatalogNamespace,
                                     queryFrom,
                                     true);
                uassertStatusOK(collection_internal::insertDocument(
                    opCtx, collsColl.getCollection(), InsertStatement(finalDoc), nullptr));
            }
            AutoGetCollection idxColl(opCtx, NamespaceString::kShardIndexCatalogNamespace, MODE_IX);

            if (toUuid) {
                // Remove the 'to' indexes.
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                mongo::deleteObjects(
                    opCtx,
                    idxColl.getCollection(),
                    NamespaceString::kShardIndexCatalogNamespace,
                    BSON(IndexCatalogType::kCollectionUUIDFieldName << toUuid.value()),
                    false);
            }

            auto entryObj = BSON("op"
                                 << "m"
                                 << "entry"
                                 << BSON(IndexCatalogType::kLastmodFieldName
                                         << indexVersion << "fromNss" << fromNss.ns() << "toNss"
                                         << toNss.ns()));

            opCtx->getServiceContext()
                ->getOpObserver()
                ->onModifyShardedCollectionGlobalIndexCatalogEntry(
                    opCtx, fromNss, idxColl->uuid(), entryObj);
            wunit.commit();
        });
}

void addGlobalIndexCatalogEntryToCollection(OperationContext* opCtx,
                                            const NamespaceString& userCollectionNss,
                                            const std::string& name,
                                            const BSONObj& keyPattern,
                                            const BSONObj& options,
                                            const UUID& collectionUUID,
                                            const Timestamp& lastmod,
                                            const boost::optional<UUID>& indexCollectionUUID) {
    IndexCatalogType indexCatalogEntry(name, keyPattern, options, lastmod, collectionUUID);
    indexCatalogEntry.setIndexCollectionUUID(indexCollectionUUID);

    writeConflictRetry(
        opCtx, "AddIndexCatalogEntry", NamespaceString::kShardIndexCatalogNamespace.ns(), [&]() {
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection collsColl(
                opCtx, NamespaceString::kShardCollectionCatalogNamespace, MODE_IX);

            {
                // First get the document to check the index version if the document already exists
                const auto query = BSON(CollectionType::kNssFieldName
                                        << userCollectionNss.ns() << CollectionType::kUuidFieldName
                                        << collectionUUID);
                BSONObj collectionDoc;
                bool docExists =
                    Helpers::findOne(opCtx, collsColl.getCollection(), query, collectionDoc);
                if (docExists && !collectionDoc[CollectionType::kIndexVersionFieldName].eoo() &&
                    lastmod <= collectionDoc[CollectionType::kIndexVersionFieldName].timestamp()) {
                    LOGV2_DEBUG(
                        6712300,
                        1,
                        "addGlobalIndexCatalogEntryToCollection has index version older than "
                        "current "
                        "collection index version",
                        "collectionIndexVersion"_attr =
                            collectionDoc[CollectionType::kIndexVersionFieldName].timestamp(),
                        "expectedIndexVersion"_attr = lastmod);
                    return;
                }
                // Update the document (or create it) with the new index version
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                auto request = UpdateRequest();
                request.setNamespaceString(NamespaceString::kShardCollectionCatalogNamespace);
                request.setQuery(query);
                request.setUpdateModification(
                    BSON(CollectionType::kNssFieldName
                         << userCollectionNss.ns() << CollectionType::kUuidFieldName
                         << collectionUUID << CollectionType::kIndexVersionFieldName << lastmod));
                request.setUpsert(true);
                request.setFromOplogApplication(true);
                mongo::update(opCtx, collsColl.getDb(), request);
            }

            AutoGetCollection idxColl(opCtx, NamespaceString::kShardIndexCatalogNamespace, MODE_IX);

            {
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                BSONObjBuilder builder(indexCatalogEntry.toBSON());
                auto idStr = format(FMT_STRING("{}_{}"), collectionUUID.toString(), name);
                builder.append("_id", idStr);
                uassertStatusOK(collection_internal::insertDocument(opCtx,
                                                                    idxColl.getCollection(),
                                                                    InsertStatement{builder.obj()},
                                                                    nullptr,
                                                                    false));
            }
            auto entryObj = BSON("op"
                                 << "i"
                                 << "entry" << indexCatalogEntry.toBSON());
            opCtx->getServiceContext()
                ->getOpObserver()
                ->onModifyShardedCollectionGlobalIndexCatalogEntry(
                    opCtx, userCollectionNss, idxColl->uuid(), entryObj);
            wunit.commit();
        });
}

void removeGlobalIndexCatalogEntryFromCollection(OperationContext* opCtx,
                                                 const NamespaceString& userCollectionNss,
                                                 const UUID& collectionUUID,
                                                 const std::string& indexName,
                                                 const Timestamp& lastmod) {
    writeConflictRetry(
        opCtx, "RemoveIndexCatalogEntry", NamespaceString::kShardIndexCatalogNamespace.ns(), [&]() {
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection collsColl(
                opCtx, NamespaceString::kShardCollectionCatalogNamespace, MODE_IX);

            {
                // First get the document to check the index version if the document already exists
                const auto query = BSON(CollectionType::kNssFieldName
                                        << userCollectionNss.ns() << CollectionType::kUuidFieldName
                                        << collectionUUID);
                BSONObj collectionDoc;
                bool docExists =
                    Helpers::findOne(opCtx, collsColl.getCollection(), query, collectionDoc);
                if (docExists && !collectionDoc[CollectionType::kIndexVersionFieldName].eoo() &&
                    lastmod <= collectionDoc[CollectionType::kIndexVersionFieldName].timestamp()) {
                    LOGV2_DEBUG(
                        6712301,
                        1,
                        "removeGlobalIndexCatalogEntryFromCollection has index version older than "
                        "current "
                        "collection index version",
                        "collectionIndexVersion"_attr =
                            collectionDoc[CollectionType::kIndexVersionFieldName].timestamp(),
                        "expectedIndexVersion"_attr = lastmod);
                    return;
                }
                // Update the document (or create it) with the new index version
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                auto request = UpdateRequest();
                request.setNamespaceString(NamespaceString::kShardCollectionCatalogNamespace);
                request.setQuery(query);
                request.setUpdateModification(
                    BSON(CollectionType::kNssFieldName
                         << userCollectionNss.ns() << CollectionType::kUuidFieldName
                         << collectionUUID << CollectionType::kIndexVersionFieldName << lastmod));
                request.setUpsert(true);
                request.setFromOplogApplication(true);
                mongo::update(opCtx, collsColl.getDb(), request);
            }

            AutoGetCollection idxColl(opCtx, NamespaceString::kShardIndexCatalogNamespace, MODE_IX);

            {
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                mongo::deleteObjects(opCtx,
                                     idxColl.getCollection(),
                                     NamespaceString::kShardIndexCatalogNamespace,
                                     BSON(IndexCatalogType::kCollectionUUIDFieldName
                                          << collectionUUID << IndexCatalogType::kNameFieldName
                                          << indexName),
                                     true);
            }

            auto entryObj = BSON("op"
                                 << "d"
                                 << "entry"
                                 << BSON(IndexCatalogType::kNameFieldName
                                         << indexName << IndexCatalogType::kLastmodFieldName
                                         << lastmod << IndexCatalogType::kCollectionUUIDFieldName
                                         << collectionUUID));

            opCtx->getServiceContext()
                ->getOpObserver()
                ->onModifyShardedCollectionGlobalIndexCatalogEntry(
                    opCtx, userCollectionNss, idxColl->uuid(), entryObj);
            wunit.commit();
        });
}

void replaceGlobalIndexes(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const UUID& uuid,
                          const Timestamp& indexVersion,
                          const std::vector<IndexCatalogType>& indexes) {
    writeConflictRetry(
        opCtx, "ReplaceIndexCatalog", NamespaceString::kShardIndexCatalogNamespace.ns(), [&]() {
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection collsColl(
                opCtx, NamespaceString::kShardCollectionCatalogNamespace, MODE_IX);
            {
                // Set final indexVersion
                const auto query = BSON(CollectionType::kNssFieldName
                                        << nss.ns() << CollectionType::kUuidFieldName << uuid);

                // Update the document (or create it) with the new index version
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                auto request = UpdateRequest();
                request.setNamespaceString(NamespaceString::kShardCollectionCatalogNamespace);
                request.setQuery(query);
                request.setUpdateModification(BSON(CollectionType::kNssFieldName
                                                   << nss.ns() << CollectionType::kUuidFieldName
                                                   << uuid << CollectionType::kIndexVersionFieldName
                                                   << indexVersion));
                request.setUpsert(true);
                request.setFromOplogApplication(true);
                mongo::update(opCtx, collsColl.getDb(), request);
            }

            AutoGetCollection idxColl(opCtx, NamespaceString::kShardIndexCatalogNamespace, MODE_IX);
            BSONArrayBuilder indexesBSON;
            {
                // Clear old indexes.
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                deleteGlobalIndexes(opCtx, idxColl.getCollection(), uuid);

                // Add new indexes.
                for (const auto& i : indexes) {
                    // Attach a custom generated _id.
                    auto indexBSON = i.toBSON();
                    BSONObjBuilder builder(indexBSON);
                    auto idStr =
                        format(FMT_STRING("{}_{}"), uuid.toString(), i.getName().toString());
                    builder.append("_id", idStr);
                    uassertStatusOK(
                        collection_internal::insertDocument(opCtx,
                                                            idxColl.getCollection(),
                                                            InsertStatement{builder.done()},
                                                            nullptr,
                                                            false));

                    indexesBSON.append(indexBSON);
                }
            }
            auto entryObj = BSON("op"
                                 << "r"
                                 << "entry"
                                 << BSON(IndexCatalogType::kCollectionUUIDFieldName
                                         << uuid << CollectionType::kNssFieldName << nss.toString()
                                         << "v" << indexVersion << "i" << indexesBSON.arr()));

            opCtx->getServiceContext()
                ->getOpObserver()
                ->onModifyShardedCollectionGlobalIndexCatalogEntry(
                    opCtx, nss, idxColl->uuid(), entryObj);
            wunit.commit();
        });
}

void dropCollectionGlobalIndexesMetadata(OperationContext* opCtx,
                                         const NamespaceString& userCollectionNss) {
    writeConflictRetry(
        opCtx, "DropIndexCatalogEntry", NamespaceString::kShardIndexCatalogNamespace.ns(), [&]() {
            boost::optional<UUID> collectionUUID;
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection collsColl(
                opCtx, NamespaceString::kShardCollectionCatalogNamespace, MODE_IX);
            {
                const auto query = BSON(CollectionType::kNssFieldName << userCollectionNss.ns());
                BSONObj collectionDoc;
                // Return if there is nothing to clear.
                if (!Helpers::findOne(opCtx, collsColl.getCollection(), query, collectionDoc)) {
                    return;
                }
                collectionUUID.emplace(
                    uassertStatusOK(UUID::parse(collectionDoc[CollectionType::kUuidFieldName])));
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                mongo::deleteObjects(opCtx,
                                     collsColl.getCollection(),
                                     NamespaceString::kShardCollectionCatalogNamespace,
                                     query,
                                     true);
            }

            AutoGetCollection idxColl(opCtx, NamespaceString::kShardIndexCatalogNamespace, MODE_IX);

            {
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                deleteGlobalIndexes(opCtx, idxColl.getCollection(), *collectionUUID);
            }
            auto entryObj = BSON("op"
                                 << "o"
                                 << "entry"
                                 << BSON(IndexCatalogType::kCollectionUUIDFieldName
                                         << *collectionUUID << CollectionType::kNssFieldName
                                         << userCollectionNss.toString()));

            opCtx->getServiceContext()
                ->getOpObserver()
                ->onModifyShardedCollectionGlobalIndexCatalogEntry(
                    opCtx, userCollectionNss, idxColl->uuid(), entryObj);
            wunit.commit();
        });
}

void clearGlobalIndexes(OperationContext* opCtx,
                        const NamespaceString& userCollectionNss,
                        const UUID& collectionUUID) {
    writeConflictRetry(
        opCtx, "ClearIndexCatalogEntry", NamespaceString::kShardIndexCatalogNamespace.ns(), [&]() {
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection collsColl(
                opCtx, NamespaceString::kShardCollectionCatalogNamespace, MODE_IX);
            {
                // First unset the index version.
                const auto query = BSON(CollectionType::kNssFieldName
                                        << userCollectionNss.ns() << CollectionType::kUuidFieldName
                                        << collectionUUID);
                BSONObj collectionDoc;
                bool docExists =
                    Helpers::findOne(opCtx, collsColl.getCollection(), query, collectionDoc);
                // Return if there is nothing to clear.
                if (!docExists || collectionDoc[CollectionType::kIndexVersionFieldName].eoo()) {
                    return;
                }
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                mongo::deleteObjects(opCtx,
                                     collsColl.getCollection(),
                                     NamespaceString::kShardCollectionCatalogNamespace,
                                     query,
                                     true);
                auto finalDoc = collectionDoc.filterFieldsUndotted(
                    BSON(CollectionType::kIndexVersionFieldName << 1), false);
                uassertStatusOK(collection_internal::insertDocument(
                    opCtx, collsColl.getCollection(), InsertStatement(finalDoc), nullptr));
            }

            AutoGetCollection idxColl(opCtx, NamespaceString::kShardIndexCatalogNamespace, MODE_IX);

            {
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                deleteGlobalIndexes(opCtx, idxColl.getCollection(), collectionUUID);
            }
            auto entryObj = BSON("op"
                                 << "c"
                                 << "entry"
                                 << BSON(IndexCatalogType::kCollectionUUIDFieldName
                                         << collectionUUID << CollectionType::kNssFieldName
                                         << userCollectionNss.toString()));

            opCtx->getServiceContext()
                ->getOpObserver()
                ->onModifyShardedCollectionGlobalIndexCatalogEntry(
                    opCtx, userCollectionNss, idxColl->uuid(), entryObj);
            wunit.commit();
        });
}

}  // namespace mongo
