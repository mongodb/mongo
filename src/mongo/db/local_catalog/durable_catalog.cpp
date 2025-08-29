/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/durable_catalog.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/collection_record_store_options.h"
#include "mongo/db/local_catalog/durable_catalog_entry_metadata.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/storage/feature_document_util.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <memory>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::durable_catalog {
namespace {

// Finds the durable catalog entry using the provided RecordStore cursor. The returned BSONObj is
// unowned and is only valid while the cursor is positioned.
BSONObj findEntry(SeekableRecordCursor& cursor, const RecordId& catalogId) {
    auto record = cursor.seekExact(catalogId);
    if (!record) {
        return BSONObj();
    }

    return record->data.releaseToBson();
}

std::shared_ptr<CatalogEntryMetaData> parseMetaData(const BSONElement& mdElement) {
    std::shared_ptr<CatalogEntryMetaData> md;
    if (mdElement.isABSONObj()) {
        LOGV2_DEBUG(22210, 3, "returning metadata: {mdElement}", "mdElement"_attr = mdElement);
        md = std::make_shared<CatalogEntryMetaData>();
        md->parse(mdElement.Obj());
    }
    return md;
}

// Ensures that only one catalog entry has the same ident as 'expectedCatalogEntry'.  This check is
// potentially expensive, as it iterates over all catalog entries, and should only be used after an
// 'ObjectAlreadyExists' error occurs after the first attempt to persist a new collection in
// storage. Such cases are expected to be rare.
Status validateNoIdentConflictInCatalog(OperationContext* opCtx,
                                        const MDBCatalog::EntryIdentifier& expectedCatalogEntry,
                                        MDBCatalog* mdbCatalog) {
    std::vector<MDBCatalog::EntryIdentifier> entriesWithIdent;
    auto cursor = mdbCatalog->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();

        if (feature_document_util::isFeatureDocument(obj)) {
            // Skip over the version document because it doesn't correspond to a collection.
            continue;
        }

        auto entryIdent = obj["ident"].String();
        if (entryIdent == expectedCatalogEntry.ident &&
            expectedCatalogEntry.catalogId != record->id) {
            return Status(
                ErrorCodes::ObjectAlreadyExists,
                fmt::format("Could not create collection {} with ident {} because the ident was "
                            "already in use by another table recorded in the catalog",
                            expectedCatalogEntry.nss.toStringForErrorMsg(),
                            expectedCatalogEntry.ident));
        }
    }
    return Status::OK();
}

// Retries creating new collection's table on disk after the first attempt returns
// 'ObjectAlreadyExists'. This can happen if idents are replicated, the initial create attempt was
// rolled back, and the same operation gets applied again. In which case, the ident may correspond
// to a table still on disk.
StatusWith<std::unique_ptr<RecordStore>> retryCreateCollectionIfObjectAlreadyExists(
    OperationContext* opCtx,
    const MDBCatalog::EntryIdentifier& catalogEntry,
    const boost::optional<UUID>& uuid,
    const RecordStore::Options& recordStoreOptions,
    const Status& originalFailure,
    MDBCatalog* mdbCatalog) {
    // First, validate the ident doesn't appear in multiple catalog entries. This should never
    // happen unless there is manual oplog work or applyOps intervention which has caused
    // corruption.
    Status validateStatus = validateNoIdentConflictInCatalog(opCtx, catalogEntry, mdbCatalog);
    if (!validateStatus.isOK()) {
        return validateStatus;
    }

    // Rollback only guarantees the first state of two-phase table drop. If the initial create for
    // the table was rolled back, it can still exist in the storage engine. The ident is likely in
    // the drop-pending reaper, so we must remove it before trying to create the same collection
    // again.
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    const auto& ident = catalogEntry.ident;
    auto dropStatus = storageEngine->immediatelyCompletePendingDrop(opCtx, ident);
    if (!dropStatus.isOK()) {
        LOGV2(10526201,
              "Attempted to drop and recreate a collection ident which already existed, but failed "
              "the drop",
              "ident"_attr = ident,
              "uuid"_attr = uuid,
              "nss"_attr = catalogEntry.nss.toStringForErrorMsg(),
              "dropResult"_attr = dropStatus,
              "originalCreateFailure"_attr = originalFailure);
        return originalFailure;
    }

    auto createResult =
        mdbCatalog->createRecordStoreForEntry(opCtx, catalogEntry, uuid, recordStoreOptions);
    LOGV2(10526200,
          "Attempted to drop and recreate a collection ident which already existed",
          "ident"_attr = ident,
          "uuid"_attr = uuid,
          "nss"_attr = catalogEntry.nss.toStringForErrorMsg(),
          "dropResult"_attr = dropStatus,
          "createResult"_attr = createResult.getStatus());
    return createResult;
}

}  // namespace

