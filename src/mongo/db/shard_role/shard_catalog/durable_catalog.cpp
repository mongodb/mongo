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

#include "mongo/db/shard_role/shard_catalog/durable_catalog.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/database_name.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/shard_role/shard_catalog/collection_record_store_options.h"
#include "mongo/db/shard_role/shard_catalog/durable_catalog_entry_metadata.h"
#include "mongo/db/shard_role/transaction_resources.h"
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
}  // namespace

namespace internal {
CatalogEntryMetaData createMetaDataForNewCollection(const NamespaceString& nss,
                                                    const CollectionOptions& collectionOptions) {
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
                                const NamespaceString& nss) {
    BSONObjBuilder b;
    b.append("ident", ident);
    b.append("idxIdent", idxIdent);
    b.append("md", md.toBSON());
    b.append("ns", NamespaceStringUtil::serializeForCatalog(nss));
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

namespace {
/**
 * Creates the underlying storage for a collection or index, handling cases where the ident is
 * transiently in use but can be safely dropped. The actual creation is performed by the `create`
 * callback argument, which should attempt to create the record store or SDI and return whatever
 * error that produces. The `identExists` callback should scan the catalog for the ident and check
 * if it's already present. This callback is only invoked after an ident conflict has been detected,
 * so it can perform checks which are too expensive for the happy path.
 */
Status createStorage(OperationContext* opCtx,
                     StringData ident,
                     function_ref<Status()> create,
                     function_ref<bool()> identExists) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();

    // Rolling back table creation performs a two-phase drop, so this ident may be pending drop. If
    // it is we'll need to complete the drop before we can proceed. If it isn't, this is a no-op.
    if (auto status = storageEngine->immediatelyCompletePendingDrop(opCtx, ident); !status.isOK()) {
        LOGV2(11093000,
              "Ident being created was drop-pending and could not be dropped immediately",
              "ident"_attr = ident,
              "error"_attr = status);
        return status;
    }

    Status status = create();
    if (status == ErrorCodes::ObjectAlreadyExists) {
        // The ident is already in use (and wasn't drop pending). Check if the ident is already
        // found in the catalog, which would mean that we've either hit a bug or an admin user did
        // something invalid with oplog editing or applyOps. This is an expensive check, so we do it
        // only after optimistically trying the creation the first time.
        if (identExists()) {
            LOGV2(11093001,
                  "Ident being created is already in the catalog and so cannot be dropped",
                  "ident"_attr = ident);
            return status;
        }

        // The ident isn't in the catalog and isn't drop-pending, so we can safely drop it, which is
        // also what would happen if we were to reload the catalog. This can happen if a table is
        // created while a checkpoint is in progress, as DDL operations being non-transactional
        // means that the table *might* be included in the checkpoint despite not existing at the
        // checkpoint's timestamp.
        auto dropStatus = storageEngine->getEngine()->dropIdent(ru, ident, true);
        if (!dropStatus.isOK()) {
            LOGV2(11093002,
                  "Ident being created is known to the storage engine but not in the catalog, but "
                  "could not be dropped",
                  "ident"_attr = ident,
                  "error"_attr = dropStatus);
            return dropStatus;
        }

        // Try again after dropping the existing table. This is expected to always work.
        status = create();
    }

    if (!status.isOK()) {
        return status;
    }

    ru.onRollback([ident = std::string(ident)](OperationContext* opCtx) {
        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        storageEngine->addDropPendingIdent(Timestamp::min(), std::make_shared<Ident>(ident));
    });

    return status;
}
}  // namespace

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

    auto engine = opCtx->getServiceContext()->getStorageEngine()->getEngine();
    auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
    auto status = createStorage(
        opCtx,
        ident,
        [&] {
            return engine->createRecordStore(provider,
                                             *shard_role_details::getRecoveryUnit(opCtx),
                                             nss,
                                             ident,
                                             recordStoreOptions);
        },
        [&] { return mdbCatalog->hasCollectionIdent(opCtx, ident); });
    if (!status.isOK()) {
        return status;
    }

    // Update the catalog only after successfully creating the record store. In between attempts at
    // creating the record store we check if the catalog already contains the ident we're trying to
    // create, and that's easier to do if we haven't already added our entry containing the ident.
    auto mdbCatalogEntryObj =
        internal::buildRawMDBCatalogEntry(ident, BSONObj() /* idxIdent */, md, nss);
    status = mdbCatalog->addEntry(opCtx, ident, nss, mdbCatalogEntryObj, catalogId).getStatus();
    if (!status.isOK()) {
        return status;
    }

    auto rs = engine->getRecordStore(opCtx, nss, ident, recordStoreOptions, collectionOptions.uuid);
    invariant(rs);
    return rs;
}

Status createIndex(OperationContext* opCtx,
                   const RecordId& catalogId,
                   const NamespaceString& nss,
                   const CollectionOptions& collectionOptions,
                   const IndexConfig& indexConfig,
                   StringData ident) {
    invariant(collectionOptions.uuid);

    auto mdbCatalog = MDBCatalog::get(opCtx);
    auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    auto engine = opCtx->getServiceContext()->getStorageEngine()->getEngine();

    return createStorage(
        opCtx,
        ident,
        [&] {
            return engine->createSortedDataInterface(
                provider,
                ru,
                nss,
                *collectionOptions.uuid,
                ident,
                indexConfig,
                collectionOptions.indexOptionDefaults.getStorageEngine());
        },
        [&] { return mdbCatalog->hasIndexIdent(opCtx, ident); });
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
