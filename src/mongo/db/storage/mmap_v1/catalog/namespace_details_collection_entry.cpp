// namespace_details_collection_entry.h

/**
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/mmap_v1/catalog/namespace_details_collection_entry.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_details.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_details_rsv1_metadata.h"
#include "mongo/db/storage/mmap_v1/data_file.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_database_catalog_entry.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/log.h"
#include "mongo/util/startup_test.h"

namespace mongo {

using std::string;

NamespaceDetailsCollectionCatalogEntry::NamespaceDetailsCollectionCatalogEntry(
    StringData ns,
    NamespaceDetails* details,
    RecordStore* namespacesRecordStore,
    RecordId namespacesRecordId,
    RecordStore* indexRecordStore,
    MMAPV1DatabaseCatalogEntry* db)
    : CollectionCatalogEntry(ns),
      _details(details),
      _namespacesRecordStore(namespacesRecordStore),
      _indexRecordStore(indexRecordStore),
      _db(db) {
    setNamespacesRecordId(nullptr, namespacesRecordId);
}

CollectionOptions NamespaceDetailsCollectionCatalogEntry::getCollectionOptions(
    OperationContext* opCtx) const {
    CollectionOptions options = _db->getCollectionOptions(opCtx, _namespacesRecordId);

    if (options.flagsSet) {
        if (options.flags != _details->userFlags) {
            warning() << "system.namespaces and NamespaceDetails disagree about userFlags."
                      << " system.namespaces: " << options.flags
                      << " NamespaceDetails: " << _details->userFlags;
            dassert(options.flags == _details->userFlags);
        }
    }

    // Fill in the actual flags from the NamespaceDetails.
    // Leaving flagsSet alone since it indicates whether the user actively set the flags.
    options.flags = _details->userFlags;

    return options;
}

int NamespaceDetailsCollectionCatalogEntry::getTotalIndexCount(OperationContext* opCtx) const {
    return _details->nIndexes + _details->indexBuildsInProgress;
}

int NamespaceDetailsCollectionCatalogEntry::getCompletedIndexCount(OperationContext* opCtx) const {
    return _details->nIndexes;
}

int NamespaceDetailsCollectionCatalogEntry::getMaxAllowedIndexes() const {
    return NamespaceDetails::NIndexesMax;
}

void NamespaceDetailsCollectionCatalogEntry::getAllIndexes(OperationContext* opCtx,
                                                           std::vector<std::string>* names) const {
    NamespaceDetails::IndexIterator i = _details->ii(true);
    while (i.more()) {
        const IndexDetails& id = i.next();
        const BSONObj obj = _indexRecordStore->dataFor(opCtx, id.info.toRecordId()).toBson();
        names->push_back(obj.getStringField("name"));
    }
}

bool NamespaceDetailsCollectionCatalogEntry::isIndexMultikey(OperationContext* opCtx,
                                                             StringData idxName,
                                                             MultikeyPaths* multikeyPaths) const {
    // TODO SERVER-22727: Populate 'multikeyPaths' with path components that cause 'idxName' to be
    // multikey.
    int idxNo = _findIndexNumber(opCtx, idxName);
    invariant(idxNo >= 0);
    return isIndexMultikey(idxNo);
}

bool NamespaceDetailsCollectionCatalogEntry::isIndexMultikey(int idxNo) const {
    return (_details->multiKeyIndexBits & (((unsigned long long)1) << idxNo)) != 0;
}

bool NamespaceDetailsCollectionCatalogEntry::setIndexIsMultikey(
    OperationContext* opCtx, StringData indexName, const MultikeyPaths& multikeyPaths) {
    // TODO SERVER-22727: Store new path components from 'multikeyPaths' that cause 'indexName' to
    // be multikey.
    int idxNo = _findIndexNumber(opCtx, indexName);
    invariant(idxNo >= 0);
    const bool multikey = true;
    return setIndexIsMultikey(opCtx, idxNo, multikey);
}

bool NamespaceDetailsCollectionCatalogEntry::setIndexIsMultikey(OperationContext* opCtx,
                                                                int idxNo,
                                                                bool multikey) {
    unsigned long long mask = 1ULL << idxNo;

    if (multikey) {
        // Shortcut if the bit is already set correctly
        if (_details->multiKeyIndexBits & mask) {
            return false;
        }

        *opCtx->recoveryUnit()->writing(&_details->multiKeyIndexBits) |= mask;
    } else {
        // Shortcut if the bit is already set correctly
        if (!(_details->multiKeyIndexBits & mask)) {
            return false;
        }

        // Invert mask: all 1's except a 0 at the ith bit
        mask = ~mask;
        *opCtx->recoveryUnit()->writing(&_details->multiKeyIndexBits) &= mask;
    }

    return true;
}

RecordId NamespaceDetailsCollectionCatalogEntry::getIndexHead(OperationContext* opCtx,
                                                              StringData idxName) const {
    int idxNo = _findIndexNumber(opCtx, idxName);
    invariant(idxNo >= 0);
    return _details->idx(idxNo).head.toRecordId();
}

BSONObj NamespaceDetailsCollectionCatalogEntry::getIndexSpec(OperationContext* opCtx,
                                                             StringData idxName) const {
    int idxNo = _findIndexNumber(opCtx, idxName);
    invariant(idxNo >= 0);
    const IndexDetails& id = _details->idx(idxNo);
    return _indexRecordStore->dataFor(opCtx, id.info.toRecordId()).toBson();
}

void NamespaceDetailsCollectionCatalogEntry::setIndexHead(OperationContext* opCtx,
                                                          StringData idxName,
                                                          const RecordId& newHead) {
    int idxNo = _findIndexNumber(opCtx, idxName);
    invariant(idxNo >= 0);
    *opCtx->recoveryUnit()->writing(&_details->idx(idxNo).head) = DiskLoc::fromRecordId(newHead);
}

bool NamespaceDetailsCollectionCatalogEntry::isIndexReady(OperationContext* opCtx,
                                                          StringData idxName) const {
    int idxNo = _findIndexNumber(opCtx, idxName);
    invariant(idxNo >= 0);
    return idxNo < getCompletedIndexCount(opCtx);
}

KVPrefix NamespaceDetailsCollectionCatalogEntry::getIndexPrefix(OperationContext* opCtx,
                                                                StringData indexName) const {
    return KVPrefix::kNotPrefixed;
}

int NamespaceDetailsCollectionCatalogEntry::_findIndexNumber(OperationContext* opCtx,
                                                             StringData idxName) const {
    NamespaceDetails::IndexIterator i = _details->ii(true);
    while (i.more()) {
        const IndexDetails& id = i.next();
        int idxNo = i.pos() - 1;
        const BSONObj obj = _indexRecordStore->dataFor(opCtx, id.info.toRecordId()).toBson();
        if (idxName == obj.getStringField("name"))
            return idxNo;
    }
    return -1;
}

/* remove bit from a bit array - actually remove its slot, not a clear
   note: this function does not work with x == 63 -- that is ok
         but keep in mind in the future if max indexes were extended to
         exactly 64 it would be a problem
*/
unsigned long long removeAndSlideBit(unsigned long long b, int x) {
    unsigned long long tmp = b;
    return (tmp & ((((unsigned long long)1) << x) - 1)) | ((tmp >> (x + 1)) << x);
}

