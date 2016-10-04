// kv_collection_catalog_entry.cpp

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

#include "mongo/db/storage/kv/kv_collection_catalog_entry.h"

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/kv/kv_catalog.h"
#include "mongo/db/storage/kv/kv_catalog_feature_tracker.h"
#include "mongo/db/storage/kv/kv_engine.h"

namespace mongo {

using std::string;

namespace {

bool indexTypeSupportsPathLevelMultikeyTracking(StringData accessMethod) {
    return accessMethod == IndexNames::BTREE || accessMethod == IndexNames::GEO_2DSPHERE;
}

}  // namespace

class KVCollectionCatalogEntry::AddIndexChange : public RecoveryUnit::Change {
public:
    AddIndexChange(OperationContext* opCtx, KVCollectionCatalogEntry* cce, StringData ident)
        : _opCtx(opCtx), _cce(cce), _ident(ident.toString()) {}

    virtual void commit() {}
    virtual void rollback() {
        // Intentionally ignoring failure.
        _cce->_engine->dropIdent(_opCtx, _ident);
    }

    OperationContext* const _opCtx;
    KVCollectionCatalogEntry* const _cce;
    const std::string _ident;
};

class KVCollectionCatalogEntry::RemoveIndexChange : public RecoveryUnit::Change {
public:
    RemoveIndexChange(OperationContext* opCtx, KVCollectionCatalogEntry* cce, StringData ident)
        : _opCtx(opCtx), _cce(cce), _ident(ident.toString()) {}

    virtual void rollback() {}
    virtual void commit() {
        // Intentionally ignoring failure here. Since we've removed the metadata pointing to the
        // index, we should never see it again anyway.
        _cce->_engine->dropIdent(_opCtx, _ident);
    }

    OperationContext* const _opCtx;
    KVCollectionCatalogEntry* const _cce;
    const std::string _ident;
};


KVCollectionCatalogEntry::KVCollectionCatalogEntry(
    KVEngine* engine, KVCatalog* catalog, StringData ns, StringData ident, RecordStore* rs)
    : BSONCollectionCatalogEntry(ns),
      _engine(engine),
      _catalog(catalog),
      _ident(ident.toString()),
      _recordStore(rs) {}

KVCollectionCatalogEntry::~KVCollectionCatalogEntry() {}

bool KVCollectionCatalogEntry::setIndexIsMultikey(OperationContext* txn,
                                                  StringData indexName,
                                                  const MultikeyPaths& multikeyPaths) {
    MetaData md = _getMetaData(txn);

    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);

    const bool tracksPathLevelMultikeyInfo = !md.indexes[offset].multikeyPaths.empty();
    if (tracksPathLevelMultikeyInfo) {
        invariant(!multikeyPaths.empty());
        invariant(multikeyPaths.size() == md.indexes[offset].multikeyPaths.size());
    } else {
        invariant(multikeyPaths.empty());

        if (md.indexes[offset].multikey) {
            // The index is already set as multikey and we aren't tracking path-level multikey
            // information for it. We return false to indicate that the index metadata is unchanged.
            return false;
        }
    }

    md.indexes[offset].multikey = true;

    if (tracksPathLevelMultikeyInfo) {
        bool newPathIsMultikey = false;
        bool somePathIsMultikey = false;

        // Store new path components that cause this index to be multikey in catalog's index
        // metadata.
        for (size_t i = 0; i < multikeyPaths.size(); ++i) {
            std::set<size_t>& indexMultikeyComponents = md.indexes[offset].multikeyPaths[i];
            for (const auto multikeyComponent : multikeyPaths[i]) {
                auto result = indexMultikeyComponents.insert(multikeyComponent);
                newPathIsMultikey = newPathIsMultikey || result.second;
                somePathIsMultikey = true;
            }
        }

        // If all of the sets in the multikey paths vector were empty, then no component of any
        // indexed field caused the index to be multikey. setIndexIsMultikey() therefore shouldn't
        // have been called.
        invariant(somePathIsMultikey);

        if (!newPathIsMultikey) {
            // We return false to indicate that the index metadata is unchanged.
            return false;
        }
    }

    _catalog->putMetaData(txn, ns().toString(), md);
    return true;
}

