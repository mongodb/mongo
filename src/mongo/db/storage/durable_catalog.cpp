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

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <memory>
#include <set>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/feature_document_util.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
class DurableCatalog::AddIdentChange : public RecoveryUnit::Change {
public:
    AddIdentChange(DurableCatalog* catalog, RecordId catalogId)
        : _catalog(catalog), _catalogId(std::move(catalogId)) {}

    void commit(OperationContext* opCtx, boost::optional<Timestamp>) noexcept override {}
    void rollback(OperationContext* opCtx) noexcept override {
        stdx::lock_guard<stdx::mutex> lk(_catalog->_catalogIdToEntryMapLock);
        _catalog->_catalogIdToEntryMap.erase(_catalogId);
    }

private:
    DurableCatalog* const _catalog;
    const RecordId _catalogId;
};

DurableCatalog::DurableCatalog(RecordStore* rs,
                               bool directoryPerDb,
                               bool directoryForIndexes,
                               KVEngine* engine)
    : _rs(rs),
      _directoryPerDb(directoryPerDb),
      _directoryForIndexes(directoryForIndexes),
      _engine(engine) {}

void DurableCatalog::init(OperationContext* opCtx) {
    // No locking needed since called single threaded.
    auto cursor = _rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();

        // For backwards compatibility where older version have a written feature document
        if (feature_document_util::isFeatureDocument(obj)) {
            continue;
        }

        // No rollback since this is just loading already committed data.
        auto ident = obj["ident"].String();
        auto nss = NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(
            obj["ns"].String());
        _catalogIdToEntryMap[record->id] = EntryIdentifier(record->id, ident, nss);
    }
}

std::vector<DurableCatalog::EntryIdentifier> DurableCatalog::getAllCatalogEntries(
    OperationContext* opCtx) const {
    std::vector<DurableCatalog::EntryIdentifier> ret;

    auto cursor = _rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();
        if (feature_document_util::isFeatureDocument(obj)) {
            // Skip over the version document because it doesn't correspond to a collection.
            continue;
        }
        auto ident = obj["ident"].String();
        auto nss = NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(
            obj["ns"].String());

        ret.emplace_back(record->id, ident, nss);
    }

    return ret;
}

boost::optional<DurableCatalogEntry> DurableCatalog::scanForCatalogEntryByNss(
    OperationContext* opCtx, const NamespaceString& nss) const {
    auto cursor = _rs->getCursor(opCtx);
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

boost::optional<DurableCatalogEntry> DurableCatalog::scanForCatalogEntryByUUID(
    OperationContext* opCtx, const UUID& uuid) const {
    auto cursor = _rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();
        auto entry = parseCatalogEntry(record->id, obj);
        if (entry && entry->metadata->options.uuid == uuid) {
            return entry;
        }
    }

    return boost::none;
}

DurableCatalog::EntryIdentifier DurableCatalog::getEntry(const RecordId& catalogId) const {
    stdx::lock_guard<stdx::mutex> lk(_catalogIdToEntryMapLock);
    auto it = _catalogIdToEntryMap.find(catalogId);
    invariant(it != _catalogIdToEntryMap.end());
    return it->second;
}

NamespaceString DurableCatalog::getNSSFromCatalog(OperationContext* opCtx,
                                                  const RecordId& catalogId) const {
    {
        stdx::lock_guard<stdx::mutex> lk(_catalogIdToEntryMapLock);
        auto it = _catalogIdToEntryMap.find(catalogId);
        if (it != _catalogIdToEntryMap.end()) {
            return it->second.nss;
        }
    }

    // Re-read the catalog at the provided timestamp in case the collection was dropped.
    auto cursor = _rs->getCursor(opCtx);
    BSONObj obj = _findEntry(*cursor, catalogId);
    if (!obj.isEmpty()) {
        return NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(
            obj["ns"].String());
    }

    tassert(9117800, str::stream() << "Namespace not found for " << catalogId, false);
}