class IndexUpdateTest : public StartupTest {
public:
    void run() {
        verify(removeAndSlideBit(1, 0) == 0);
        verify(removeAndSlideBit(2, 0) == 1);
        verify(removeAndSlideBit(2, 1) == 0);
        verify(removeAndSlideBit(255, 1) == 127);
        verify(removeAndSlideBit(21, 2) == 9);
        verify(removeAndSlideBit(0x4000000000000001ULL, 62) == 1);
    }
} iu_unittest;

Status NamespaceDetailsCollectionCatalogEntry::removeIndex(OperationContext* opCtx,
                                                           StringData indexName) {
    int idxNo = _findIndexNumber(opCtx, indexName);
    if (idxNo < 0)
        return Status(ErrorCodes::NamespaceNotFound, "index not found to remove");

    RecordId infoLocation = _details->idx(idxNo).info.toRecordId();

    {  // sanity check
        BSONObj info = _indexRecordStore->dataFor(opCtx, infoLocation).toBson();
        invariant(info["name"].String() == indexName);
    }

    {  // drop the namespace
        string indexNamespace = IndexDescriptor::makeIndexNamespace(ns().ns(), indexName);
        Status status = _db->dropCollection(opCtx, indexNamespace);
        if (!status.isOK()) {
            return status;
        }
    }

    {  // all info in the .ns file
        NamespaceDetails* d = _details->writingWithExtra(opCtx);

        // fix the _multiKeyIndexBits, by moving all bits above me down one
        d->multiKeyIndexBits = removeAndSlideBit(d->multiKeyIndexBits, idxNo);

        if (idxNo >= d->nIndexes)
            d->indexBuildsInProgress--;
        else
            d->nIndexes--;

        for (int i = idxNo; i < getTotalIndexCount(opCtx); i++)
            d->idx(i) = d->idx(i + 1);

        d->idx(getTotalIndexCount(opCtx)) = IndexDetails();
    }

    // Someone may be querying the system.indexes namespace directly, so we need to invalidate
    // its cursors.
    MMAPV1DatabaseCatalogEntry::invalidateSystemCollectionRecord(
        opCtx, NamespaceString(_db->name(), "system.indexes"), infoLocation);

    // remove from system.indexes
    _indexRecordStore->deleteRecord(opCtx, infoLocation);

    return Status::OK();
}