namespace internal {
CatalogEntryMetaData createMetaDataForNewCollection(const NamespaceString& nss,
                                                    const CollectionOptions& collectionOptions) {
    const auto ns = NamespaceStringUtil::serializeForCatalog(nss);
    CatalogEntryMetaData md;
    md.nss = nss;
    md.options = collectionOptions;
    if (collectionOptions.timeseries) {
        // All newly created catalog entries for time-series collections will have this flag set
        // to false by default as mixed-schema data is only possible in versions 5.1 and
        // earlier.
        md.timeseriesBucketsMayHaveMixedSchemaData = false;
    }
    return md;
}

BSONObj buildRawMDBCatalogEntry(const std::string& ident,
                                const BSONObj& idxIdent,
                                const CatalogEntryMetaData& md,
                                const std::string& ns) {
    BSONObjBuilder b;
    b.append("ident", ident);
    b.append("idxIdent", idxIdent);
    b.append("md", md.toBSON());
    b.append("ns", ns);
    return b.obj();
}
}  // namespace internal

boost::optional<CatalogEntry> scanForCatalogEntryByNss(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       const MDBCatalog* mdbCatalog) {
    auto cursor = mdbCatalog->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();

        if (feature_document_util::isFeatureDocument(obj)) {
            // Skip over the version document because it doesn't correspond to a collection.
            continue;
        }

        auto entryNss = NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(
            obj["ns"].String());
        if (entryNss == nss) {
            return parseCatalogEntry(record->id, obj);
        }
    }

    return boost::none;
}

boost::optional<CatalogEntry> scanForCatalogEntryByUUID(OperationContext* opCtx,
                                                        const UUID& uuid,
                                                        const MDBCatalog* mdbCatalog) {
    auto cursor = mdbCatalog->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();
        auto entry = parseCatalogEntry(record->id, obj);
        if (entry && entry->metadata->options.uuid == uuid) {
            return entry;
        }
    }

    return boost::none;
}

boost::optional<CatalogEntry> getParsedCatalogEntry(OperationContext* opCtx,
                                                    const RecordId& catalogId,
                                                    const MDBCatalog* mdbCatalog) {
    auto cursor = mdbCatalog->getCursor(opCtx);
    BSONObj obj = findEntry(*cursor, catalogId);
    return parseCatalogEntry(catalogId, obj);
}

void putMetaData(OperationContext* opCtx,
                 const RecordId& catalogId,
                 durable_catalog::CatalogEntryMetaData& md,
                 MDBCatalog* mdbCatalog,
                 boost::optional<BSONObj> indexIdents) {
    auto cursor = mdbCatalog->getCursor(opCtx);
    auto entryObj = findEntry(*cursor, catalogId);

    // rebuilt doc
    BSONObjBuilder b;
    b.append("md", md.toBSON());

    if (indexIdents) {
        b.append("idxIdent", *indexIdents);
    } else {
        // Rebuild idxIdent, validating that idents are present for all indexes and discarding
        // idents for any indexes that no longer exist
        BSONObjBuilder newIdentMap;
        BSONObj oldIdentMap;
        if (auto idxIdent = entryObj["idxIdent"]; !idxIdent.eoo()) {
            oldIdentMap = idxIdent.Obj();
        }

        for (auto&& index : md.indexes) {
            if (!index.isPresent()) {
                continue;
            }

            // All indexes with buildUUIDs must be ready:false.
            invariant(!(index.buildUUID && index.ready), str::stream() << md.toBSON(true));

            auto ident = oldIdentMap.getField(index.nameStringData());
            invariant(ident.type() == BSONType::string, index.nameStringData());
            newIdentMap.append(index.nameStringData(), ident.valueStringData());
        }

        b.append("idxIdent", newIdentMap.obj());
    }


    // Add whatever is left
    b.appendElementsUnique(entryObj);

    mdbCatalog->putUpdatedEntry(opCtx, catalogId, b.obj());
}

