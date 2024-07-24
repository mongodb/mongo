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
#include <array>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <memory>
#include <set>
#include <type_traits>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine_interface.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {
// Does not escape letters, digits, '.', or '_'.
// Otherwise escapes to a '.' followed by a zero-filled 2- or 3-digit decimal number.
// Note that this escape table does not produce a 1:1 mapping to and from dbname, and
// collisions are possible.
// For example:
//     "db.123", "db\0143", and "db\073" all escape to "db.123".
//       {'d','b','1','2','3'} => "d" + "b" + "." + "1" + "2" + "3" => "db.123"
//       {'d','b','\x0c','3'}  => "d" + "b" + ".12" + "3"           => "db.123"
//       {'d','b','\x3b'}      => "d" + "b" + ".123"                => "db.123"
constexpr std::array<StringData, 256> escapeTable = {
    ".00"_sd,  ".01"_sd,  ".02"_sd,  ".03"_sd,  ".04"_sd,  ".05"_sd,  ".06"_sd,  ".07"_sd,
    ".08"_sd,  ".09"_sd,  ".10"_sd,  ".11"_sd,  ".12"_sd,  ".13"_sd,  ".14"_sd,  ".15"_sd,
    ".16"_sd,  ".17"_sd,  ".18"_sd,  ".19"_sd,  ".20"_sd,  ".21"_sd,  ".22"_sd,  ".23"_sd,
    ".24"_sd,  ".25"_sd,  ".26"_sd,  ".27"_sd,  ".28"_sd,  ".29"_sd,  ".30"_sd,  ".31"_sd,
    ".32"_sd,  ".33"_sd,  ".34"_sd,  ".35"_sd,  ".36"_sd,  ".37"_sd,  ".38"_sd,  ".39"_sd,
    ".40"_sd,  ".41"_sd,  ".42"_sd,  ".43"_sd,  ".44"_sd,  ".45"_sd,  "."_sd,    ".47"_sd,
    "0"_sd,    "1"_sd,    "2"_sd,    "3"_sd,    "4"_sd,    "5"_sd,    "6"_sd,    "7"_sd,
    "8"_sd,    "9"_sd,    ".58"_sd,  ".59"_sd,  ".60"_sd,  ".61"_sd,  ".62"_sd,  ".63"_sd,
    ".64"_sd,  "A"_sd,    "B"_sd,    "C"_sd,    "D"_sd,    "E"_sd,    "F"_sd,    "G"_sd,
    "H"_sd,    "I"_sd,    "J"_sd,    "K"_sd,    "L"_sd,    "M"_sd,    "N"_sd,    "O"_sd,
    "P"_sd,    "Q"_sd,    "R"_sd,    "S"_sd,    "T"_sd,    "U"_sd,    "V"_sd,    "W"_sd,
    "X"_sd,    "Y"_sd,    "Z"_sd,    ".91"_sd,  ".92"_sd,  ".93"_sd,  ".94"_sd,  "_"_sd,
    ".96"_sd,  "a"_sd,    "b"_sd,    "c"_sd,    "d"_sd,    "e"_sd,    "f"_sd,    "g"_sd,
    "h"_sd,    "i"_sd,    "j"_sd,    "k"_sd,    "l"_sd,    "m"_sd,    "n"_sd,    "o"_sd,
    "p"_sd,    "q"_sd,    "r"_sd,    "s"_sd,    "t"_sd,    "u"_sd,    "v"_sd,    "w"_sd,
    "x"_sd,    "y"_sd,    "z"_sd,    ".123"_sd, ".124"_sd, ".125"_sd, ".126"_sd, ".127"_sd,
    ".128"_sd, ".129"_sd, ".130"_sd, ".131"_sd, ".132"_sd, ".133"_sd, ".134"_sd, ".135"_sd,
    ".136"_sd, ".137"_sd, ".138"_sd, ".139"_sd, ".140"_sd, ".141"_sd, ".142"_sd, ".143"_sd,
    ".144"_sd, ".145"_sd, ".146"_sd, ".147"_sd, ".148"_sd, ".149"_sd, ".150"_sd, ".151"_sd,
    ".152"_sd, ".153"_sd, ".154"_sd, ".155"_sd, ".156"_sd, ".157"_sd, ".158"_sd, ".159"_sd,
    ".160"_sd, ".161"_sd, ".162"_sd, ".163"_sd, ".164"_sd, ".165"_sd, ".166"_sd, ".167"_sd,
    ".168"_sd, ".169"_sd, ".170"_sd, ".171"_sd, ".172"_sd, ".173"_sd, ".174"_sd, ".175"_sd,
    ".176"_sd, ".177"_sd, ".178"_sd, ".179"_sd, ".180"_sd, ".181"_sd, ".182"_sd, ".183"_sd,
    ".184"_sd, ".185"_sd, ".186"_sd, ".187"_sd, ".188"_sd, ".189"_sd, ".190"_sd, ".191"_sd,
    ".192"_sd, ".193"_sd, ".194"_sd, ".195"_sd, ".196"_sd, ".197"_sd, ".198"_sd, ".199"_sd,
    ".200"_sd, ".201"_sd, ".202"_sd, ".203"_sd, ".204"_sd, ".205"_sd, ".206"_sd, ".207"_sd,
    ".208"_sd, ".209"_sd, ".210"_sd, ".211"_sd, ".212"_sd, ".213"_sd, ".214"_sd, ".215"_sd,
    ".216"_sd, ".217"_sd, ".218"_sd, ".219"_sd, ".220"_sd, ".221"_sd, ".222"_sd, ".223"_sd,
    ".224"_sd, ".225"_sd, ".226"_sd, ".227"_sd, ".228"_sd, ".229"_sd, ".230"_sd, ".231"_sd,
    ".232"_sd, ".233"_sd, ".234"_sd, ".235"_sd, ".236"_sd, ".237"_sd, ".238"_sd, ".239"_sd,
    ".240"_sd, ".241"_sd, ".242"_sd, ".243"_sd, ".244"_sd, ".245"_sd, ".246"_sd, ".247"_sd,
    ".248"_sd, ".249"_sd, ".250"_sd, ".251"_sd, ".252"_sd, ".253"_sd, ".254"_sd, ".255"_sd};