Status NamespaceDetailsCollectionCatalogEntry::prepareForIndexBuild(OperationContext* opCtx,
                                                                    const IndexDescriptor* desc) {
    BSONObj spec = desc->infoObj();
    // 1) entry in system.indexs
    StatusWith<RecordId> systemIndexesEntry =
        _indexRecordStore->insertRecord(opCtx, spec.objdata(), spec.objsize(), false);
    if (!systemIndexesEntry.isOK())
        return systemIndexesEntry.getStatus();

    // 2) NamespaceDetails mods
    IndexDetails* id;
    try {
        id = &_details->idx(getTotalIndexCount(opCtx), true);
    } catch (DBException&) {
        _details->allocExtra(opCtx, ns().ns(), _db->_namespaceIndex, getTotalIndexCount(opCtx));
        id = &_details->idx(getTotalIndexCount(opCtx), false);
    }

    const DiskLoc infoLoc = DiskLoc::fromRecordId(systemIndexesEntry.getValue());
    *opCtx->recoveryUnit()->writing(&id->info) = infoLoc;
    *opCtx->recoveryUnit()->writing(&id->head) = DiskLoc();

    opCtx->recoveryUnit()->writingInt(_details->indexBuildsInProgress) += 1;

    // 3) indexes entry in .ns file and system.namespaces
    _db->createNamespaceForIndex(opCtx, desc->indexNamespace());

    // TODO SERVER-22727: Create an entry for path-level multikey info when creating the new index.

    // Mark the collation feature as in use if the index has a non-simple collation.
    if (spec["collation"]) {
        _db->markCollationFeatureAsInUse(opCtx);
    }

    return Status::OK();
}

void NamespaceDetailsCollectionCatalogEntry::indexBuildSuccess(OperationContext* opCtx,
                                                               StringData indexName) {
    int idxNo = _findIndexNumber(opCtx, indexName);
    fassert(17202, idxNo >= 0);

    // Make sure the newly created index is relocated to nIndexes, if it isn't already there
    if (idxNo != getCompletedIndexCount(opCtx)) {
        int toIdxNo = getCompletedIndexCount(opCtx);

        //_details->swapIndex( opCtx, idxNo, toIdxNo );

        // flip main meta data
        IndexDetails temp = _details->idx(idxNo);
        *opCtx->recoveryUnit()->writing(&_details->idx(idxNo)) = _details->idx(toIdxNo);
        *opCtx->recoveryUnit()->writing(&_details->idx(toIdxNo)) = temp;

        // flip multi key bits
        bool tempMultikey = isIndexMultikey(idxNo);
        setIndexIsMultikey(opCtx, idxNo, isIndexMultikey(toIdxNo));
        setIndexIsMultikey(opCtx, toIdxNo, tempMultikey);

        idxNo = toIdxNo;
        invariant((idxNo = _findIndexNumber(opCtx, indexName)));
    }

    opCtx->recoveryUnit()->writingInt(_details->indexBuildsInProgress) -= 1;
    opCtx->recoveryUnit()->writingInt(_details->nIndexes) += 1;

    invariant(isIndexReady(opCtx, indexName));
}

