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

#include "mongo/db/s/sharding_index_catalog_ddl_util.h"

#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/s/shard_authoritative_catalog_gen.h"
#include "mongo/db/shard_role.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_index_catalog.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
/**
 * Remove all indexes by uuid.
 */
void deleteShardingIndexCatalogEntries(OperationContext* opCtx,
                                       const ScopedCollectionAcquisition& collection,
                                       const UUID& uuid) {
    mongo::deleteObjects(
        opCtx, collection, BSON(IndexCatalogType::kCollectionUUIDFieldName << uuid), false);
}


const ScopedCollectionAcquisition& getAcquisitionForNss(
    const std::vector<ScopedCollectionAcquisition>& acquisitions, const NamespaceString& nss) {
    auto it = std::find_if(acquisitions.begin(), acquisitions.end(), [&nss](auto& acquisition) {
        return acquisition.nss() == nss;
    });
    invariant(it != acquisitions.end());
    return *it;
}
}  // namespace

void renameCollectionShardingIndexCatalog(OperationContext* opCtx,
                                          const NamespaceString& fromNss,
                                          const NamespaceString& toNss,
                                          const Timestamp& indexVersion) {
    writeConflictRetry(
        opCtx,
        "RenameCollectionShardingIndexCatalog",
        NamespaceString::kShardIndexCatalogNamespace.ns(),
        [&]() {
            boost::optional<UUID> toUuid;
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection fromToColl(
                opCtx, fromNss, MODE_IX, AutoGetCollection::Options{}.secondaryNssOrUUIDs({toNss}));
            const auto acquisitions = acquireCollections(
                opCtx,
                {CollectionAcquisitionRequest(
                     NamespaceString(NamespaceString::kShardCollectionCatalogNamespace),
                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                     repl::ReadConcernArgs::get(opCtx),
                     AcquisitionPrerequisites::kWrite),
                 CollectionAcquisitionRequest(
                     NamespaceString(NamespaceString::kShardIndexCatalogNamespace),
                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                     repl::ReadConcernArgs::get(opCtx),
                     AcquisitionPrerequisites::kWrite)},
                MODE_IX);

            const auto& collsColl = getAcquisitionForNss(
                acquisitions, NamespaceString::kShardCollectionCatalogNamespace);
            const auto& idxColl =
                getAcquisitionForNss(acquisitions, NamespaceString::kShardIndexCatalogNamespace);

            {
                // First get the document to check the index version if the document already exists
                const auto queryTo =
                    BSON(ShardAuthoritativeCollectionType::kNssFieldName << toNss.ns());
                BSONObj collectionToDoc;
                bool docExists =
                    Helpers::findOne(opCtx, collsColl.getCollectionPtr(), queryTo, collectionToDoc);
                if (docExists) {
                    auto collectionTo = ShardAuthoritativeCollectionType::parse(
                        IDLParserContext("RenameCollectionShardingIndexCatalogCtx"),
                        collectionToDoc);
                    auto toIndexVersion =
                        collectionTo.getIndexVersion().get_value_or(Timestamp(0, 0));
                    if (indexVersion <= toIndexVersion) {
                        LOGV2_DEBUG(7079500,
                                    1,
                                    "renameCollectionShardingIndexCatalog has index version older "
                                    "than current collection index version",
                                    "collectionIndexVersion"_attr = toIndexVersion,
                                    "expectedIndexVersion"_attr = indexVersion,
                                    "fromNss"_attr = redact(toStringForLogging(fromNss)),
                                    "toNss"_attr = redact(toStringForLogging(toNss)));
                        return;
                    }
                    toUuid.emplace(collectionTo.getUuid());
                }
                // Update the document (or create it) with the new index version
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                // Save uuid to remove the 'to' indexes later on.
                if (docExists) {
                    // Remove the 'to' entry.
                    mongo::deleteObjects(opCtx, collsColl, queryTo, true);
                }
                // Replace the _id in the 'From' entry.
                BSONObj collectionFromDoc;
                auto queryFrom = BSON(CollectionType::kNssFieldName << fromNss.ns());
                fassert(7082801,
                        Helpers::findOne(
                            opCtx, collsColl.getCollectionPtr(), queryFrom, collectionFromDoc));
                auto collectionFrom = ShardAuthoritativeCollectionType::parse(
                    IDLParserContext("RenameCollectionShardingIndexCatalogCtx"), collectionFromDoc);
                collectionFrom.setNss(toNss);

                mongo::deleteObjects(opCtx, collsColl, queryFrom, true);
                uassertStatusOK(
                    collection_internal::insertDocument(opCtx,
                                                        collsColl.getCollectionPtr(),
                                                        InsertStatement(collectionFrom.toBSON()),
                                                        nullptr));
            }

            if (toUuid) {
                // Remove the 'to' indexes.
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                mongo::deleteObjects(
                    opCtx,
                    idxColl,
                    BSON(IndexCatalogType::kCollectionUUIDFieldName << toUuid.value()),
                    false);
            }

            opCtx->getServiceContext()->getOpObserver()->onModifyCollectionShardingIndexCatalog(
                opCtx,
                fromNss,
                idxColl.uuid(),
                ShardingIndexCatalogRenameEntry(fromNss, toNss, indexVersion).toBSON());
            wunit.commit();
        });
}