StatusWith<std::unique_ptr<RecordStore>> createCollection(
    OperationContext* opCtx,
    const RecordId& catalogId,
    const NamespaceString& nss,
    const std::string& ident,
    const CollectionOptions& collectionOptions,
    MDBCatalog* mdbCatalog) {
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IX));
    invariant(nss.coll().size() > 0);

    auto recordStoreOptions = getRecordStoreOptions(nss, collectionOptions);

    durable_catalog::CatalogEntryMetaData md =
        internal::createMetaDataForNewCollection(nss, collectionOptions);
    const auto ns = NamespaceStringUtil::serializeForCatalog(nss);
    auto mdbCatalogEntryObj =
        internal::buildRawMDBCatalogEntry(ident, BSONObj() /* idxIdent */, md, ns);
    auto swCatalogEntry = mdbCatalog->addEntry(opCtx, ident, nss, mdbCatalogEntryObj, catalogId);
    if (!swCatalogEntry.isOK()) {
        return swCatalogEntry.getStatus();
    }
    const auto& catalogEntry = swCatalogEntry.getValue();
    invariant(catalogEntry.catalogId == catalogId);

    auto createResult = mdbCatalog->createRecordStoreForEntry(
        opCtx, catalogEntry, md.options.uuid, recordStoreOptions);
    if (createResult.getStatus() == ErrorCodes::ObjectAlreadyExists) {
        createResult = retryCreateCollectionIfObjectAlreadyExists(opCtx,
                                                                  catalogEntry,
                                                                  md.options.uuid,
                                                                  recordStoreOptions,
                                                                  createResult.getStatus(),
                                                                  mdbCatalog);
    }
    return createResult;
}

Status createIndex(OperationContext* opCtx,
                   const RecordId& catalogId,
                   const NamespaceString& nss,
                   const CollectionOptions& collectionOptions,
                   const IndexConfig& indexConfig,
                   StringData ident) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto kvEngine = storageEngine->getEngine();
    auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();

    invariant(collectionOptions.uuid);

    bool replicateLocalCatalogIdentifiers = shouldReplicateLocalCatalogIdentifers(
        rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider(),
        VersionContext::getDecoration(opCtx));
    if (replicateLocalCatalogIdentifiers) {
        // If a previous attempt at creating this index was rolled back, the ident may still be drop
        // pending. Complete that drop before creating the index if so.
        if (Status status = storageEngine->immediatelyCompletePendingDrop(opCtx, ident);
            !status.isOK()) {
            LOGV2(10526400,
                  "Index ident was drop pending and required completing the drop",
                  "ident"_attr = ident,
                  "error"_attr = status);
            return status;
        }
    }

    Status status = kvEngine->createSortedDataInterface(
        provider,
        ru,
        nss,
        *collectionOptions.uuid,
        ident,
        indexConfig,
        collectionOptions.indexOptionDefaults.getStorageEngine());

    if (status.isOK()) {
        if (replicateLocalCatalogIdentifiers) {
            ru.onRollback([storageEngine, ident = std::string(ident), &ru](OperationContext*) {
                storageEngine->addDropPendingIdent(Timestamp::min(),
                                                   std::make_shared<Ident>(ident));
            });
        } else {
            ru.onRollback([kvEngine, ident = std::string(ident), &ru](OperationContext*) {
                // Intentionally ignoring failure.
                kvEngine->dropIdent(ru, ident, false).ignore();
            });
        }
    }
    return status;
}

StatusWith<ImportResult> importCollection(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const BSONObj& metadata,
                                          const BSONObj& storageMetadata,
                                          bool generateNewUUID,
                                          MDBCatalog* mdbCatalog,
                                          bool panicOnCorruptWtMetadata,
                                          bool repair) {
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_X));
    invariant(nss.coll().size() > 0);

    // Ensure the 'md' can be parsed.
    const BSONElement mdElement = metadata["md"];
    uassert(ErrorCodes::BadValue, "Malformed catalog metadata", mdElement.isABSONObj());

    durable_catalog::CatalogEntryMetaData md;
    md.parse(mdElement.Obj());

    uassert(ErrorCodes::BadValue,
            "Attempted to import catalog entry without an ident",
            metadata.hasField("ident"));

    const auto& catalogEntry = [&] {
        if (generateNewUUID) {
            // Generate a new UUID for the collection.
            md.options.uuid = UUID::gen();
            BSONObjBuilder catalogEntryBuilder;
            // Generate a new "md" field after setting the new UUID.
            catalogEntryBuilder.append("md", md.toBSON());
            // Append the rest of the metadata.
            catalogEntryBuilder.appendElementsUnique(metadata);
            return catalogEntryBuilder.obj();
        }
        return metadata;
    }();
    RecordStore::Options recordStoreOptions = getRecordStoreOptions(nss, md.options);
    auto importResult = mdbCatalog->importCatalogEntry(opCtx,
                                                       nss,
                                                       *md.options.uuid,
                                                       recordStoreOptions,
                                                       catalogEntry,
                                                       storageMetadata,
                                                       panicOnCorruptWtMetadata,
                                                       repair);
    if (!importResult.isOK()) {
        return importResult.getStatus();
    }
    std::pair<RecordId, std::unique_ptr<RecordStore>> catalogIdRecordStorePair =
        std::move(importResult.getValue());
    return ImportResult(std::move(catalogIdRecordStorePair.first),
                        std::move(catalogIdRecordStorePair.second),
                        md.options.uuid.value());
}