std::string escapeDbName(const DatabaseName& dbName) {
    std::string escaped;
    const auto db = DatabaseNameUtil::serialize(dbName, SerializationContext::stateCatalog());
    escaped.reserve(db.size());
    for (unsigned char c : db) {
        StringData ce = escapeTable[c];
        escaped.append(ce.begin(), ce.end());
    }
    return escaped;
}

}  // namespace

class DurableCatalog::AddIdentChange : public RecoveryUnit::Change {
public:
    AddIdentChange(DurableCatalog* catalog, RecordId catalogId)
        : _catalog(catalog), _catalogId(std::move(catalogId)) {}

    void commit(OperationContext* opCtx, boost::optional<Timestamp>) override {}
    void rollback(OperationContext* opCtx) override {
        stdx::lock_guard<Latch> lk(_catalog->_catalogIdToEntryMapLock);
        _catalog->_catalogIdToEntryMap.erase(_catalogId);
    }

private:
    DurableCatalog* const _catalog;
    const RecordId _catalogId;
};

DurableCatalog::DurableCatalog(RecordStore* rs,
                               bool directoryPerDb,
                               bool directoryForIndexes,
                               StorageEngineInterface* engine)
    : _rs(rs),
      _directoryPerDb(directoryPerDb),
      _directoryForIndexes(directoryForIndexes),
      _rand(_newRand()),
      _next(0),
      _engine(engine) {}

bool DurableCatalog::_hasEntryCollidingWithRand(WithLock) const {
    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    for (auto it = _catalogIdToEntryMap.begin(); it != _catalogIdToEntryMap.end(); ++it) {
        if (StringData(it->second.ident).endsWith(_rand))
            return true;
    }
    return false;
}

std::string DurableCatalog::_newInternalIdent(StringData identStem) {
    stdx::lock_guard<Latch> lk(_randLock);
    StringBuilder buf;
    buf << _kInternalIdentPrefix;
    buf << identStem;
    buf << _next++ << '-' << _rand;
    return buf.str();
}

std::string DurableCatalog::_newRand() {
    return str::stream() << SecureRandom().nextUInt64();
}

std::string DurableCatalog::getFilesystemPathForDb(const DatabaseName& dbName) const {
    if (_directoryPerDb) {
        return storageGlobalParams.dbpath + '/' + escapeDbName(dbName);
    } else {
        return storageGlobalParams.dbpath;
    }
}