void KVCollectionCatalogEntry::setIndexHead(OperationContext* txn,
                                            StringData indexName,
                                            const RecordId& newHead) {
    MetaData md = _getMetaData(txn);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    md.indexes[offset].head = newHead;
    _catalog->putMetaData(txn, ns().toString(), md);
}

Status KVCollectionCatalogEntry::removeIndex(OperationContext* txn, StringData indexName) {
    MetaData md = _getMetaData(txn);

    if (md.findIndexOffset(indexName) < 0)
        return Status::OK();  // never had the index so nothing to do.

    const string ident = _catalog->getIndexIdent(txn, ns().ns(), indexName);

    md.eraseIndex(indexName);
    _catalog->putMetaData(txn, ns().toString(), md);

    // Lazily remove to isolate underlying engine from rollback.
    txn->recoveryUnit()->registerChange(new RemoveIndexChange(txn, this, ident));
    return Status::OK();
}

Status KVCollectionCatalogEntry::prepareForIndexBuild(OperationContext* txn,
                                                      const IndexDescriptor* spec) {
    MetaData md = _getMetaData(txn);
    IndexMetaData imd(spec->infoObj(), false, RecordId(), false);
    if (indexTypeSupportsPathLevelMultikeyTracking(spec->getAccessMethodName())) {
        const auto feature =
            KVCatalog::FeatureTracker::RepairableFeature::kPathLevelMultikeyTracking;
        if (!_catalog->getFeatureTracker()->isRepairableFeatureInUse(txn, feature)) {
            _catalog->getFeatureTracker()->markRepairableFeatureAsInUse(txn, feature);
        }
        imd.multikeyPaths = MultikeyPaths{static_cast<size_t>(spec->keyPattern().nFields())};
    }

    // Mark collation feature as in use if the index has a non-simple collation.
    if (imd.spec["collation"]) {
        const auto feature = KVCatalog::FeatureTracker::NonRepairableFeature::kCollation;
        if (!_catalog->getFeatureTracker()->isNonRepairableFeatureInUse(txn, feature)) {
            _catalog->getFeatureTracker()->markNonRepairableFeatureAsInUse(txn, feature);
        }
    }

    md.indexes.push_back(imd);
    _catalog->putMetaData(txn, ns().toString(), md);

    string ident = _catalog->getIndexIdent(txn, ns().ns(), spec->indexName());

    const Status status = _engine->createSortedDataInterface(txn, ident, spec);
    if (status.isOK()) {
        txn->recoveryUnit()->registerChange(new AddIndexChange(txn, this, ident));
    }

    return status;
}

void KVCollectionCatalogEntry::indexBuildSuccess(OperationContext* txn, StringData indexName) {
    MetaData md = _getMetaData(txn);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    md.indexes[offset].ready = true;
    _catalog->putMetaData(txn, ns().toString(), md);
}

void KVCollectionCatalogEntry::updateTTLSetting(OperationContext* txn,
                                                StringData idxName,
                                                long long newExpireSeconds) {
    MetaData md = _getMetaData(txn);
    int offset = md.findIndexOffset(idxName);
    invariant(offset >= 0);
    md.indexes[offset].updateTTLSetting(newExpireSeconds);
    _catalog->putMetaData(txn, ns().toString(), md);
}

void KVCollectionCatalogEntry::updateFlags(OperationContext* txn, int newValue) {
    MetaData md = _getMetaData(txn);
    md.options.flags = newValue;
    md.options.flagsSet = true;
    _catalog->putMetaData(txn, ns().toString(), md);
}

void KVCollectionCatalogEntry::clearTempFlag(OperationContext* txn) {
    MetaData md = _getMetaData(txn);
    md.options.temp = false;
    _catalog->putMetaData(txn, ns().ns(), md);
}

void KVCollectionCatalogEntry::updateValidator(OperationContext* txn,
                                               const BSONObj& validator,
                                               StringData validationLevel,
                                               StringData validationAction) {
    MetaData md = _getMetaData(txn);
    md.options.validator = validator;
    md.options.validationLevel = validationLevel.toString();
    md.options.validationAction = validationAction.toString();
    _catalog->putMetaData(txn, ns().toString(), md);
}

BSONCollectionCatalogEntry::MetaData KVCollectionCatalogEntry::_getMetaData(
    OperationContext* txn) const {
    return _catalog->getMetaData(txn, ns().toString());
}
}