StatusWith<DurableCatalog::EntryIdentifier> DurableCatalog::_addEntry(
    OperationContext* opCtx,
    NamespaceString nss,
    const std::string& ident,
    const CollectionOptions& options) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_IX));

    BSONObj obj;
    {
        BSONObjBuilder b;
        b.append("ns", NamespaceStringUtil::serializeForCatalog(nss));
        b.append("ident", ident);
        BSONCollectionCatalogEntry::MetaData md;
        md.nss = nss;
        md.options = options;

        if (options.timeseries) {
            // All newly created catalog entries for time-series collections will have this flag set
            // to false by default as mixed-schema data is only possible in versions 5.1 and
            // earlier.
            md.timeseriesBucketsMayHaveMixedSchemaData = false;
        }
        b.append("md", md.toBSON());
        obj = b.obj();
    }
    StatusWith<RecordId> res = _rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), Timestamp());
    if (!res.isOK())
        return res.getStatus();

    stdx::lock_guard<stdx::mutex> lk(_catalogIdToEntryMapLock);
    invariant(_catalogIdToEntryMap.find(res.getValue()) == _catalogIdToEntryMap.end());
    _catalogIdToEntryMap[res.getValue()] = {res.getValue(), ident, nss};
    shard_role_details::getRecoveryUnit(opCtx)->registerChange(
        std::make_unique<AddIdentChange>(this, res.getValue()));

    LOGV2_DEBUG(22207,
                1,
                "stored meta data for {nss} @ {res_getValue}",
                logAttrs(nss),
                "res_getValue"_attr = res.getValue());
    return {{res.getValue(), ident, nss}};
}

StatusWith<DurableCatalog::EntryIdentifier> DurableCatalog::_importEntry(OperationContext* opCtx,
                                                                         NamespaceString nss,
                                                                         const BSONObj& metadata) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_IX));

    auto ident = metadata["ident"].String();
    StatusWith<RecordId> res =
        _rs->insertRecord(opCtx, metadata.objdata(), metadata.objsize(), Timestamp());
    if (!res.isOK())
        return res.getStatus();

    stdx::lock_guard<stdx::mutex> lk(_catalogIdToEntryMapLock);
    invariant(_catalogIdToEntryMap.find(res.getValue()) == _catalogIdToEntryMap.end());
    _catalogIdToEntryMap[res.getValue()] = {res.getValue(), ident, nss};
    shard_role_details::getRecoveryUnit(opCtx)->registerChange(
        std::make_unique<AddIdentChange>(this, res.getValue()));

    LOGV2_DEBUG(5095101, 1, "imported meta data", logAttrs(nss), "metadata"_attr = res.getValue());
    return {{res.getValue(), ident, nss}};
}

std::string DurableCatalog::getIndexIdent(OperationContext* opCtx,
                                          const RecordId& catalogId,
                                          StringData idxName) const {
    auto cursor = _rs->getCursor(opCtx);
    BSONObj obj = _findEntry(*cursor, catalogId);
    BSONObj idxIdent = obj["idxIdent"].Obj();
    return idxIdent[idxName].String();
}

std::vector<std::string> DurableCatalog::getIndexIdents(OperationContext* opCtx,
                                                        const RecordId& catalogId) const {
    std::vector<std::string> idents;

    auto cursor = _rs->getCursor(opCtx);
    BSONObj obj = _findEntry(*cursor, catalogId);
    if (obj["idxIdent"].eoo()) {
        // No index entries for this catalog entry.
        return idents;
    }

    BSONObj idxIdent = obj["idxIdent"].Obj();

    BSONObjIterator it(idxIdent);
    while (it.more()) {
        BSONElement elem = it.next();
        idents.push_back(elem.String());
    }

    return idents;
}

BSONObj DurableCatalog::_findEntry(SeekableRecordCursor& cursor, const RecordId& catalogId) const {
    LOGV2_DEBUG(22208, 3, "looking up metadata for: {catalogId}", "catalogId"_attr = catalogId);
    auto record = cursor.seekExact(catalogId);
    if (!record) {
        return BSONObj();
    }

    return record->data.releaseToBson();
}

boost::optional<DurableCatalogEntry> DurableCatalog::getParsedCatalogEntry(
    OperationContext* opCtx, const RecordId& catalogId) const {
    auto cursor = _rs->getCursor(opCtx);
    BSONObj obj = _findEntry(*cursor, catalogId);
    return parseCatalogEntry(catalogId, obj);
}

std::shared_ptr<BSONCollectionCatalogEntry::MetaData> DurableCatalog::_parseMetaData(
    const BSONElement& mdElement) const {
    std::shared_ptr<BSONCollectionCatalogEntry::MetaData> md;
    if (mdElement.isABSONObj()) {
        LOGV2_DEBUG(22210, 3, "returning metadata: {mdElement}", "mdElement"_attr = mdElement);
        md = std::make_shared<BSONCollectionCatalogEntry::MetaData>();
        md->parse(mdElement.Obj());
    }
    return md;
}

