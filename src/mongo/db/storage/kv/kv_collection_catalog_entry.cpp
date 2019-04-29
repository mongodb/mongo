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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/kv/kv_collection_catalog_entry.h"

#include <memory>

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/kv/kv_catalog.h"
#include "mongo/db/storage/kv/kv_catalog_feature_tracker.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/util/log.h"

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

    virtual void commit(boost::optional<Timestamp>) {}
    virtual void rollback() {
        // Intentionally ignoring failure.
        auto kvEngine = _cce->_engine->getEngine();
        MONGO_COMPILER_VARIABLE_UNUSED auto status = kvEngine->dropIdent(_opCtx, _ident);
    }

    OperationContext* const _opCtx;
    KVCollectionCatalogEntry* const _cce;
    const std::string _ident;
};

class KVCollectionCatalogEntry::RemoveIndexChange : public RecoveryUnit::Change {
public:
    RemoveIndexChange(OperationContext* opCtx,
                      KVCollectionCatalogEntry* cce,
                      OptionalCollectionUUID uuid,
                      const NamespaceString& indexNss,
                      StringData indexName,
                      StringData ident)
        : _opCtx(opCtx),
          _cce(cce),
          _uuid(uuid),
          _indexNss(indexNss),
          _indexName(indexName),
          _ident(ident.toString()) {}

    virtual void rollback() {}
    virtual void commit(boost::optional<Timestamp> commitTimestamp) {
        // Intentionally ignoring failure here. Since we've removed the metadata pointing to the
        // index, we should never see it again anyway.
        auto engine = _cce->_engine;
        auto storageEngine = engine->getStorageEngine();
        if (storageEngine->supportsPendingDrops() && commitTimestamp) {
            log() << "Deferring table drop for index '" << _indexName << "' on collection '"
                  << _indexNss << (_uuid ? " (" + _uuid->toString() + ")'" : "") << ". Ident: '"
                  << _ident << "', commit timestamp: '" << commitTimestamp << "'";
            engine->addDropPendingIdent(*commitTimestamp, _indexNss, _ident);
        } else {
            auto kvEngine = engine->getEngine();
            MONGO_COMPILER_VARIABLE_UNUSED auto status = kvEngine->dropIdent(_opCtx, _ident);
        }
    }

    OperationContext* const _opCtx;
    KVCollectionCatalogEntry* const _cce;
    OptionalCollectionUUID _uuid;
    const NamespaceString _indexNss;
    const std::string _indexName;
    const std::string _ident;
};


KVCollectionCatalogEntry::KVCollectionCatalogEntry(KVStorageEngineInterface* engine,
                                                   KVCatalog* catalog,
                                                   StringData ns,
                                                   StringData ident,
                                                   std::unique_ptr<RecordStore> rs)
    : BSONCollectionCatalogEntry(ns),
      _engine(engine),
      _catalog(catalog),
      _ident(ident.toString()),
      _recordStore(std::move(rs)) {}

KVCollectionCatalogEntry::~KVCollectionCatalogEntry() {}

bool KVCollectionCatalogEntry::setIndexIsMultikey(OperationContext* opCtx,
                                                  StringData indexName,
                                                  const MultikeyPaths& multikeyPaths) {
    MetaData md = _getMetaData(opCtx);

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

    _catalog->putMetaData(opCtx, ns(), md);
    return true;
}

void KVCollectionCatalogEntry::setIndexKeyStringWithLongTypeBitsExistsOnDisk(
    OperationContext* opCtx) {
    const auto feature =
        KVCatalog::FeatureTracker::RepairableFeature::kIndexKeyStringWithLongTypeBits;
    if (!_catalog->getFeatureTracker()->isRepairableFeatureInUse(opCtx, feature)) {
        _catalog->getFeatureTracker()->markRepairableFeatureAsInUse(opCtx, feature);
    }
}

void KVCollectionCatalogEntry::setIndexHead(OperationContext* opCtx,
                                            StringData indexName,
                                            const RecordId& newHead) {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    md.indexes[offset].head = newHead;
    _catalog->putMetaData(opCtx, ns(), md);
}