std::string DurableCatalog::generateUniqueIdent(NamespaceString nss, const char* kind) {
    // If this changes to not put _rand at the end, _hasEntryCollidingWithRand will need fixing.
    stdx::lock_guard<Latch> lk(_randLock);
    StringBuilder buf;
    if (_directoryPerDb) {
        buf << escapeDbName(nss.dbName()) << '/';
    }
    buf << kind;
    buf << (_directoryForIndexes ? '/' : '-');
    buf << _next++ << '-' << _rand;
    return buf.str();
}

void DurableCatalog::init(OperationContext* opCtx) {
    // No locking needed since called single threaded.
    auto cursor = _rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();

        // For backwards compatibility where older version have a written feature document
        if (isFeatureDocument(obj)) {
            continue;
        }

        // No rollback since this is just loading already committed data.
        auto ident = obj["ident"].String();
        auto nss = NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(
            obj["ns"].String());
        _catalogIdToEntryMap[record->id] = EntryIdentifier(record->id, ident, nss);
    }

    // In the unlikely event that we have used this _rand before generate a new one.
    stdx::lock_guard<Latch> lk(_randLock);
    while (_hasEntryCollidingWithRand(lk)) {
        _rand = _newRand();
    }
}

std::vector<DurableCatalog::EntryIdentifier> DurableCatalog::getAllCatalogEntries(
    OperationContext* opCtx) const {
    std::vector<DurableCatalog::EntryIdentifier> ret;

    auto cursor = _rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();
        if (isFeatureDocument(obj)) {
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

        if (isFeatureDocument(obj)) {
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
    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    auto it = _catalogIdToEntryMap.find(catalogId);
    invariant(it != _catalogIdToEntryMap.end());
    return it->second;
}

NamespaceString DurableCatalog::getNSSFromCatalog(OperationContext* opCtx,
                                                  const RecordId& catalogId) const {
    {
        stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
        auto it = _catalogIdToEntryMap.find(catalogId);
        if (it != _catalogIdToEntryMap.end()) {
            return it->second.nss;
        }
    }

    // Re-read the catalog at the provided timestamp in case the collection was dropped.
    BSONObj obj = _findEntry(opCtx, catalogId);
    if (!obj.isEmpty()) {
        return NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(
            obj["ns"].String());
    }

    tassert(9117800, str::stream() << "Namespace not found for " << catalogId, false);
}

StatusWith<DurableCatalog::EntryIdentifier> DurableCatalog::_addEntry(
    OperationContext* opCtx, NamespaceString nss, const CollectionOptions& options) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_IX));

    auto ident = generateUniqueIdent(nss, "collection");

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
            if (feature_flags::gTSBucketingParametersUnchanged.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                md.timeseriesBucketingParametersHaveChanged = false;
            }
        }
        b.append("md", md.toBSON());
        obj = b.obj();
    }
    StatusWith<RecordId> res = _rs->insertRecord(opCtx, obj.objdata(), obj.objsize(), Timestamp());
    if (!res.isOK())
        return res.getStatus();

    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
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

    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
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
    BSONObj obj = _findEntry(opCtx, catalogId);
    BSONObj idxIdent = obj["idxIdent"].Obj();
    return idxIdent[idxName].String();
}

std::vector<std::string> DurableCatalog::getIndexIdents(OperationContext* opCtx,
                                                        const RecordId& catalogId) const {
    std::vector<std::string> idents;

    BSONObj obj = _findEntry(opCtx, catalogId);
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

BSONObj DurableCatalog::_findEntry(OperationContext* opCtx, const RecordId& catalogId) const {
    LOGV2_DEBUG(22208, 3, "looking up metadata for: {catalogId}", "catalogId"_attr = catalogId);
    RecordData data;
    if (!_rs->findRecord(opCtx, catalogId, &data)) {
        return BSONObj();
    }

    return data.releaseToBson().getOwned();
}

boost::optional<DurableCatalogEntry> DurableCatalog::getParsedCatalogEntry(
    OperationContext* opCtx, const RecordId& catalogId) const {
    BSONObj obj = _findEntry(opCtx, catalogId);
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
    NamespaceString nss(md.nss);
    BSONObj obj = _findEntry(opCtx, catalogId);

    {
        // rebuilt doc
        BSONObjBuilder b;
        b.append("md", md.toBSON());

        BSONObjBuilder newIdentMap;
        BSONObj oldIdentMap;
        if (obj["idxIdent"].isABSONObj())
            oldIdentMap = obj["idxIdent"].Obj();

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
            // missing, create new
            newIdentMap.append(name, generateUniqueIdent(nss, "index"));
        }
        b.append("idxIdent", newIdentMap.obj());

        // add whatever is left
        b.appendElementsUnique(obj);
        obj = b.obj();
    }

    LOGV2_DEBUG(22211, 3, "recording new metadata: {obj}", "obj"_attr = obj);
    Status status = _rs->updateRecord(opCtx, catalogId, obj.objdata(), obj.objsize());
    fassert(28521, status);
}