void DurableCatalog::putMetaData(OperationContext* opCtx,
                                 const RecordId& catalogId,
                                 BSONCollectionCatalogEntry::MetaData& md) {
    BSONObj obj = [&] {
        auto cursor = _rs->getCursor(opCtx);
        auto entryObj = _findEntry(*cursor, catalogId);

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
            // TODO SERVER-102875: Compute the ident for a new index outside the DurableCatalog.
            newIdentMap.append(name,
                               ident::generateNewIndexIdent(
                                   md.nss.dbName(), _directoryPerDb, _directoryForIndexes));
        }
        b.append("idxIdent", newIdentMap.obj());

        // add whatever is left
        b.appendElementsUnique(entryObj);
        return b.obj();
    }();

    LOGV2_DEBUG(22211, 3, "recording new metadata: {obj}", "obj"_attr = obj);
    Status status = _rs->updateRecord(opCtx, catalogId, obj.objdata(), obj.objsize());
    fassert(28521, status);
}

Status DurableCatalog::_removeEntry(OperationContext* opCtx, const RecordId& catalogId) {
    stdx::lock_guard<stdx::mutex> lk(_catalogIdToEntryMapLock);
    const auto it = _catalogIdToEntryMap.find(catalogId);
    if (it == _catalogIdToEntryMap.end()) {
        return Status(ErrorCodes::NamespaceNotFound, "collection not found");
    }

    shard_role_details::getRecoveryUnit(opCtx)->onRollback(
        [this, catalogId, entry = it->second](OperationContext*) {
            stdx::lock_guard<stdx::mutex> lk(_catalogIdToEntryMapLock);
            _catalogIdToEntryMap[catalogId] = entry;
        });

    LOGV2_DEBUG(22212,
                1,
                "deleting metadata for {it_second_namespace} @ {catalogId}",
                "it_second_namespace"_attr = it->second.nss,
                "catalogId"_attr = catalogId);
    _rs->deleteRecord(opCtx, catalogId);
    _catalogIdToEntryMap.erase(it);

    return Status::OK();
}

std::vector<std::string> DurableCatalog::getAllIdents(OperationContext* opCtx) const {
    std::vector<std::string> v;

    auto cursor = _rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();
        if (feature_document_util::isFeatureDocument(obj)) {
            // Skip over the version document because it doesn't correspond to a namespace entry and
            // therefore doesn't refer to any idents.
            continue;
        }
        v.push_back(obj["ident"].String());

        BSONElement e = obj["idxIdent"];
        if (!e.isABSONObj())
            continue;
        BSONObj idxIdent = e.Obj();

        BSONObjIterator sub(idxIdent);
        while (sub.more()) {
            BSONElement e = sub.next();
            v.push_back(e.String());
        }
    }

    return v;
}

StatusWith<std::string> DurableCatalog::newOrphanedIdent(OperationContext* opCtx,
                                                         std::string ident,
                                                         const CollectionOptions& optionsWithUUID) {
    // The collection will be named local.orphan.xxxxx.
    std::string identNs = ident;
    std::replace(identNs.begin(), identNs.end(), '-', '_');
    const auto nss = NamespaceStringUtil::deserialize(
        DatabaseName::kLocal, NamespaceString::kOrphanCollectionPrefix + identNs);

    BSONObj obj;
    {
        BSONObjBuilder b;
        b.append("ns", NamespaceStringUtil::serializeForCatalog(nss));
        b.append("ident", ident);
        BSONCollectionCatalogEntry::MetaData md;
        md.nss = nss;
        // Default options with newly generated UUID.
        md.options = optionsWithUUID;
        b.append("md", md.toBSON());
        obj = b.obj();
    }
    StatusWith<RecordId> res = _rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), Timestamp());
    if (!res.isOK())
        return res.getStatus();

    stdx::lock_guard<stdx::mutex> lk(_catalogIdToEntryMapLock);
    invariant(_catalogIdToEntryMap.find(res.getValue()) == _catalogIdToEntryMap.end());
    _catalogIdToEntryMap[res.getValue()] = EntryIdentifier(res.getValue(), ident, nss);
    shard_role_details::getRecoveryUnit(opCtx)->registerChange(
        std::make_unique<AddIdentChange>(this, res.getValue()));

    LOGV2_DEBUG(22213,
                1,
                "stored meta data for orphaned collection {namespace} @ {res_getValue}",
                logAttrs(nss),
                "res_getValue"_attr = res.getValue());
    return {NamespaceStringUtil::serializeForCatalog(nss)};
}