void addShardingIndexCatalogEntryToCollection(OperationContext* opCtx,
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
            AutoGetCollection userColl(opCtx, userCollectionNss, MODE_IX);
            AutoGetCollection collsColl(opCtx,
                                        NamespaceString::kShardCollectionCatalogNamespace,
                                        MODE_IX,
                                        AutoGetCollection::Options{}.secondaryNssOrUUIDs(
                                            {NamespaceString::kShardIndexCatalogNamespace}));

            {
                // First get the document to check the index version if the document already exists
                const auto query =
                    BSON(ShardAuthoritativeCollectionType::kNssFieldName
                         << userCollectionNss.ns()
                         << ShardAuthoritativeCollectionType::kUuidFieldName << collectionUUID);
                BSONObj collectionDoc;
                bool docExists =
                    Helpers::findOne(opCtx, collsColl.getCollection(), query, collectionDoc);
                if (docExists) {
                    auto collection = ShardAuthoritativeCollectionType::parse(
                        IDLParserContext("AddIndexCatalogEntry"), collectionDoc);
                    if (collection.getIndexVersion() && lastmod <= *collection.getIndexVersion()) {
                        LOGV2_DEBUG(6712300,
                                    1,
                                    "addShardingIndexCatalogEntryToCollection has index version "
                                    "older than current collection index version",
                                    "collectionIndexVersion"_attr = *collection.getIndexVersion(),
                                    "expectedIndexVersion"_attr = lastmod,
                                    "nss"_attr = redact(toStringForLogging(userCollectionNss)));
                        return;
                    }
                }
                // Update the document (or create it) with the new index version
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                auto request = UpdateRequest();
                request.setNamespaceString(NamespaceString::kShardCollectionCatalogNamespace);
                request.setQuery(query);
                request.setUpdateModification(
                    BSON(ShardAuthoritativeCollectionType::kNssFieldName
                         << userCollectionNss.ns()
                         << ShardAuthoritativeCollectionType::kUuidFieldName << collectionUUID
                         << ShardAuthoritativeCollectionType::kIndexVersionFieldName << lastmod));
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

            opCtx->getServiceContext()->getOpObserver()->onModifyCollectionShardingIndexCatalog(
                opCtx,
                userCollectionNss,
                idxColl->uuid(),
                ShardingIndexCatalogInsertEntry(indexCatalogEntry).toBSON());
            wunit.commit();
        });
}

void removeShardingIndexCatalogEntryFromCollection(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const UUID& uuid,
                                                   const StringData& indexName,
                                                   const Timestamp& lastmod) {
    writeConflictRetry(
        opCtx,
        "RemoveShardingIndexCatalogEntryFromCollection",
        NamespaceString::kShardIndexCatalogNamespace.ns(),
        [&]() {
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection userColl(opCtx, nss, MODE_IX);
            AutoGetCollection collsColl(opCtx,
                                        NamespaceString::kShardCollectionCatalogNamespace,
                                        MODE_IX,
                                        AutoGetCollection::Options{}.secondaryNssOrUUIDs(
                                            {NamespaceString::kShardIndexCatalogNamespace}));
            {
                // First get the document to check the index version if the document already exists
                const auto query =
                    BSON(ShardAuthoritativeCollectionType::kNssFieldName
                         << nss.ns() << ShardAuthoritativeCollectionType::kUuidFieldName << uuid);
                BSONObj collectionDoc;
                bool docExists =
                    Helpers::findOne(opCtx, collsColl.getCollection(), query, collectionDoc);
                if (docExists) {
                    auto collection = ShardAuthoritativeCollectionType::parse(
                        IDLParserContext("RemoveIndexCatalogEntry"), collectionDoc);
                    if (collection.getIndexVersion() && lastmod <= *collection.getIndexVersion()) {
                        LOGV2_DEBUG(6712301,
                                    1,
                                    "removeShardingIndexCatalogEntryFromCollection has index "
                                    "version older than current collection index version",
                                    "collectionIndexVersion"_attr = *collection.getIndexVersion(),
                                    "expectedIndexVersion"_attr = lastmod,
                                    "nss"_attr = redact(toStringForLogging(nss)));
                        return;
                    }
                }
                // Update the document (or create it) with the new index version
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                auto request = UpdateRequest();
                request.setNamespaceString(NamespaceString::kShardCollectionCatalogNamespace);
                request.setQuery(query);
                request.setUpdateModification(
                    BSON(ShardAuthoritativeCollectionType::kNssFieldName
                         << nss.ns() << ShardAuthoritativeCollectionType::kUuidFieldName << uuid
                         << ShardAuthoritativeCollectionType::kIndexVersionFieldName << lastmod));
                request.setUpsert(true);
                request.setFromOplogApplication(true);
                mongo::update(opCtx, collsColl.getDb(), request);
            }

            const auto idxColl =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest(
                                      NamespaceString(NamespaceString::kShardIndexCatalogNamespace),
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(opCtx),
                                      AcquisitionPrerequisites::kWrite),
                                  MODE_IX);

            {
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                mongo::deleteObjects(opCtx,
                                     idxColl,
                                     BSON(IndexCatalogType::kCollectionUUIDFieldName
                                          << uuid << IndexCatalogType::kNameFieldName << indexName),
                                     true);
            }

            opCtx->getServiceContext()->getOpObserver()->onModifyCollectionShardingIndexCatalog(
                opCtx,
                nss,
                idxColl.uuid(),
                ShardingIndexCatalogRemoveEntry(indexName.toString(), uuid, lastmod).toBSON());
            wunit.commit();
        });
}

