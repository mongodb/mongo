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


#include "mongo/db/storage/durable_catalog_impl.h"

#include <fmt/format.h>
#include <memory>
#include <stdlib.h>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/bits.h"
#include "mongo/platform/random.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {
// This is a global resource, which protects accesses to the catalog metadata (instance-wide).
// It is never used with KVEngines that support doc-level locking so this should never conflict
// with anything else.

const char kNamespaceFieldName[] = "ns";
const char kNonRepairableFeaturesFieldName[] = "nonRepairable";
const char kRepairableFeaturesFieldName[] = "repairable";
const char kInternalIdentPrefix[] = "internal-";
const char kResumableIndexBuildIdentStem[] = "resumable-index-build-";

void appendPositionsOfBitsSet(uint64_t value, StringBuilder* sb) {
    invariant(sb);

    *sb << "[ ";
    bool firstIteration = true;
    while (value) {
        const int lowestSetBitPosition = countTrailingZeros64(value);
        if (!firstIteration) {
            *sb << ", ";
        }
        *sb << lowestSetBitPosition;
        value ^= (1ULL << lowestSetBitPosition);
        firstIteration = false;
    }
    *sb << " ]";
}

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

std::string escapeDbName(StringData dbname) {
    std::string escaped;
    escaped.reserve(dbname.size());
    for (unsigned char c : dbname) {
        StringData ce = escapeTable[c];
        escaped.append(ce.begin(), ce.end());
    }
    return escaped;
}

bool indexTypeSupportsPathLevelMultikeyTracking(StringData accessMethod) {
    return accessMethod == IndexNames::BTREE || accessMethod == IndexNames::GEO_2DSPHERE;
}
}  // namespace

class DurableCatalogImpl::AddIdentChange : public RecoveryUnit::Change {
public:
    AddIdentChange(DurableCatalogImpl* catalog, RecordId catalogId)
        : _catalog(catalog), _catalogId(std::move(catalogId)) {}

    virtual void commit(boost::optional<Timestamp>) {}
    virtual void rollback() {
        stdx::lock_guard<Latch> lk(_catalog->_catalogIdToEntryMapLock);
        _catalog->_catalogIdToEntryMap.erase(_catalogId);
    }

    DurableCatalogImpl* const _catalog;
    const RecordId _catalogId;
};

DurableCatalogImpl::DurableCatalogImpl(RecordStore* rs,
                                       bool directoryPerDb,
                                       bool directoryForIndexes,
                                       StorageEngineInterface* engine)
    : _rs(rs),
      _directoryPerDb(directoryPerDb),
      _directoryForIndexes(directoryForIndexes),
      _rand(_newRand()),
      _next(0),
      _engine(engine) {}

DurableCatalogImpl::~DurableCatalogImpl() {
    _rs = nullptr;
}

std::string DurableCatalogImpl::_newRand() {
    return str::stream() << SecureRandom().nextInt64();
}

bool DurableCatalogImpl::_hasEntryCollidingWithRand(WithLock) const {
    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    for (auto it = _catalogIdToEntryMap.begin(); it != _catalogIdToEntryMap.end(); ++it) {
        if (StringData(it->second.ident).endsWith(_rand))
            return true;
    }
    return false;
}

std::string DurableCatalogImpl::newInternalIdent() {
    return _newInternalIdent("");
}

std::string DurableCatalogImpl::newInternalResumableIndexBuildIdent() {
    return _newInternalIdent(kResumableIndexBuildIdentStem);
}

std::string DurableCatalogImpl::_newInternalIdent(StringData identStem) {
    stdx::lock_guard<Latch> lk(_randLock);
    StringBuilder buf;
    buf << kInternalIdentPrefix;
    buf << identStem;
    buf << _next++ << '-' << _rand;
    return buf.str();
}

std::string DurableCatalogImpl::getFilesystemPathForDb(const std::string& dbName) const {
    if (_directoryPerDb) {
        return storageGlobalParams.dbpath + '/' + escapeDbName(dbName);
    } else {
        return storageGlobalParams.dbpath;
    }
}

std::string DurableCatalogImpl::_newUniqueIdent(NamespaceString nss, const char* kind) {
    // If this changes to not put _rand at the end, _hasEntryCollidingWithRand will need fixing.
    stdx::lock_guard<Latch> lk(_randLock);
    StringBuilder buf;
    if (_directoryPerDb) {
        buf << escapeDbName(nss.db()) << '/';
    }
    buf << kind;
    buf << (_directoryForIndexes ? '/' : '-');
    buf << _next++ << '-' << _rand;
    return buf.str();
}