StatusWith<std::pair<RecordId, std::unique_ptr<RecordStore>>> DurableCatalog::createCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::string& ident,
    const CollectionOptions& options) {
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IX));
    invariant(nss.coll().size() > 0);

    StatusWith<EntryIdentifier> swEntry = _addEntry(opCtx, nss, ident, options);
    if (!swEntry.isOK())
        return swEntry.getStatus();
    EntryIdentifier& entry = swEntry.getValue();

    const auto keyFormat = [&] {
        // Clustered collections require KeyFormat::String, but the opposite is not necessarily
        // true: a clustered record store that is not associated with a collection has
        // KeyFormat::String and and no CollectionOptions.
        if (options.clusteredIndex) {
            return KeyFormat::String;
        }
        return KeyFormat::Long;
    }();
    Status status = _engine->createRecordStore(
        nss, entry.ident, keyFormat, options.timeseries.has_value(), options.storageEngine);
    if (!status.isOK())
        return status;

    auto ru = shard_role_details::getRecoveryUnit(opCtx);
    shard_role_details::getRecoveryUnit(opCtx)->onRollback(
        [ru, catalog = this, ident = entry.ident](OperationContext*) {
            // Intentionally ignoring failure
            catalog->_engine->dropIdent(ru, ident, /*identHasSizeInfo=*/true).ignore();
        });

    auto rs = _engine->getRecordStore(opCtx, nss, entry.ident, options);
    invariant(rs);

    return std::pair<RecordId, std::unique_ptr<RecordStore>>(entry.catalogId, std::move(rs));
}

Status DurableCatalog::createIndex(OperationContext* opCtx,
                                   const RecordId& catalogId,
                                   const NamespaceString& nss,
                                   const CollectionOptions& collOptions,
                                   const IndexConfig& indexConfig) {
    std::string ident = getIndexIdent(opCtx, catalogId, indexConfig.indexName);

    invariant(collOptions.uuid);
    Status status =
        _engine->createSortedDataInterface(*shard_role_details::getRecoveryUnit(opCtx),
                                           nss,
                                           *collOptions.uuid,
                                           ident,
                                           indexConfig,
                                           collOptions.indexOptionDefaults.getStorageEngine());
    if (status.isOK()) {
        shard_role_details::getRecoveryUnit(opCtx)->onRollback(
            [this, ident, recoveryUnit = shard_role_details::getRecoveryUnit(opCtx)](
                OperationContext*) {
                // Intentionally ignoring failure.
                _engine->dropIdent(recoveryUnit, ident, /*identHasSizeInfo=*/false).ignore();
            });
    }
    return status;
}

StatusWith<DurableCatalog::ImportResult> DurableCatalog::importCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& metadata,
    const BSONObj& storageMetadata,
    bool generateNewUUID,
    bool panicOnCorruptWtMetadata,
    bool repair) {
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_X));
    invariant(nss.coll().size() > 0);

    BSONCollectionCatalogEntry::MetaData md;
    const BSONElement mdElement = metadata["md"];
    uassert(ErrorCodes::BadValue, "Malformed catalog metadata", mdElement.isABSONObj());
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

    std::set<std::string> indexIdents;
    if (auto&& idxIdent = catalogEntry["idxIdent"]) {
        for (auto&& indexIdent : idxIdent.Obj()) {
            indexIdents.insert(indexIdent.String());
        }
    }

    StatusWith<EntryIdentifier> swEntry = _importEntry(opCtx, nss, catalogEntry);
    if (!swEntry.isOK())
        return swEntry.getStatus();
    EntryIdentifier& entry = swEntry.getValue();

    shard_role_details::getRecoveryUnit(opCtx)->onRollback(
        [catalog = this, ident = entry.ident, indexIdents = indexIdents](OperationContext* opCtx) {
            catalog->_engine->dropIdentForImport(
                *opCtx, *shard_role_details::getRecoveryUnit(opCtx), ident);
            for (const auto& indexIdent : indexIdents) {
                catalog->_engine->dropIdentForImport(
                    *opCtx, *shard_role_details::getRecoveryUnit(opCtx), indexIdent);
            }
        });

    Status status =
        _engine->importRecordStore(entry.ident, storageMetadata, panicOnCorruptWtMetadata, repair);

    if (!status.isOK())
        return status;

    for (const auto& indexIdent : indexIdents) {
        status = _engine->importSortedDataInterface(*shard_role_details::getRecoveryUnit(opCtx),
                                                    indexIdent,
                                                    storageMetadata,
                                                    panicOnCorruptWtMetadata,
                                                    repair);
        if (!status.isOK()) {
            return status;
        }
    }

    auto rs = _engine->getRecordStore(opCtx, nss, entry.ident, md.options);
    invariant(rs);

    return DurableCatalog::ImportResult(entry.catalogId, std::move(rs), md.options.uuid.value());
}