Status DurableCatalog::_removeEntry(OperationContext* opCtx, const RecordId& catalogId) {
    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    const auto it = _catalogIdToEntryMap.find(catalogId);
    if (it == _catalogIdToEntryMap.end()) {
        return Status(ErrorCodes::NamespaceNotFound, "collection not found");
    }

    shard_role_details::getRecoveryUnit(opCtx)->onRollback(
        [this, catalogId, entry = it->second](OperationContext*) {
            stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
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
        if (isFeatureDocument(obj)) {
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

    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
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
    const CollectionOptions& options,
    bool allocateDefaultSpace) {
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IX));
    invariant(nss.coll().size() > 0);

    StatusWith<EntryIdentifier> swEntry = _addEntry(opCtx, nss, options);
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
    Status status =
        _engine->getEngine()->createRecordStore(opCtx, nss, entry.ident, options, keyFormat);
    if (!status.isOK())
        return status;

    auto ru = shard_role_details::getRecoveryUnit(opCtx);
    shard_role_details::getRecoveryUnit(opCtx)->onRollback(
        [ru, catalog = this, ident = entry.ident](OperationContext*) {
            // Intentionally ignoring failure
            catalog->_engine->getEngine()->dropIdent(ru, ident).ignore();
        });

    auto rs = _engine->getEngine()->getRecordStore(opCtx, nss, entry.ident, options);
    invariant(rs);

    return std::pair<RecordId, std::unique_ptr<RecordStore>>(entry.catalogId, std::move(rs));
}

Status DurableCatalog::createIndex(OperationContext* opCtx,
                                   const RecordId& catalogId,
                                   const NamespaceString& nss,
                                   const CollectionOptions& collOptions,
                                   const IndexDescriptor* spec) {
    std::string ident = getIndexIdent(opCtx, catalogId, spec->indexName());

    auto kvEngine = _engine->getEngine();
    Status status = spec->getIndexType() == INDEX_COLUMN
        ? kvEngine->createColumnStore(opCtx, nss, collOptions, ident, spec)
        : kvEngine->createSortedDataInterface(opCtx, nss, collOptions, ident, spec);
    if (status.isOK()) {
        shard_role_details::getRecoveryUnit(opCtx)->onRollback(
            [this, ident, recoveryUnit = shard_role_details::getRecoveryUnit(opCtx)](
                OperationContext*) {
                // Intentionally ignoring failure.
                auto kvEngine = _engine->getEngine();
                kvEngine->dropIdent(recoveryUnit, ident).ignore();
            });
    }
    return status;
}

StatusWith<DurableCatalog::ImportResult> DurableCatalog::importCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& metadata,
    const BSONObj& storageMetadata,
    const ImportOptions& importOptions) {
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
        if (importOptions.importCollectionUUIDOption ==
            ImportOptions::ImportCollectionUUIDOption::kGenerateNew) {
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

    // Before importing the idents belonging to the collection and indexes, change '_rand' if there
    // will be a conflict.
    std::set<std::string> indexIdents;
    {
        const std::string collectionIdent = catalogEntry["ident"].String();

        if (!catalogEntry["idxIdent"].eoo()) {
            for (const auto& indexIdent : catalogEntry["idxIdent"].Obj()) {
                indexIdents.insert(indexIdent.String());
            }
        }

        auto identsToImportConflict = [&](WithLock) -> bool {
            if (StringData(collectionIdent).endsWith(_rand)) {
                return true;
            }

            for (const std::string& ident : indexIdents) {
                if (StringData(ident).endsWith(_rand)) {
                    return true;
                }
            }
            return false;
        };

        stdx::lock_guard<Latch> lk(_randLock);
        while (!importOptions.skipIdentCollisionCheck &&
               (_hasEntryCollidingWithRand(lk) || identsToImportConflict(lk))) {
            _rand = _newRand();
        }
    }

    StatusWith<EntryIdentifier> swEntry = _importEntry(opCtx, nss, catalogEntry);
    if (!swEntry.isOK())
        return swEntry.getStatus();
    EntryIdentifier& entry = swEntry.getValue();

    shard_role_details::getRecoveryUnit(opCtx)->onRollback(
        [catalog = this, ident = entry.ident, indexIdents = indexIdents](OperationContext* opCtx) {
            catalog->_engine->getEngine()->dropIdentForImport(opCtx, ident);
            for (const auto& indexIdent : indexIdents) {
                catalog->_engine->getEngine()->dropIdentForImport(opCtx, indexIdent);
            }
        });

    auto kvEngine = _engine->getEngine();
    Status status = kvEngine->importRecordStore(opCtx, entry.ident, storageMetadata, importOptions);
    if (!status.isOK())
        return status;

    for (const std::string& indexIdent : indexIdents) {
        status =
            kvEngine->importSortedDataInterface(opCtx, indexIdent, storageMetadata, importOptions);
        if (!status.isOK()) {
            return status;
        }
    }

    auto rs = _engine->getEngine()->getRecordStore(opCtx, nss, entry.ident, md.options);
    invariant(rs);

    return DurableCatalog::ImportResult(entry.catalogId, std::move(rs), md.options.uuid.value());
}

Status DurableCatalog::renameCollection(OperationContext* opCtx,
                                        const RecordId& catalogId,
                                        const NamespaceString& toNss,
                                        BSONCollectionCatalogEntry::MetaData& md) {
    BSONObj old = _findEntry(opCtx, catalogId).getOwned();
    {
        BSONObjBuilder b;

        b.append("ns", NamespaceStringUtil::serializeForCatalog(toNss));
        b.append("md", md.toBSON());

        b.appendElementsUnique(old);

        BSONObj obj = b.obj();
        Status status = _rs->updateRecord(opCtx, catalogId, obj.objdata(), obj.objsize());
        fassert(28522, status);
    }

    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    const auto it = _catalogIdToEntryMap.find(catalogId);
    invariant(it != _catalogIdToEntryMap.end());

    NamespaceString fromName = it->second.nss;
    it->second.nss = toNss;
    shard_role_details::getRecoveryUnit(opCtx)->onRollback(
        [this, catalogId, fromName](OperationContext*) {
            stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
            const auto it = _catalogIdToEntryMap.find(catalogId);
            invariant(it != _catalogIdToEntryMap.end());
            it->second.nss = fromName;
        });

    return Status::OK();
}

Status DurableCatalog::dropCollection(OperationContext* opCtx, const RecordId& catalogId) {
    EntryIdentifier entry;
    {
        stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
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
                                                          const IndexDescriptor* spec,
                                                          StringData ident) {
    auto status = _engine->getEngine()->dropSortedDataInterface(opCtx, ident);
    if (!status.isOK())
        return status;

    status = _engine->getEngine()->createSortedDataInterface(opCtx, nss, collOptions, ident, spec);

    return status;
}

bool DurableCatalog::isIndexMultikey(OperationContext* opCtx,
                                     const RecordId& catalogId,
                                     StringData indexName,
                                     MultikeyPaths* multikeyPaths) const {
    auto catalogEntry = getParsedCatalogEntry(opCtx, catalogId);
    auto md = catalogEntry->metadata;

    int offset = md->findIndexOffset(indexName);
    invariant(offset >= 0,
              str::stream() << "cannot get multikey for index " << indexName << " @ " << catalogId
                            << " : " << md->toBSON());

    if (multikeyPaths && !md->indexes[offset].multikeyPaths.empty()) {
        *multikeyPaths = md->indexes[offset].multikeyPaths;
    }

    return md->indexes[offset].multikey;
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
    if (isFeatureDocument(obj)) {
        return boost::none;
    }

    BSONElement idxIdent = obj["idxIdent"];
    return DurableCatalogEntry{catalogId,
                               obj["ident"].String(),
                               idxIdent.eoo() ? BSONObj() : idxIdent.Obj().getOwned(),
                               _parseMetaData(obj["md"])};
}

}  // namespace mongo
