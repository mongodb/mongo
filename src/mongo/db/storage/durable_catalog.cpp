/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/storage/durable_catalog.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/catalog/collection_record_store_options.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/feature_document_util.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/transaction_resources.h"
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

namespace mongo {
namespace durable_catalog {
namespace internal {

// Finds the durable catalog entry using the provided RecordStore cursor. The returned BSONObj is
// unowned and is only valid while the cursor is positioned.
BSONObj findEntry(SeekableRecordCursor& cursor, const RecordId& catalogId) {
    auto record = cursor.seekExact(catalogId);
    if (!record) {
        return BSONObj();
    }

    return record->data.releaseToBson();
}

std::shared_ptr<BSONCollectionCatalogEntry::MetaData> parseMetaData(const BSONElement& mdElement) {
    std::shared_ptr<BSONCollectionCatalogEntry::MetaData> md;
    if (mdElement.isABSONObj()) {
        LOGV2_DEBUG(22210, 3, "returning metadata: {mdElement}", "mdElement"_attr = mdElement);
        md = std::make_shared<BSONCollectionCatalogEntry::MetaData>();
        md->parse(mdElement.Obj());
    }
    return md;
}

BSONCollectionCatalogEntry::MetaData createMetaDataForNewCollection(
    const NamespaceString& nss, const CollectionOptions& collectionOptions) {
    const auto ns = NamespaceStringUtil::serializeForCatalog(nss);
    BSONCollectionCatalogEntry::MetaData md;
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
                                const BSONCollectionCatalogEntry::MetaData& md,
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
    BSONObj obj = internal::findEntry(*cursor, catalogId);
    return parseCatalogEntry(catalogId, obj);
}

void putMetaData(OperationContext* opCtx,
                 const RecordId& catalogId,
                 BSONCollectionCatalogEntry::MetaData& md,
                 MDBCatalog* mdbCatalog) {
    BSONObj rawMDBCatalogEntry = [&] {
        auto cursor = mdbCatalog->getCursor(opCtx);
        auto entryObj = internal::findEntry(*cursor, catalogId);

        // rebuilt doc
        BSONObjBuilder b;
        b.append("md", md.toBSON());

        BSONObjBuilder newIdentMap;
        BSONObj oldIdentMap;
        if (entryObj["idxIdent"].isABSONObj())
            oldIdentMap = entryObj["idxIdent"].Obj();

        for (size_t i = 0; i < md.indexes.size(); i++) {
            const auto& index = md.indexes[i];
            if (!index.isPresent()) {
                continue;
            }

            auto name = index.nameStringData();

            // All indexes with buildUUIDs must be ready:false.
            invariant(!(index.buildUUID && index.ready), str::stream() << md.toBSON(true));

            // fix ident map
            BSONElement e = oldIdentMap[name];
            if (e.type() == String) {
                newIdentMap.append(e);
                continue;
            }

            // The index in 'md.indexes' is missing a corresponding 'idxIdent' - which indicates
            // there is a new index to track in the catalog, and the catlog must generate an ident
            // to associate with it.
            //
            // TODO SERVER-102875: Compute the ident for a new index outside the durable_catalog.
            newIdentMap.append(
                name,
                ident::generateNewIndexIdent(md.nss.dbName(),
                                             mdbCatalog->isUsingDirectoryPerDb(),
                                             mdbCatalog->isUsingDirectoryForIndexes()));
        }
        b.append("idxIdent", newIdentMap.obj());

        // Add whatever is left
        b.appendElementsUnique(entryObj);
        return b.obj();
    }();

    mdbCatalog->putUpdatedEntry(opCtx, catalogId, rawMDBCatalogEntry);
}

StatusWith<std::string> newOrphanedIdent(OperationContext* opCtx,
                                         const std::string& ident,
                                         const CollectionOptions& optionsWithUUID,
                                         MDBCatalog* mdbCatalog) {
    // The collection will be named local.orphan.xxxxx.
    std::string identNs = ident;
    std::replace(identNs.begin(), identNs.end(), '-', '_');
    const auto nss = NamespaceStringUtil::deserialize(
        DatabaseName::kLocal, NamespaceString::kOrphanCollectionPrefix + identNs);
    auto ns = NamespaceStringUtil::serializeForCatalog(nss);


    BSONCollectionCatalogEntry::MetaData md =
        internal::createMetaDataForNewCollection(nss, optionsWithUUID);
    auto catalogEntryObj =
        internal::buildRawMDBCatalogEntry(ident, BSONObj() /* idxIdent */, md, ns);

    auto res = mdbCatalog->addOrphanedEntry(opCtx, ident, nss, catalogEntryObj);
    if (!res.isOK()) {
        return res.getStatus();
    }
    return {ns};
}

StatusWith<std::pair<RecordId, std::unique_ptr<RecordStore>>> createCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::string& ident,
    const CollectionOptions& collectionOptions,
    MDBCatalog* mdbCatalog) {
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IX));
    invariant(nss.coll().size() > 0);

    auto recordStoreOptions = getRecordStoreOptions(nss, collectionOptions);

    BSONCollectionCatalogEntry::MetaData md =
        internal::createMetaDataForNewCollection(nss, collectionOptions);
    const auto ns = NamespaceStringUtil::serializeForCatalog(nss);
    auto mdbCatalogEntryObj =
        internal::buildRawMDBCatalogEntry(ident, BSONObj() /* idxIdent */, md, ns);

    return mdbCatalog->initializeNewEntry(
        opCtx, md.options.uuid, ident, nss, recordStoreOptions, mdbCatalogEntryObj);
}

Status createIndex(OperationContext* opCtx,
                   const RecordId& catalogId,
                   const NamespaceString& nss,
                   const CollectionOptions& collectionOptions,
                   const IndexConfig& indexConfig,
                   MDBCatalog* mdbCatalog) {
    std::string ident = mdbCatalog->getIndexIdent(opCtx, catalogId, indexConfig.indexName);
    auto engine = opCtx->getServiceContext()->getStorageEngine()->getEngine();
    invariant(collectionOptions.uuid);
    Status status =
        engine->createSortedDataInterface(*shard_role_details::getRecoveryUnit(opCtx),
                                          nss,
                                          *collectionOptions.uuid,
                                          ident,
                                          indexConfig,
                                          collectionOptions.indexOptionDefaults.getStorageEngine());
    if (status.isOK()) {
        shard_role_details::getRecoveryUnit(opCtx)->onRollback(
            [engine, ident, recoveryUnit = shard_role_details::getRecoveryUnit(opCtx)](
                OperationContext*) {
                // Intentionally ignoring failure.
                engine->dropIdent(recoveryUnit, ident, /*identHasSizeInfo=*/false).ignore();
            });
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

    BSONCollectionCatalogEntry::MetaData md;
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
                        BSONCollectionCatalogEntry::MetaData& md,
                        MDBCatalog* mdbCatalog) {
    auto cursor = mdbCatalog->getCursor(opCtx);
    BSONObj old = internal::findEntry(*cursor, catalogId);
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
    status =
        engine->createSortedDataInterface(*shard_role_details::getRecoveryUnit(opCtx),
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
    return CatalogEntry{catalogId,
                        obj["ident"].String(),
                        idxIdent.eoo() ? BSONObj() : idxIdent.Obj().getOwned(),
                        internal::parseMetaData(obj["md"])};
}

}  // namespace durable_catalog
}  // namespace mongo
