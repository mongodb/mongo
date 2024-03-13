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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#include <absl/container/node_hash_map.h>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/shard_authoritative_catalog_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_collection_gen.h"
#include "mongo/s/catalog/type_index_catalog.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
/**
 * Remove all indexes by uuid.
 */
void deleteShardingIndexCatalogEntries(OperationContext* opCtx,
                                       const CollectionAcquisition& collection,
                                       const UUID& uuid) {
    mongo::deleteObjects(
        opCtx, collection, BSON(IndexCatalogType::kCollectionUUIDFieldName << uuid), false);
}

}  // namespace

void renameCollectionShardingIndexCatalog(OperationContext* opCtx,
                                          const NamespaceString& fromNss,
                                          const NamespaceString& toNss,
                                          const Timestamp& indexVersion) {
    writeConflictRetry(
        opCtx,
        "RenameCollectionShardingIndexCatalog",
        NamespaceString::kShardIndexCatalogNamespace,
        [&]() {
            boost::optional<UUID> toUuid;
            WriteUnitOfWork wunit(opCtx);
            std::vector<NamespaceStringOrUUID> toNssVec({toNss});
            AutoGetCollection fromToColl(opCtx,
                                         fromNss,
                                         MODE_IX,
                                         AutoGetCollection::Options{}.secondaryNssOrUUIDs(
                                             toNssVec.cbegin(), toNssVec.cend()));
            auto acquisitions = acquireCollections(
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

            const auto& collsColl =
                acquisitions.at(NamespaceString::kShardCollectionCatalogNamespace);
            const auto& idxColl = acquisitions.at(NamespaceString::kShardIndexCatalogNamespace);

            {
                // First get the document to check the index version if the document already exists
                const auto queryTo = BSON(
                    ShardAuthoritativeCollectionType::kNssFieldName
                    << NamespaceStringUtil::serialize(toNss, SerializationContext::stateDefault()));
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
                auto queryFrom =
                    BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                             fromNss, SerializationContext::stateDefault()));
                fassert(7082801,
                        Helpers::findOne(
                            opCtx, collsColl.getCollectionPtr(), queryFrom, collectionFromDoc));
                auto collectionFrom = ShardAuthoritativeCollectionType::parse(
                    IDLParserContext("RenameCollectionShardingIndexCatalogCtx"), collectionFromDoc);
                collectionFrom.setNss(toNss);

                mongo::deleteObjects(opCtx, collsColl, queryFrom, true);
                uassertStatusOK(Helpers::insert(opCtx, collsColl, collectionFrom.toBSON()));
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
        opCtx, "AddIndexCatalogEntry", NamespaceString::kShardIndexCatalogNamespace, [&]() {
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection userColl(opCtx, userCollectionNss, MODE_IX);
            auto acquisitions = acquireCollections(
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

            auto& collsColl = acquisitions.at(NamespaceString::kShardCollectionCatalogNamespace);
            const auto& idxColl = acquisitions.at(NamespaceString::kShardIndexCatalogNamespace);
            const auto usserCollectionNssStr = NamespaceStringUtil::serialize(
                userCollectionNss, SerializationContext::stateDefault());
            {
                // First get the document to check the index version if the document already exists
                const auto query =
                    BSON(ShardAuthoritativeCollectionType::kNssFieldName
                         << usserCollectionNssStr
                         << ShardAuthoritativeCollectionType::kUuidFieldName << collectionUUID);
                BSONObj collectionDoc;
                bool docExists =
                    Helpers::findOne(opCtx, collsColl.getCollectionPtr(), query, collectionDoc);
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
                         << usserCollectionNssStr
                         << ShardAuthoritativeCollectionType::kUuidFieldName << collectionUUID
                         << ShardAuthoritativeCollectionType::kIndexVersionFieldName << lastmod));
                request.setUpsert(true);
                request.setFromOplogApplication(true);
                mongo::update(opCtx, collsColl, request);
            }

            {
                repl::UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
                BSONObjBuilder builder(indexCatalogEntry.toBSON());
                auto idStr = format(FMT_STRING("{}_{}"), collectionUUID.toString(), name);
                builder.append("_id", idStr);
                uassertStatusOK(Helpers::insert(opCtx, idxColl, builder.obj()));
            }

            opCtx->getServiceContext()->getOpObserver()->onModifyCollectionShardingIndexCatalog(
                opCtx,
                userCollectionNss,
                idxColl.uuid(),
                ShardingIndexCatalogInsertEntry(indexCatalogEntry).toBSON());
            wunit.commit();
        });
}