void NamespaceDetailsCollectionCatalogEntry::updateTTLSetting(OperationContext* opCtx,
                                                              StringData idxName,
                                                              long long newExpireSeconds) {
    int idx = _findIndexNumber(opCtx, idxName);
    invariant(idx >= 0);

    IndexDetails& indexDetails = _details->idx(idx);

    BSONObj obj = _indexRecordStore->dataFor(opCtx, indexDetails.info.toRecordId()).toBson();
    const BSONElement oldExpireSecs = obj.getField("expireAfterSeconds");

    // Important that we set the new value in-place.  We are writing directly to the
    // object here so must be careful not to overwrite with a longer numeric type.

    char* nonConstPtr = const_cast<char*>(oldExpireSecs.value());
    switch (oldExpireSecs.type()) {
        case EOO:
            massert(16631, "index does not have an 'expireAfterSeconds' field", false);
            break;
        case NumberInt:
            *opCtx->recoveryUnit()->writing(reinterpret_cast<int*>(nonConstPtr)) = newExpireSeconds;
            break;
        case NumberDouble:
            *opCtx->recoveryUnit()->writing(reinterpret_cast<double*>(nonConstPtr)) =
                newExpireSeconds;
            break;
        case NumberLong:
            *opCtx->recoveryUnit()->writing(reinterpret_cast<long long*>(nonConstPtr)) =
                newExpireSeconds;
            break;
        default:
            massert(16632, "current 'expireAfterSeconds' is not a number", false);
    }
}

void NamespaceDetailsCollectionCatalogEntry::_updateSystemNamespaces(OperationContext* opCtx,
                                                                     const BSONObj& update) {
    if (!_namespacesRecordStore)
        return;

    RecordData entry = _namespacesRecordStore->dataFor(opCtx, _namespacesRecordId);
    const BSONObj newEntry = applyUpdateOperators(entry.releaseToBson(), update);

    Status result = _namespacesRecordStore->updateRecord(
        opCtx, _namespacesRecordId, newEntry.objdata(), newEntry.objsize(), false, NULL);

    if (ErrorCodes::NeedsDocumentMove == result) {
        StatusWith<RecordId> newLocation = _namespacesRecordStore->insertRecord(
            opCtx, newEntry.objdata(), newEntry.objsize(), false);
        fassert(40074, newLocation.getStatus().isOK());

        // Invalidate old namespace record
        MMAPV1DatabaseCatalogEntry::invalidateSystemCollectionRecord(
            opCtx, NamespaceString(_db->name(), "system.namespaces"), _namespacesRecordId);

        _namespacesRecordStore->deleteRecord(opCtx, _namespacesRecordId);

        setNamespacesRecordId(opCtx, newLocation.getValue());
    } else {
        fassert(17486, result.isOK());
    }
}

void NamespaceDetailsCollectionCatalogEntry::updateFlags(OperationContext* opCtx, int newValue) {
    NamespaceDetailsRSV1MetaData md(ns().ns(), _details);
    md.replaceUserFlags(opCtx, newValue);
    _updateSystemNamespaces(opCtx, BSON("$set" << BSON("options.flags" << newValue)));
}