Status renameCollection(OperationContext* opCtx,
                        const RecordId& catalogId,
                        const NamespaceString& toNss,
                        durable_catalog::CatalogEntryMetaData& md,
                        MDBCatalog* mdbCatalog) {
    auto cursor = mdbCatalog->getCursor(opCtx);
    BSONObj old = findEntry(*cursor, catalogId);
    BSONObjBuilder b;

    b.append("ns", NamespaceStringUtil::serializeForCatalog(toNss));
    b.append("md", md.toBSON());

    b.appendElementsUnique(old);

    BSONObj renamedEntry = b.obj();
    return mdbCatalog->putRenamedEntry(opCtx, catalogId, toNss, renamedEntry);
}

Status dropCollection(OperationContext* opCtx, const RecordId& catalogId, MDBCatalog* mdbCatalog) {
    // First, validate it is safe to remove the collection's catalog entry.
    auto parsedEntry = getParsedCatalogEntry(opCtx, catalogId, mdbCatalog);
    if (parsedEntry) {
        invariant(parsedEntry->metadata->getTotalIndexCount() == 0);
    }

    return mdbCatalog->removeEntry(opCtx, catalogId);
}

Status dropAndRecreateIndexIdentForResume(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const CollectionOptions& collectionOptions,
                                          const IndexConfig& indexConfig,
                                          StringData ident) {
    auto engine = opCtx->getServiceContext()->getStorageEngine()->getEngine();
    auto status =
        engine->dropSortedDataInterface(*shard_role_details::getRecoveryUnit(opCtx), ident);
    if (!status.isOK())
        return status;

    invariant(collectionOptions.uuid);
    auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
    status =
        engine->createSortedDataInterface(provider,
                                          *shard_role_details::getRecoveryUnit(opCtx),
                                          nss,
                                          *collectionOptions.uuid,
                                          ident,
                                          indexConfig,
                                          collectionOptions.indexOptionDefaults.getStorageEngine());

    return status;
}

void getReadyIndexes(OperationContext* opCtx,
                     RecordId catalogId,
                     StringSet* names,
                     const MDBCatalog* mdbCatalog) {
    auto catalogEntry = getParsedCatalogEntry(opCtx, catalogId, mdbCatalog);
    if (!catalogEntry)
        return;

    auto md = catalogEntry->metadata;
    for (const auto& index : md->indexes) {
        if (index.ready)
            names->insert(index.spec["name"].String());
    }
}

bool isIndexPresent(OperationContext* opCtx,
                    const RecordId& catalogId,
                    StringData indexName,
                    const MDBCatalog* mdbCatalog) {
    auto catalogEntry = getParsedCatalogEntry(opCtx, catalogId, mdbCatalog);
    if (!catalogEntry)
        return false;

    int offset = catalogEntry->metadata->findIndexOffset(indexName);
    return offset >= 0;
}

boost::optional<CatalogEntry> parseCatalogEntry(const RecordId& catalogId, const BSONObj& obj) {
    if (obj.isEmpty()) {
        return boost::none;
    }

    // For backwards compatibility where older version have a written feature document. This
    // document cannot be parsed into a CatalogEntry. See SERVER-57125.
    if (feature_document_util::isFeatureDocument(obj)) {
        return boost::none;
    }

    BSONElement idxIdent = obj["idxIdent"];
    BSONObj idxIdentObj = idxIdent.eoo() ? BSONObj() : idxIdent.Obj().getOwned();
    return CatalogEntry{catalogId, obj["ident"].String(), idxIdentObj, parseMetaData(obj["md"])};
}

}  // namespace mongo::durable_catalog