void replaceCollectionShardingIndexCatalog(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const UUID& uuid,
                                           const Timestamp& indexVersion,
                                           const std::vector<IndexCatalogType>& indexes) {
    writeConflictRetry(
        opCtx,
        "ReplaceCollectionShardingIndexCatalog",
        NamespaceString::kShardIndexCatalogNamespace.ns(),
        [&]() {
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection userColl(opCtx, nss, MODE_IX);
            AutoGetCollection collsColl(opCtx,
                                        NamespaceString::kShardCollectionCatalogNamespace,
                                        MODE_IX,
                                        AutoGetCollection::Options{}.secondaryNssOrUUIDs(
                                            {NamespaceString::kShardIndexCatalogNamespace}));
            {
                const auto query =
                    BSON(ShardAuthoritativeCollectionType::kNssFieldName
                         << nss.ns() << ShardAuthoritativeCollectionType::kUuidFieldName << uuid);
                BSONObj collectionDoc;
                bool docExists =
                    Helpers::findOne(opCtx, collsColl.getCollection(), query, collectionDoc);
                if (docExists) {
                    auto collection = ShardAuthoritativeCollectionType::parse(
                        IDLParserContext("ReplaceIndexCatalogEntry"), collectionDoc);
                    if (collection.getIndexVersion() &&
                        indexVersion <= *collection.getIndexVersion()) {
                        LOGV2_DEBUG(6712304,
                                    1,
                                    "replaceCollectionGlobalIndexes has index version older than "
                                    "current collection index version",
                                    "collectionIndexVersion"_attr = *collection.getIndexVersion(),
                                    "expectedIndexVersion"_attr = indexVersion,
                                    "nss"_attr = redact(toStringForLogging(nss)));
                        return;
                    }
                }

                // Set final indexVersion. Update the document (or create it) with the new index
                // version
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                auto request = UpdateRequest();
                request.setNamespaceString(NamespaceString::kShardCollectionCatalogNamespace);
                request.setQuery(query);
                request.setUpdateModification(BSON(
                    ShardAuthoritativeCollectionType::kNssFieldName
                    << nss.ns() << ShardAuthoritativeCollectionType::kUuidFieldName << uuid
                    << ShardAuthoritativeCollectionType::kIndexVersionFieldName << indexVersion));
                request.setUpsert(true);
                request.setFromOplogApplication(true);
                mongo::update(opCtx, collsColl.getDb(), request);
            }

            const auto idxColl =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest(
                                      NamespaceString(NamespaceString::kShardIndexCatalogNamespace),
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(opCtx),
                                      AcquisitionPrerequisites::kWrite),
                                  MODE_IX);
            {
                // Clear old indexes.
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                deleteShardingIndexCatalogEntries(opCtx, idxColl, uuid);

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
                                                            idxColl.getCollectionPtr(),
                                                            InsertStatement{builder.done()},
                                                            nullptr,
                                                            false));
                }
            }

            opCtx->getServiceContext()->getOpObserver()->onModifyCollectionShardingIndexCatalog(
                opCtx,
                nss,
                idxColl.uuid(),
                ShardingIndexCatalogReplaceEntry(uuid, indexVersion, indexes).toBSON());
            wunit.commit();
        });
}