void NamespaceDetailsCollectionCatalogEntry::addUUID(OperationContext* opCtx,
                                                     CollectionUUID uuid,
                                                     Collection* coll) {
    // Add a UUID to CollectionOptions if a UUID does not yet exist.
    if (ns().coll() == "system.namespaces") {
        return;
    }
    RecordData namespaceData;
    invariant(_namespacesRecordStore->findRecord(opCtx, _namespacesRecordId, &namespaceData));

    auto namespacesBson = namespaceData.releaseToBson();

    if (namespacesBson["options"].isABSONObj() && !namespacesBson["options"].Obj()["uuid"].eoo()) {
        fassert(40565, UUID::parse(namespacesBson["options"].Obj()["uuid"]).getValue() == uuid);
    } else {
        _updateSystemNamespaces(opCtx, BSON("$set" << BSON("options.uuid" << uuid)));
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx->getServiceContext());
        catalog.onCreateCollection(opCtx, coll, uuid);
    }
}

void NamespaceDetailsCollectionCatalogEntry::removeUUID(OperationContext* opCtx) {
    // Remove the UUID from CollectionOptions if a UUID exists.
    if (ns().coll() == "system.namespaces") {
        return;
    }
    RecordData namespaceData;
    invariant(_namespacesRecordStore->findRecord(opCtx, _namespacesRecordId, &namespaceData));
    auto namespacesBson = namespaceData.releaseToBson();
    if (!namespacesBson["options"].isABSONObj()) {
        return;
    }
    auto optionsObj = namespacesBson["options"].Obj();

    if (!optionsObj["uuid"].eoo()) {
        CollectionUUID uuid = UUID::parse(optionsObj["uuid"]).getValue();
        _updateSystemNamespaces(opCtx,
                                BSON("$unset" << BSON("options.uuid"
                                                      << "")));
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx->getServiceContext());
        Collection* coll = catalog.lookupCollectionByUUID(uuid);
        if (coll) {
            catalog.onDropCollection(opCtx, uuid);
        }
    }
}

bool NamespaceDetailsCollectionCatalogEntry::isEqualToMetadataUUID(OperationContext* opCtx,
                                                                   OptionalCollectionUUID uuid) {
    if (ns().coll() != "system.namespaces") {
        RecordData namespaceData;
        invariant(_namespacesRecordStore->findRecord(opCtx, _namespacesRecordId, &namespaceData));

        auto namespacesBson = namespaceData.releaseToBson();
        if (uuid && namespacesBson["options"].isABSONObj()) {
            auto optionsObj = namespacesBson["options"].Obj();
            return !optionsObj["uuid"].eoo() &&
                UUID::parse(optionsObj["uuid"]).getValue() == uuid.get();
        } else {
            return !uuid && (!namespacesBson["options"].isABSONObj() ||
                             namespacesBson["options"].Obj()["uuid"].eoo());
        }
    } else {
        return true;
    }
}

void NamespaceDetailsCollectionCatalogEntry::updateValidator(OperationContext* opCtx,
                                                             const BSONObj& validator,
                                                             StringData validationLevel,
                                                             StringData validationAction) {
    _updateSystemNamespaces(
        opCtx,
        BSON("$set" << BSON("options.validator" << validator << "options.validationLevel"
                                                << validationLevel
                                                << "options.validationAction"
                                                << validationAction)));
}

void NamespaceDetailsCollectionCatalogEntry::setNamespacesRecordId(OperationContext* opCtx,
                                                                   RecordId newId) {
    if (newId.isNull()) {
        invariant(ns().coll() == "system.namespaces" || ns().coll() == "system.indexes");
    } else {
        // 'opCtx' is allowed to be null, but we don't need an OperationContext in MMAP, so that's
        // OK.
        auto namespaceEntry = _namespacesRecordStore->dataFor(opCtx, newId).releaseToBson();
        invariant(namespaceEntry["name"].String() == ns().ns());

        // Register RecordId change for rollback if we're not initializing.
        if (opCtx && !_namespacesRecordId.isNull()) {
            auto oldNamespacesRecordId = _namespacesRecordId;
            opCtx->recoveryUnit()->onRollback([=] { _namespacesRecordId = oldNamespacesRecordId; });
        }
        _namespacesRecordId = newId;
    }
}

void NamespaceDetailsCollectionCatalogEntry::updateCappedSize(OperationContext* opCtx,
                                                              long long size) {
    invariant(false);
}
}