void removeShardingIndexCatalogEntryFromCollection(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const UUID& uuid,
                                                   StringData indexName,
                                                   const Timestamp& lastmod) {
    writeConflictRetry(
        opCtx,
        "RemoveShardingIndexCatalogEntryFromCollection",
        NamespaceString::kShardIndexCatalogNamespace,
        [&]() {
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection userColl(opCtx, nss, MODE_IX);
            auto acquisitions = acquireCollections(
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

            auto& collsColl = acquisitions.at(NamespaceString::kShardCollectionCatalogNamespace);
            const auto& idxColl = acquisitions.at(NamespaceString::kShardIndexCatalogNamespace);
            const auto nssStr =
                NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
            {
                // First get the document to check the index version if the document already exists
                const auto query =
                    BSON(ShardAuthoritativeCollectionType::kNssFieldName
                         << nssStr << ShardAuthoritativeCollectionType::kUuidFieldName << uuid);
                BSONObj collectionDoc;
                bool docExists =
                    Helpers::findOne(opCtx, collsColl.getCollectionPtr(), query, collectionDoc);
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
                         << nssStr << ShardAuthoritativeCollectionType::kUuidFieldName << uuid
                         << ShardAuthoritativeCollectionType::kIndexVersionFieldName << lastmod));
                request.setUpsert(true);
                request.setFromOplogApplication(true);
                mongo::update(opCtx, collsColl, request);
            }

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
        NamespaceString::kShardIndexCatalogNamespace,
        [&]() {
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection userColl(opCtx, nss, MODE_IX);
            auto acquisitions = acquireCollections(
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

            auto& collsColl = acquisitions.at(NamespaceString::kShardCollectionCatalogNamespace);
            const auto& idxColl = acquisitions.at(NamespaceString::kShardIndexCatalogNamespace);
            const auto nssStr =
                NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
            {
                const auto query =
                    BSON(ShardAuthoritativeCollectionType::kNssFieldName
                         << nssStr << ShardAuthoritativeCollectionType::kUuidFieldName << uuid);
                BSONObj collectionDoc;
                bool docExists =
                    Helpers::findOne(opCtx, collsColl.getCollectionPtr(), query, collectionDoc);
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
                    << nssStr << ShardAuthoritativeCollectionType::kUuidFieldName << uuid
                    << ShardAuthoritativeCollectionType::kIndexVersionFieldName << indexVersion));
                request.setUpsert(true);
                request.setFromOplogApplication(true);
                mongo::update(opCtx, collsColl, request);
            }

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
                    uassertStatusOK(Helpers::insert(opCtx, idxColl, builder.done()));
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
        NamespaceString::kShardIndexCatalogNamespace,
        [&]() {
            boost::optional<UUID> collectionUUID;
            WriteUnitOfWork wunit(opCtx);
            Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
            Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
            auto acquisitions = acquireCollections(
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

            const auto& collsColl =
                acquisitions.at(NamespaceString::kShardCollectionCatalogNamespace);
            const auto& idxColl = acquisitions.at(NamespaceString::kShardIndexCatalogNamespace);
            {
                const auto query = BSON(
                    ShardAuthoritativeCollectionType::kNssFieldName
                    << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
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
        NamespaceString::kShardIndexCatalogNamespace,
        [&]() {
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection userColl(opCtx, nss, MODE_IX);
            auto acquisitions = acquireCollections(
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

            const auto& collsColl =
                acquisitions.at(NamespaceString::kShardCollectionCatalogNamespace);
            const auto& idxColl = acquisitions.at(NamespaceString::kShardIndexCatalogNamespace);

            {
                // First unset the index version.
                const auto query = BSON(
                    ShardAuthoritativeCollectionType::kNssFieldName
                    << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())
                    << ShardAuthoritativeCollectionType::kUuidFieldName << uuid);
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
                uassertStatusOK(Helpers::insert(opCtx, collsColl, collection.toBSON()));
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