Status DurableCatalog::renameCollection(OperationContext* opCtx,
                                        const RecordId& catalogId,
                                        const NamespaceString& toNss,
                                        BSONCollectionCatalogEntry::MetaData& md) {
    {
        auto cursor = _rs->getCursor(opCtx);
        BSONObj old = _findEntry(*cursor, catalogId);
        BSONObjBuilder b;

        b.append("ns", NamespaceStringUtil::serializeForCatalog(toNss));
        b.append("md", md.toBSON());

        b.appendElementsUnique(old);

        BSONObj obj = b.obj();
        Status status = _rs->updateRecord(opCtx, catalogId, obj.objdata(), obj.objsize());
        fassert(28522, status);
    }

    stdx::lock_guard<stdx::mutex> lk(_catalogIdToEntryMapLock);
    const auto it = _catalogIdToEntryMap.find(catalogId);
    invariant(it != _catalogIdToEntryMap.end());

    NamespaceString fromName = it->second.nss;
    it->second.nss = toNss;
    shard_role_details::getRecoveryUnit(opCtx)->onRollback(
        [this, catalogId, fromName](OperationContext*) {
            stdx::lock_guard<stdx::mutex> lk(_catalogIdToEntryMapLock);
            const auto it = _catalogIdToEntryMap.find(catalogId);
            invariant(it != _catalogIdToEntryMap.end());
            it->second.nss = fromName;
        });

    return Status::OK();
}

Status DurableCatalog::dropCollection(OperationContext* opCtx, const RecordId& catalogId) {
    EntryIdentifier entry;
    {
        stdx::lock_guard<stdx::mutex> lk(_catalogIdToEntryMapLock);
        entry = _catalogIdToEntryMap[catalogId];
    }

    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(entry.nss, MODE_X));
    invariant(getParsedCatalogEntry(opCtx, catalogId)->metadata->getTotalIndexCount() == 0);

    // Remove metadata from mdb_catalog
    Status status = _removeEntry(opCtx, catalogId);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

Status DurableCatalog::dropAndRecreateIndexIdentForResume(OperationContext* opCtx,
                                                          const NamespaceString& nss,
                                                          const CollectionOptions& collOptions,
                                                          const IndexConfig& indexConfig,
                                                          StringData ident) {
    auto status =
        _engine->dropSortedDataInterface(*shard_role_details::getRecoveryUnit(opCtx), ident);
    if (!status.isOK())
        return status;

    invariant(collOptions.uuid);
    status = _engine->createSortedDataInterface(*shard_role_details::getRecoveryUnit(opCtx),
                                                nss,
                                                *collOptions.uuid,
                                                ident,
                                                indexConfig,
                                                collOptions.indexOptionDefaults.getStorageEngine());

    return status;
}

void DurableCatalog::getReadyIndexes(OperationContext* opCtx,
                                     RecordId catalogId,
                                     StringSet* names) const {
    auto catalogEntry = getParsedCatalogEntry(opCtx, catalogId);
    if (!catalogEntry)
        return;

    auto md = catalogEntry->metadata;
    for (const auto& index : md->indexes) {
        if (index.ready)
            names->insert(index.spec["name"].String());
    }
}

bool DurableCatalog::isIndexPresent(OperationContext* opCtx,
                                    const RecordId& catalogId,
                                    StringData indexName) const {
    auto catalogEntry = getParsedCatalogEntry(opCtx, catalogId);
    if (!catalogEntry)
        return false;

    int offset = catalogEntry->metadata->findIndexOffset(indexName);
    return offset >= 0;
}

boost::optional<DurableCatalogEntry> DurableCatalog::parseCatalogEntry(const RecordId& catalogId,
                                                                       const BSONObj& obj) const {
    if (obj.isEmpty()) {
        return boost::none;
    }

    // For backwards compatibility where older version have a written feature document. This
    // document cannot be parsed into a DurableCatalogEntry. See SERVER-57125.
    if (feature_document_util::isFeatureDocument(obj)) {
        return boost::none;
    }

    BSONElement idxIdent = obj["idxIdent"];
    return DurableCatalogEntry{catalogId,
                               obj["ident"].String(),
                               idxIdent.eoo() ? BSONObj() : idxIdent.Obj().getOwned(),
                               _parseMetaData(obj["md"])};
}

}  // namespace mongo