Status KVCollectionCatalogEntry::removeIndex(OperationContext* opCtx, StringData indexName) {
    MetaData md = _getMetaData(opCtx);

    if (md.findIndexOffset(indexName) < 0)
        return Status::OK();  // never had the index so nothing to do.

    const string ident = _catalog->getIndexIdent(opCtx, ns(), indexName);

    md.eraseIndex(indexName);
    _catalog->putMetaData(opCtx, ns(), md);

    // Lazily remove to isolate underlying engine from rollback.
    opCtx->recoveryUnit()->registerChange(new RemoveIndexChange(
        opCtx, this, md.options.uuid, ns().makeIndexNamespace(indexName), indexName, ident));
    return Status::OK();
}

Status KVCollectionCatalogEntry::prepareForIndexBuild(OperationContext* opCtx,
                                                      const IndexDescriptor* spec,
                                                      IndexBuildProtocol indexBuildProtocol,
                                                      bool isBackgroundSecondaryBuild) {
    MetaData md = _getMetaData(opCtx);

    KVPrefix prefix = KVPrefix::getNextPrefix(ns());
    IndexMetaData imd;
    imd.spec = spec->infoObj();
    imd.ready = false;
    imd.head = RecordId();
    imd.multikey = false;
    imd.prefix = prefix;
    imd.isBackgroundSecondaryBuild = isBackgroundSecondaryBuild;
    imd.runTwoPhaseBuild = indexBuildProtocol == IndexBuildProtocol::kTwoPhase;

    if (indexTypeSupportsPathLevelMultikeyTracking(spec->getAccessMethodName())) {
        const auto feature =
            KVCatalog::FeatureTracker::RepairableFeature::kPathLevelMultikeyTracking;
        if (!_catalog->getFeatureTracker()->isRepairableFeatureInUse(opCtx, feature)) {
            _catalog->getFeatureTracker()->markRepairableFeatureAsInUse(opCtx, feature);
        }
        imd.multikeyPaths = MultikeyPaths{static_cast<size_t>(spec->keyPattern().nFields())};
    }

    // Mark collation feature as in use if the index has a non-simple collation.
    if (imd.spec["collation"]) {
        const auto feature = KVCatalog::FeatureTracker::NonRepairableFeature::kCollation;
        if (!_catalog->getFeatureTracker()->isNonRepairableFeatureInUse(opCtx, feature)) {
            _catalog->getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx, feature);
        }
    }

    md.indexes.push_back(imd);
    _catalog->putMetaData(opCtx, ns(), md);

    string ident = _catalog->getIndexIdent(opCtx, ns(), spec->indexName());

    auto kvEngine = _engine->getEngine();
    const Status status = kvEngine->createGroupedSortedDataInterface(opCtx, ident, spec, prefix);
    if (status.isOK()) {
        opCtx->recoveryUnit()->registerChange(new AddIndexChange(opCtx, this, ident));
    }

    return status;
}

bool KVCollectionCatalogEntry::isTwoPhaseIndexBuild(OperationContext* opCtx,
                                                    StringData indexName) const {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    return md.indexes[offset].runTwoPhaseBuild;
}

long KVCollectionCatalogEntry::getIndexBuildVersion(OperationContext* opCtx,
                                                    StringData indexName) const {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    return md.indexes[offset].versionOfBuild;
}

void KVCollectionCatalogEntry::setIndexBuildScanning(
    OperationContext* opCtx,
    StringData indexName,
    std::string sideWritesIdent,
    boost::optional<std::string> constraintViolationsIdent) {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    invariant(!md.indexes[offset].ready);
    invariant(!md.indexes[offset].buildPhase);
    invariant(md.indexes[offset].runTwoPhaseBuild);

    md.indexes[offset].buildPhase = kIndexBuildScanning.toString();
    md.indexes[offset].sideWritesIdent = sideWritesIdent;
    md.indexes[offset].constraintViolationsIdent = constraintViolationsIdent;
    _catalog->putMetaData(opCtx, ns(), md);
}