void dropCollectionShardingIndexCatalog(OperationContext* opCtx, const NamespaceString& nss) {
    writeConflictRetry(
        opCtx,
        "DropCollectionShardingIndexCatalog",
        NamespaceString::kShardIndexCatalogNamespace.ns(),
        [&]() {
            boost::optional<UUID> collectionUUID;
            WriteUnitOfWork wunit(opCtx);
            Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
            Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
            const auto acquisitions = acquireCollections(
                opCtx,
                {CollectionAcquisitionRequest(
                     NamespaceString(NamespaceString::kShardCollectionCatalogNamespace),
                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                     repl::ReadConcernArgs::get(opCtx),
                     AcquisitionPrerequisites::kWrite),
                 CollectionAcquisitionRequest(
                     NamespaceString(NamespaceString::kShardIndexCatalogNamespace),
                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                     repl::ReadConcernArgs::get(opCtx),
                     AcquisitionPrerequisites::kWrite)},
                MODE_IX);

            const auto& collsColl = getAcquisitionForNss(
                acquisitions, NamespaceString::kShardCollectionCatalogNamespace);
            const auto& idxColl =
                getAcquisitionForNss(acquisitions, NamespaceString::kShardIndexCatalogNamespace);

            {
                const auto query =
                    BSON(ShardAuthoritativeCollectionType::kNssFieldName << nss.ns());
                BSONObj collectionDoc;
                // Get the collection UUID, if nothing is found, return early.
                if (!Helpers::findOne(opCtx, collsColl.getCollectionPtr(), query, collectionDoc)) {
                    LOGV2_DEBUG(6712305,
                                1,
                                "dropCollectionGlobalIndexesMetadata did not found collection, "
                                "skipping dropping index metadata",
                                "nss"_attr = redact(toStringForLogging(nss)));
                    return;
                }
                auto collection = ShardAuthoritativeCollectionType::parse(
                    IDLParserContext("dropCollectionShardingIndexCatalog"), collectionDoc);
                collectionUUID.emplace(collection.getUuid());
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                mongo::deleteObjects(opCtx, collsColl, query, true);
            }

            // AutoGetCollection idxColl(opCtx, NamespaceString::kShardIndexCatalogNamespace,
            // MODE_IX);

            {
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                deleteShardingIndexCatalogEntries(opCtx, idxColl, *collectionUUID);
            }

            opCtx->getServiceContext()->getOpObserver()->onModifyCollectionShardingIndexCatalog(
                opCtx,
                nss,
                idxColl.uuid(),
                ShardingIndexCatalogDropEntry(*collectionUUID).toBSON());
            wunit.commit();
        });
}

void clearCollectionShardingIndexCatalog(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const UUID& uuid) {
    writeConflictRetry(
        opCtx,
        "ClearCollectionShardingIndexCatalog",
        NamespaceString::kShardIndexCatalogNamespace.ns(),
        [&]() {
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection userColl(opCtx, nss, MODE_IX);
            const auto acquisitions = acquireCollections(
                opCtx,
                {CollectionAcquisitionRequest(
                     NamespaceString(NamespaceString::kShardCollectionCatalogNamespace),
                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                     repl::ReadConcernArgs::get(opCtx),
                     AcquisitionPrerequisites::kWrite),
                 CollectionAcquisitionRequest(
                     NamespaceString(NamespaceString::kShardIndexCatalogNamespace),
                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                     repl::ReadConcernArgs::get(opCtx),
                     AcquisitionPrerequisites::kWrite)},
                MODE_IX);

            const auto& collsColl = getAcquisitionForNss(
                acquisitions, NamespaceString::kShardCollectionCatalogNamespace);
            const auto& idxColl =
                getAcquisitionForNss(acquisitions, NamespaceString::kShardIndexCatalogNamespace);
            {
                // First unset the index version.
                const auto query =
                    BSON(ShardAuthoritativeCollectionType::kNssFieldName
                         << nss.ns() << ShardAuthoritativeCollectionType::kUuidFieldName << uuid);
                BSONObj collectionDoc;
                bool docExists =
                    Helpers::findOne(opCtx, collsColl.getCollectionPtr(), query, collectionDoc);

                // Return if there is nothing to clear.
                if (!docExists) {
                    return;
                }

                auto collection = ShardAuthoritativeCollectionType::parse(
                    IDLParserContext("clearCollectionShardingIndexCatalogCtx"), collectionDoc);

                if (!collection.getIndexVersion()) {
                    return;
                }

                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                mongo::deleteObjects(opCtx, collsColl, query, true);
                collection.setIndexVersion(boost::none);
                uassertStatusOK(
                    collection_internal::insertDocument(opCtx,
                                                        collsColl.getCollectionPtr(),
                                                        InsertStatement(collection.toBSON()),
                                                        nullptr));
            }

            {
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                deleteShardingIndexCatalogEntries(opCtx, idxColl, uuid);
            }

            opCtx->getServiceContext()->getOpObserver()->onModifyCollectionShardingIndexCatalog(
                opCtx, nss, idxColl.uuid(), ShardingIndexCatalogClearEntry(uuid).toBSON());
            wunit.commit();
        });
}

}  // namespace mongo