void DurableCatalogImpl::init(OperationContext* opCtx) {
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
        auto ns = obj["ns"].String();
        _catalogIdToEntryMap[record->id] = Entry(record->id, ident, NamespaceString(ns));
    }

    // In the unlikely event that we have used this _rand before generate a new one.
    stdx::lock_guard<Latch> lk(_randLock);
    while (_hasEntryCollidingWithRand(lk)) {
        _rand = _newRand();
    }
}

std::vector<DurableCatalog::Entry> DurableCatalogImpl::getAllCatalogEntries(
    OperationContext* opCtx) const {
    std::vector<DurableCatalog::Entry> ret;

    auto cursor = _rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();
        if (isFeatureDocument(obj)) {
            // Skip over the version document because it doesn't correspond to a collection.
            continue;
        }
        auto ident = obj["ident"].String();
        auto ns = obj["ns"].String();

        ret.emplace_back(record->id, ident, NamespaceString(ns));
    }

    return ret;
}

DurableCatalog::Entry DurableCatalogImpl::getEntry(const RecordId& catalogId) const {
    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    auto it = _catalogIdToEntryMap.find(catalogId);
    invariant(it != _catalogIdToEntryMap.end());
    return it->second;
}

StatusWith<DurableCatalog::Entry> DurableCatalogImpl::_addEntry(OperationContext* opCtx,
                                                                NamespaceString nss,
                                                                const CollectionOptions& options) {
    invariant(opCtx->lockState()->isDbLockedForMode(nss.dbName(), MODE_IX));

    auto ident = _newUniqueIdent(nss, "collection");

    BSONObj obj;
    {
        BSONObjBuilder b;
        b.append("ns", nss.ns());
        b.append("ident", ident);
        BSONCollectionCatalogEntry::MetaData md;
        md.ns = nss.ns();
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

    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    invariant(_catalogIdToEntryMap.find(res.getValue()) == _catalogIdToEntryMap.end());
    _catalogIdToEntryMap[res.getValue()] = {res.getValue(), ident, nss};
    opCtx->recoveryUnit()->registerChange(std::make_unique<AddIdentChange>(this, res.getValue()));

    LOGV2_DEBUG(22207,
                1,
                "stored meta data for {nss} @ {res_getValue}",
                logAttrs(nss),
                "res_getValue"_attr = res.getValue());
    return {{res.getValue(), ident, nss}};
}

StatusWith<DurableCatalog::Entry> DurableCatalogImpl::_importEntry(OperationContext* opCtx,
                                                                   NamespaceString nss,
                                                                   const BSONObj& metadata) {
    invariant(opCtx->lockState()->isDbLockedForMode(nss.dbName(), MODE_IX));

    auto ident = metadata["ident"].String();
    StatusWith<RecordId> res =
        _rs->insertRecord(opCtx, metadata.objdata(), metadata.objsize(), Timestamp());
    if (!res.isOK())
        return res.getStatus();

    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    invariant(_catalogIdToEntryMap.find(res.getValue()) == _catalogIdToEntryMap.end());
    _catalogIdToEntryMap[res.getValue()] = {res.getValue(), ident, nss};
    opCtx->recoveryUnit()->registerChange(std::make_unique<AddIdentChange>(this, res.getValue()));

    LOGV2_DEBUG(5095101, 1, "imported meta data", logAttrs(nss), "metadata"_attr = res.getValue());
    return {{res.getValue(), ident, nss}};
}

std::string DurableCatalogImpl::getIndexIdent(OperationContext* opCtx,
                                              const RecordId& catalogId,
                                              StringData idxName) const {
    BSONObj obj = _findEntry(opCtx, catalogId);
    BSONObj idxIdent = obj["idxIdent"].Obj();
    return idxIdent[idxName].String();
}

std::vector<std::string> DurableCatalogImpl::getIndexIdents(OperationContext* opCtx,
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

BSONObj DurableCatalogImpl::_findEntry(OperationContext* opCtx, const RecordId& catalogId) const {
    LOGV2_DEBUG(22208, 3, "looking up metadata for: {catalogId}", "catalogId"_attr = catalogId);
    RecordData data;
    if (!_rs->findRecord(opCtx, catalogId, &data)) {
        // since the in memory meta data isn't managed with mvcc
        // its possible for different transactions to see slightly
        // different things, which is ok via the locking above.
        return BSONObj();
    }

    return data.releaseToBson().getOwned();
}

std::shared_ptr<BSONCollectionCatalogEntry::MetaData> DurableCatalogImpl::getMetaData(
    OperationContext* opCtx, const RecordId& catalogId) const {
    BSONObj obj = _findEntry(opCtx, catalogId);
    LOGV2_DEBUG(22209, 3, " fetched CCE metadata: {obj}", "obj"_attr = obj);
    std::shared_ptr<BSONCollectionCatalogEntry::MetaData> md;
    const BSONElement mdElement = obj["md"];
    if (mdElement.isABSONObj()) {
        LOGV2_DEBUG(22210, 3, "returning metadata: {mdElement}", "mdElement"_attr = mdElement);
        md = std::make_shared<BSONCollectionCatalogEntry::MetaData>();
        md->parse(mdElement.Obj());
    }
    return md;
}

void DurableCatalogImpl::putMetaData(OperationContext* opCtx,
                                     const RecordId& catalogId,
                                     BSONCollectionCatalogEntry::MetaData& md) {
    NamespaceString nss(md.ns);
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
            newIdentMap.append(name, _newUniqueIdent(nss, "index"));
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

Status DurableCatalogImpl::_replaceEntry(OperationContext* opCtx,
                                         const RecordId& catalogId,
                                         const NamespaceString& toNss,
                                         BSONCollectionCatalogEntry::MetaData& md) {
    BSONObj old = _findEntry(opCtx, catalogId).getOwned();
    {
        BSONObjBuilder b;

        b.append("ns", toNss.ns());
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
    opCtx->recoveryUnit()->onRollback([this, catalogId, fromName]() {
        stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
        const auto it = _catalogIdToEntryMap.find(catalogId);
        invariant(it != _catalogIdToEntryMap.end());
        it->second.nss = fromName;
    });

    return Status::OK();
}

Status DurableCatalogImpl::_removeEntry(OperationContext* opCtx, const RecordId& catalogId) {
    stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
    const auto it = _catalogIdToEntryMap.find(catalogId);
    if (it == _catalogIdToEntryMap.end()) {
        return Status(ErrorCodes::NamespaceNotFound, "collection not found");
    }

    opCtx->recoveryUnit()->onRollback([this, catalogId, entry = it->second]() {
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

std::vector<std::string> DurableCatalogImpl::getAllIdents(OperationContext* opCtx) const {
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

bool DurableCatalogImpl::isUserDataIdent(StringData ident) const {
    // Indexes and collections are candidates for dropping when the storage engine's metadata does
    // not align with the catalog metadata.
    return ident.find("index-") != std::string::npos || ident.find("index/") != std::string::npos ||
        ident.find("collection-") != std::string::npos ||
        ident.find("collection/") != std::string::npos;
}

bool DurableCatalogImpl::isInternalIdent(StringData ident) const {
    return ident.find(kInternalIdentPrefix) != std::string::npos;
}

bool DurableCatalogImpl::isResumableIndexBuildIdent(StringData ident) const {
    invariant(isInternalIdent(ident), ident.toString());
    return ident.find(kResumableIndexBuildIdentStem) != std::string::npos;
}

bool DurableCatalogImpl::isCollectionIdent(StringData ident) const {
    // Internal idents prefixed "internal-" should not be considered collections, because
    // they are not eligible for orphan recovery through repair.
    return ident.find("collection-") != std::string::npos ||
        ident.find("collection/") != std::string::npos;
}

StatusWith<std::string> DurableCatalogImpl::newOrphanedIdent(OperationContext* opCtx,
                                                             std::string ident) {
    // The collection will be named local.orphan.xxxxx.
    std::string identNs = ident;
    std::replace(identNs.begin(), identNs.end(), '-', '_');
    // TODO SERVER-62491 Use system tenantId.
    NamespaceString nss(NamespaceString(NamespaceString::kOrphanCollectionDb,
                                        NamespaceString::kOrphanCollectionPrefix + identNs));

    // Generate a new UUID for the orphaned collection.
    CollectionOptions optionsWithUUID;
    optionsWithUUID.uuid.emplace(UUID::gen());
    BSONObj obj;
    {
        BSONObjBuilder b;
        b.append("ns", nss.ns());
        b.append("ident", ident);
        BSONCollectionCatalogEntry::MetaData md;
        md.ns = nss.ns();
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
    _catalogIdToEntryMap[res.getValue()] = Entry(res.getValue(), ident, nss);
    opCtx->recoveryUnit()->registerChange(std::make_unique<AddIdentChange>(this, res.getValue()));

    LOGV2_DEBUG(22213,
                1,
                "stored meta data for orphaned collection {namespace} @ {res_getValue}",
                logAttrs(nss),
                "res_getValue"_attr = res.getValue());
    return {nss.ns()};
}

StatusWith<std::pair<RecordId, std::unique_ptr<RecordStore>>> DurableCatalogImpl::createCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionOptions& options,
    bool allocateDefaultSpace) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX));
    invariant(nss.coll().size() > 0);

    if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss)) {
        throwWriteConflictException();
    }

    StatusWith<Entry> swEntry = _addEntry(opCtx, nss, options);
    if (!swEntry.isOK())
        return swEntry.getStatus();
    Entry& entry = swEntry.getValue();

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

    auto ru = opCtx->recoveryUnit();
    opCtx->recoveryUnit()->onRollback([ru, catalog = this, ident = entry.ident]() {
        // Intentionally ignoring failure
        catalog->_engine->getEngine()->dropIdent(ru, ident).ignore();
    });

    auto rs = _engine->getEngine()->getRecordStore(opCtx, nss, entry.ident, options);
    invariant(rs);

    return std::pair<RecordId, std::unique_ptr<RecordStore>>(entry.catalogId, std::move(rs));
}

Status DurableCatalogImpl::createIndex(OperationContext* opCtx,
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
        opCtx->recoveryUnit()->onRollback([this, ident, recoveryUnit = opCtx->recoveryUnit()]() {
            // Intentionally ignoring failure.
            auto kvEngine = _engine->getEngine();
            kvEngine->dropIdent(recoveryUnit, ident).ignore();
        });
    }
    return status;
}

StatusWith<DurableCatalog::ImportResult> DurableCatalogImpl::importCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& metadata,
    const BSONObj& storageMetadata,
    const ImportOptions& importOptions) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));
    invariant(nss.coll().size() > 0);

    uassert(ErrorCodes::NamespaceExists,
            str::stream() << "Collection already exists. NS: " << nss.ns(),
            !CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss));

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
        while (_hasEntryCollidingWithRand(lk) || identsToImportConflict(lk)) {
            _rand = _newRand();
        }
    }

    StatusWith<Entry> swEntry = _importEntry(opCtx, nss, catalogEntry);
    if (!swEntry.isOK())
        return swEntry.getStatus();
    Entry& entry = swEntry.getValue();

    opCtx->recoveryUnit()->onRollback(
        [opCtx, catalog = this, ident = entry.ident, indexIdents = indexIdents]() {
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

    return DurableCatalog::ImportResult(entry.catalogId, std::move(rs), md.options.uuid.get());
}

Status DurableCatalogImpl::renameCollection(OperationContext* opCtx,
                                            const RecordId& catalogId,
                                            const NamespaceString& toNss,
                                            BSONCollectionCatalogEntry::MetaData& md) {
    return _replaceEntry(opCtx, catalogId, toNss, md);
}

Status DurableCatalogImpl::dropCollection(OperationContext* opCtx, const RecordId& catalogId) {
    Entry entry;
    {
        stdx::lock_guard<Latch> lk(_catalogIdToEntryMapLock);
        entry = _catalogIdToEntryMap[catalogId];
    }

    invariant(opCtx->lockState()->isCollectionLockedForMode(entry.nss, MODE_X));
    invariant(getTotalIndexCount(opCtx, catalogId) == 0);

    // Remove metadata from mdb_catalog
    Status status = _removeEntry(opCtx, catalogId);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

Status DurableCatalogImpl::dropAndRecreateIndexIdentForResume(OperationContext* opCtx,
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

bool DurableCatalogImpl::isIndexMultikey(OperationContext* opCtx,
                                         const RecordId& catalogId,
                                         StringData indexName,
                                         MultikeyPaths* multikeyPaths) const {
    auto md = getMetaData(opCtx, catalogId);

    int offset = md->findIndexOffset(indexName);
    invariant(offset >= 0,
              str::stream() << "cannot get multikey for index " << indexName << " @ " << catalogId
                            << " : " << md->toBSON());

    if (multikeyPaths && !md->indexes[offset].multikeyPaths.empty()) {
        *multikeyPaths = md->indexes[offset].multikeyPaths;
    }

    return md->indexes[offset].multikey;
}

int DurableCatalogImpl::getTotalIndexCount(OperationContext* opCtx,
                                           const RecordId& catalogId) const {
    auto md = getMetaData(opCtx, catalogId);
    if (!md)
        return 0;

    return md->getTotalIndexCount();
}

bool DurableCatalogImpl::isIndexPresent(OperationContext* opCtx,
                                        const RecordId& catalogId,
                                        StringData indexName) const {
    auto md = getMetaData(opCtx, catalogId);
    if (!md)
        return false;

    int offset = md->findIndexOffset(indexName);
    return offset >= 0;
}

bool DurableCatalogImpl::isIndexReady(OperationContext* opCtx,
                                      const RecordId& catalogId,
                                      StringData indexName) const {
    auto md = getMetaData(opCtx, catalogId);
    if (!md)
        return false;

    int offset = md->findIndexOffset(indexName);
    invariant(offset >= 0,
              str::stream() << "cannot get ready status for index " << indexName << " @ "
                            << catalogId << " : " << md->toBSON());
    return md->indexes[offset].ready;
}

void DurableCatalogImpl::setRand_forTest(const std::string& rand) {
    stdx::lock_guard<Latch> lk(_randLock);
    _rand = rand;
}

std::string DurableCatalogImpl::getRand_forTest() const {
    stdx::lock_guard<Latch> lk(_randLock);
    return _rand;
}

}  // namespace mongo
