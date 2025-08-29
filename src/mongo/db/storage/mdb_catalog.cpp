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

#include "mongo/db/storage/mdb_catalog.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/storage/feature_document_util.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/logv2/log.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

class MDBCatalog::AddIdentChange : public RecoveryUnit::Change {
public:
    AddIdentChange(MDBCatalog* catalog, RecordId catalogId)
        : _catalog(catalog), _catalogId(std::move(catalogId)) {}

    void commit(OperationContext* opCtx, boost::optional<Timestamp>) noexcept override {}
    void rollback(OperationContext* opCtx) noexcept override {
        stdx::lock_guard<stdx::mutex> lk(_catalog->_catalogIdToEntryMapLock);
        _catalog->_catalogIdToEntryMap.erase(_catalogId);
    }

private:
    MDBCatalog* const _catalog;
    const RecordId _catalogId;
};

MDBCatalog::MDBCatalog(RecordStore* rs, KVEngine* engine) : _rs(rs), _engine(engine) {}

void MDBCatalog::init(OperationContext* opCtx) {
    // No locking needed since called single threaded.
    auto cursor = _rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
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

RecordId MDBCatalog::reserveCatalogId(OperationContext* opCtx) {
    std::vector<RecordId> reservedRids;
    _rs->reserveRecordIds(opCtx, *shard_role_details::getRecoveryUnit(opCtx), &reservedRids, 1);
    invariant(reservedRids.size() == 1);
    return std::move(reservedRids[0]);
}

std::vector<MDBCatalog::EntryIdentifier> MDBCatalog::getAllCatalogEntries(
    OperationContext* opCtx) const {
    std::vector<MDBCatalog::EntryIdentifier> ret;

    auto cursor = _rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
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

MDBCatalog::EntryIdentifier MDBCatalog::getEntry(const RecordId& catalogId) const {
    stdx::lock_guard<stdx::mutex> lk(_catalogIdToEntryMapLock);
    auto it = _catalogIdToEntryMap.find(catalogId);
    invariant(it != _catalogIdToEntryMap.end());
    return it->second;
}

boost::optional<MDBCatalog::EntryIdentifier> MDBCatalog::getEntry_forTest(
    const RecordId& catalogId) const {
    stdx::lock_guard<stdx::mutex> lk(_catalogIdToEntryMapLock);
    auto it = _catalogIdToEntryMap.find(catalogId);
    if (it != _catalogIdToEntryMap.end()) {
        return it->second;
    }
    return boost::none;
}

BSONObj MDBCatalog::getRawCatalogEntry(OperationContext* opCtx, const RecordId& catalogId) const {
    auto cursor = _rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    return _findRawEntry(*cursor, catalogId).getOwned();
}

RecordStore::Options MDBCatalog::getParsedRecordStoreOptions(OperationContext* opCtx,
                                                             const RecordId& catalogId,
                                                             const NamespaceString& nss) const {
    auto bsonEntry = getRawCatalogEntry(opCtx, catalogId);
    return _parseRecordStoreOptions(nss, bsonEntry);
}

void MDBCatalog::putUpdatedEntry(OperationContext* opCtx,
                                 const RecordId& catalogId,
                                 const BSONObj& catalogEntryObj) {
    LOGV2_DEBUG(22211, 3, "recording new metadata: ", "catalogEntryObj"_attr = catalogEntryObj);
    Status status = _rs->updateRecord(opCtx,
                                      *shard_role_details::getRecoveryUnit(opCtx),
                                      catalogId,
                                      catalogEntryObj.objdata(),
                                      catalogEntryObj.objsize());
    fassert(28521, status);
}

std::vector<std::string> MDBCatalog::getAllIdents(OperationContext* opCtx) const {
    std::vector<std::string> v;

    auto cursor = _rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
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

std::string MDBCatalog::getIndexIdent(OperationContext* opCtx,
                                      const RecordId& catalogId,
                                      StringData idxName) const {
    std::string identForIndex;
    auto cursor = _rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    BSONObj obj = _findRawEntry(*cursor, catalogId);
    BSONObj idxIdent = obj["idxIdent"].Obj();
    if (!idxIdent.isEmpty()) {
        identForIndex = idxIdent[idxName].String();
    }
    return identForIndex;
}

std::vector<std::string> MDBCatalog::getIndexIdents(OperationContext* opCtx,
                                                    const RecordId& catalogId) {

    auto cursor = _rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    BSONObj obj = _findRawEntry(*cursor, catalogId);
    return _getIndexIdents(obj);
}

std::unique_ptr<SeekableRecordCursor> MDBCatalog::getCursor(OperationContext* opCtx,
                                                            bool forward) const {
    if (!_rs) {
        return nullptr;
    }
    return _rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx), forward);
}

StatusWith<MDBCatalog::EntryIdentifier> MDBCatalog::addEntry(OperationContext* opCtx,
                                                             const std::string& ident,
                                                             const NamespaceString& nss,
                                                             const BSONObj& catalogEntryObj,
                                                             const RecordId& catalogId) {
    StatusWith<RecordId> res = _rs->insertRecord(opCtx,
                                                 *shard_role_details::getRecoveryUnit(opCtx),
                                                 catalogId,
                                                 catalogEntryObj.objdata(),
                                                 catalogEntryObj.objsize(),
                                                 Timestamp());
    if (!res.isOK())
        return res.getStatus();

    stdx::lock_guard<stdx::mutex> lk(_catalogIdToEntryMapLock);
    invariant(_catalogIdToEntryMap.find(res.getValue()) == _catalogIdToEntryMap.end());
    _catalogIdToEntryMap[res.getValue()] = EntryIdentifier(res.getValue(), ident, nss);
    shard_role_details::getRecoveryUnit(opCtx)->registerChange(
        std::make_unique<AddIdentChange>(this, res.getValue()));

    LOGV2_DEBUG(22213,
                1,
                "stored meta data for {namespace} @ {res_getValue}",
                logAttrs(nss),
                "res_getValue"_attr = res.getValue());

    return {{res.getValue(), ident, nss}};
}

StatusWith<std::unique_ptr<RecordStore>> MDBCatalog::createRecordStoreForEntry(
    OperationContext* opCtx,
    const MDBCatalog::EntryIdentifier& entry,
    const boost::optional<UUID>& uuid,
    const RecordStore::Options& recordStoreOptions) {
    auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
    Status status =
        _engine->createRecordStore(provider, entry.nss, entry.ident, recordStoreOptions);
    if (!status.isOK()) {
        return status;
    }

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    ru.onRollback([&ru, catalog = this, ident = entry.ident](OperationContext*) {
        // Intentionally ignoring failure
        catalog->_engine->dropIdent(ru, ident, /*identHasSizeInfo=*/true).ignore();
    });

    auto rs = _engine->getRecordStore(opCtx, entry.nss, entry.ident, recordStoreOptions, uuid);
    invariant(rs);
    return rs;
}

StatusWith<std::pair<RecordId, std::unique_ptr<RecordStore>>> MDBCatalog::importCatalogEntry(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& uuid,
    const RecordStore::Options& recordStoreOptions,
    const BSONObj& catalogEntryObj,
    const BSONObj& storageMetadata,
    bool panicOnCorruptWtMetadata,
    bool repair) {

    StatusWith<EntryIdentifier> swEntry = _importEntry(opCtx, nss, catalogEntryObj);
    if (!swEntry.isOK())
        return swEntry.getStatus();
    EntryIdentifier& entry = swEntry.getValue();
    auto indexIdents = _getIndexIdents(catalogEntryObj);

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

    auto rs = _engine->getRecordStore(opCtx, nss, entry.ident, recordStoreOptions, uuid);
    invariant(rs);

    return std::pair<RecordId, std::unique_ptr<RecordStore>>(entry.catalogId, std::move(rs));
}


Status MDBCatalog::removeEntry(OperationContext* opCtx, const RecordId& catalogId) {
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
    _rs->deleteRecord(opCtx, *shard_role_details::getRecoveryUnit(opCtx), catalogId);
    _catalogIdToEntryMap.erase(it);

    return Status::OK();
}

Status MDBCatalog::putRenamedEntry(OperationContext* opCtx,
                                   const RecordId& catalogId,
                                   const NamespaceString& toNss,
                                   const BSONObj& renamedEntry) {
    Status status = _rs->updateRecord(opCtx,
                                      *shard_role_details::getRecoveryUnit(opCtx),
                                      catalogId,
                                      renamedEntry.objdata(),
                                      renamedEntry.objsize());
    fassert(28522, status);

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

NamespaceString MDBCatalog::getNSSFromCatalog(OperationContext* opCtx,
                                              const RecordId& catalogId) const {
    {
        stdx::lock_guard<stdx::mutex> lk(_catalogIdToEntryMapLock);
        auto it = _catalogIdToEntryMap.find(catalogId);
        if (it != _catalogIdToEntryMap.end()) {
            return it->second.nss;
        }
    }

    // Re-read the catalog at the provided timestamp in case the collection was dropped.
    auto cursor = _rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    BSONObj obj = _findRawEntry(*cursor, catalogId);
    if (!obj.isEmpty()) {
        return NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(
            obj["ns"].String());
    }

    tassert(9117800, str::stream() << "Namespace not found for " << catalogId, false);
}

BSONObj MDBCatalog::_buildOrphanedCatalogEntryObjAndNs(
    const std::string& ident, bool isClustered, NamespaceString* nss, std::string* ns, UUID uuid) {
    // The collection will be named local.orphan.xxxxx.
    std::string identNs = ident;
    std::replace(identNs.begin(), identNs.end(), '-', '_');

    *nss = NamespaceStringUtil::deserialize(DatabaseName::kLocal,
                                            NamespaceString::kOrphanCollectionPrefix + identNs);
    *ns = NamespaceStringUtil::serializeForCatalog(*nss);
    BSONObjBuilder catalogEntryBuilder;
    catalogEntryBuilder.append("ident", ident);
    catalogEntryBuilder.append("idxIdent", BSONObj());
    BSONObjBuilder mdBuilder = catalogEntryBuilder.subobjStart("md");
    mdBuilder.append("ns", *ns);
    BSONObjBuilder optBuilder = mdBuilder.subobjStart("options");
    optBuilder.appendElements(uuid.toBSON());
    if (isClustered) {
        BSONObjBuilder indexSpecBuilder = optBuilder.subobjStart("clusteredIndex");
        indexSpecBuilder.append("v", 2);
        indexSpecBuilder.append("key", BSON("_id" << 1));
        indexSpecBuilder.append("name", "_id_");
        indexSpecBuilder.append("unique", true);
        indexSpecBuilder.doneFast();
    }
    optBuilder.doneFast();
    mdBuilder.append("indexes", BSONArray());
    mdBuilder.doneFast();
    catalogEntryBuilder.append("ns", *ns);

    return catalogEntryBuilder.obj();
}

StatusWith<std::string> MDBCatalog::newOrphanedIdent(OperationContext* opCtx,
                                                     const std::string& ident,
                                                     bool isClustered) {
    NamespaceString nss;
    std::string ns;
    auto catalogEntry = _buildOrphanedCatalogEntryObjAndNs(ident, isClustered, &nss, &ns);
    auto res = addEntry(opCtx, ident, nss, catalogEntry, reserveCatalogId(opCtx));
    if (!res.isOK()) {
        return res.getStatus();
    }
    return {std::move(ns)};
}

RecordStore::Options MDBCatalog::_parseRecordStoreOptions(const NamespaceString& nss,
                                                          const BSONObj& obj) {
    RecordStore::Options recordStoreOptions;

    recordStoreOptions.isOplog = nss.isOplog();

    BSONElement mdElement = obj["md"];
    if (mdElement.isABSONObj()) {
        BSONElement optionsElement = mdElement.Obj()["options"];
        if (optionsElement.isABSONObj()) {
            BSONObj optionsObj = optionsElement.Obj();

            BSONElement clusteredElement = optionsObj["clusteredIndex"];
            bool isClustered = clusteredElement.isABSONObj() || clusteredElement.booleanSafe();
            recordStoreOptions.keyFormat = isClustered ? KeyFormat::String : KeyFormat::Long;
            recordStoreOptions.allowOverwrite = !isClustered;

            if (recordStoreOptions.isOplog) {
                BSONElement cappedSizeElement = optionsObj["size"];
                long long cappedSize = 0;
                if (cappedSizeElement.isNumber()) {
                    constexpr const long long kGB = 1024 * 1024 * 1024;
                    const long long kPB = 1024 * 1024 * kGB;
                    cappedSize = cappedSizeElement.safeNumberLong();
                    uassert(10455500,
                            "Invalid capped size in collection options",
                            cappedSize >= 0 && cappedSize < kPB);
                }
                recordStoreOptions.oplogMaxSize = cappedSize;
            }

            BSONElement timeseriesElement = optionsObj["timeseries"];
            if (!timeseriesElement.eoo()) {
                uassert(10455501,
                        "Timeseries options must be a document",
                        timeseriesElement.type() == mongo::BSONType::object);
                recordStoreOptions.customBlockCompressor = std::string{"zstd"_sd};
                recordStoreOptions.forceUpdateWithFullDocument = true;
            }

            recordStoreOptions.isCapped = optionsObj["capped"].trueValue();

            BSONElement storageEngineElement = optionsObj["storageEngine"];
            if (!storageEngineElement.eoo()) {
                uassert(10455502,
                        "StorageEngine must be a document",
                        storageEngineElement.type() == mongo::BSONType::object);
                for (auto&& elem : storageEngineElement.Obj()) {
                    uassert(10455503,
                            str::stream() << "StorageEngine." << elem.fieldName()
                                          << " must be an embedded document",
                            elem.type() == mongo::BSONType::object);
                }
                recordStoreOptions.storageEngineCollectionOptions =
                    storageEngineElement.Obj().getOwned();
            }
        }
    }

    return recordStoreOptions;
}

BSONObj MDBCatalog::_findRawEntry(SeekableRecordCursor& cursor, const RecordId& catalogId) const {
    LOGV2_DEBUG(22208, 3, "looking up metadata for: {catalogId}", "catalogId"_attr = catalogId);
    auto record = cursor.seekExact(catalogId);
    if (!record) {
        return BSONObj();
    }

    return record->data.releaseToBson();
}

StatusWith<MDBCatalog::EntryIdentifier> MDBCatalog::_importEntry(OperationContext* opCtx,
                                                                 const NamespaceString& nss,
                                                                 const BSONObj& catalogEntry) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_IX));

    auto ident = catalogEntry["ident"].String();
    StatusWith<RecordId> res = _rs->insertRecord(opCtx,
                                                 *shard_role_details::getRecoveryUnit(opCtx),
                                                 catalogEntry.objdata(),
                                                 catalogEntry.objsize(),
                                                 Timestamp());
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


std::vector<std::string> MDBCatalog::_getIndexIdents(const BSONObj& rawCatalogEntry) {
    std::vector<std::string> idents;

    if (rawCatalogEntry["idxIdent"].eoo()) {
        // No index entries for this catalog entry.
        return idents;
    }

    BSONObj idxIdent = rawCatalogEntry["idxIdent"].Obj();

    BSONObjIterator it(idxIdent);
    while (it.more()) {
        BSONElement elem = it.next();
        idents.push_back(elem.String());
    }
    return idents;
}

}  // namespace mongo