bool KVCollectionCatalogEntry::isIndexBuildScanning(OperationContext* opCtx,
                                                    StringData indexName) const {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    return md.indexes[offset].buildPhase == kIndexBuildScanning.toString();
}

void KVCollectionCatalogEntry::setIndexBuildDraining(OperationContext* opCtx,
                                                     StringData indexName) {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    invariant(!md.indexes[offset].ready);
    invariant(md.indexes[offset].runTwoPhaseBuild);
    invariant(md.indexes[offset].buildPhase == kIndexBuildScanning.toString());

    md.indexes[offset].buildPhase = kIndexBuildDraining.toString();
    _catalog->putMetaData(opCtx, ns(), md);
}

bool KVCollectionCatalogEntry::isIndexBuildDraining(OperationContext* opCtx,
                                                    StringData indexName) const {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    return md.indexes[offset].buildPhase == kIndexBuildDraining.toString();
}

void KVCollectionCatalogEntry::indexBuildSuccess(OperationContext* opCtx, StringData indexName) {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    md.indexes[offset].ready = true;
    md.indexes[offset].runTwoPhaseBuild = false;
    md.indexes[offset].buildPhase = boost::none;
    md.indexes[offset].sideWritesIdent = boost::none;
    md.indexes[offset].constraintViolationsIdent = boost::none;
    _catalog->putMetaData(opCtx, ns(), md);
}

boost::optional<std::string> KVCollectionCatalogEntry::getSideWritesIdent(
    OperationContext* opCtx, StringData indexName) const {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    return md.indexes[offset].sideWritesIdent;
}

boost::optional<std::string> KVCollectionCatalogEntry::getConstraintViolationsIdent(
    OperationContext* opCtx, StringData indexName) const {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    return md.indexes[offset].constraintViolationsIdent;
}

void KVCollectionCatalogEntry::updateTTLSetting(OperationContext* opCtx,
                                                StringData idxName,
                                                long long newExpireSeconds) {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(idxName);
    invariant(offset >= 0);
    md.indexes[offset].updateTTLSetting(newExpireSeconds);
    _catalog->putMetaData(opCtx, ns(), md);
}

void KVCollectionCatalogEntry::updateIndexMetadata(OperationContext* opCtx,
                                                   const IndexDescriptor* desc) {
    // Update any metadata Ident has for this index
    const string ident = _catalog->getIndexIdent(opCtx, ns(), desc->indexName());
    auto kvEngine = _engine->getEngine();
    kvEngine->alterIdentMetadata(opCtx, ident, desc);
}

bool KVCollectionCatalogEntry::isEqualToMetadataUUID(OperationContext* opCtx,
                                                     OptionalCollectionUUID uuid) {
    MetaData md = _getMetaData(opCtx);
    return md.options.uuid && md.options.uuid == uuid;
}

void KVCollectionCatalogEntry::updateValidator(OperationContext* opCtx,
                                               const BSONObj& validator,
                                               StringData validationLevel,
                                               StringData validationAction) {
    MetaData md = _getMetaData(opCtx);
    md.options.validator = validator;
    md.options.validationLevel = validationLevel.toString();
    md.options.validationAction = validationAction.toString();
    _catalog->putMetaData(opCtx, ns(), md);
}

void KVCollectionCatalogEntry::setIsTemp(OperationContext* opCtx, bool isTemp) {
    MetaData md = _getMetaData(opCtx);
    md.options.temp = isTemp;
    _catalog->putMetaData(opCtx, ns(), md);
}

void KVCollectionCatalogEntry::updateCappedSize(OperationContext* opCtx, long long size) {
    MetaData md = _getMetaData(opCtx);
    md.options.cappedSize = size;
    _catalog->putMetaData(opCtx, ns(), md);
}

BSONCollectionCatalogEntry::MetaData KVCollectionCatalogEntry::_getMetaData(
    OperationContext* opCtx) const {
    return _catalog->getMetaData(opCtx, ns());
}
}
