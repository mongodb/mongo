/**
*    Copyright (C) 2013 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog_entry_impl.h"

#include <algorithm>

#include "mongo/base/init.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/head_manager.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {
MONGO_INITIALIZER(InitializeIndexCatalogEntryFactory)(InitializerContext* const) {
    IndexCatalogEntry::registerFactory([](IndexCatalogEntry* const this_,
                                          OperationContext* const opCtx,
                                          const StringData ns,
                                          CollectionCatalogEntry* const collection,
                                          std::unique_ptr<IndexDescriptor> descriptor,
                                          CollectionInfoCache* const infoCache) {
        return stdx::make_unique<IndexCatalogEntryImpl>(
            this_, opCtx, ns, collection, std::move(descriptor), infoCache);
    });
    return Status::OK();
}
}  // namespace

using std::string;

class HeadManagerImpl : public HeadManager {
public:
    HeadManagerImpl(IndexCatalogEntry* ice) : _catalogEntry(ice) {}
    virtual ~HeadManagerImpl() {}

    const RecordId getHead(OperationContext* opCtx) const {
        return _catalogEntry->head(opCtx);
    }

    void setHead(OperationContext* opCtx, const RecordId newHead) {
        _catalogEntry->setHead(opCtx, newHead);
    }

private:
    // Not owned here.
    IndexCatalogEntry* _catalogEntry;
};

IndexCatalogEntryImpl::IndexCatalogEntryImpl(IndexCatalogEntry* const this_,
                                             OperationContext* const opCtx,
                                             const StringData ns,
                                             CollectionCatalogEntry* const collection,
                                             std::unique_ptr<IndexDescriptor> descriptor,
                                             CollectionInfoCache* const infoCache)
    : _ns(ns.toString()),
      _collection(collection),
      _descriptor(std::move(descriptor)),
      _infoCache(infoCache),
      _headManager(stdx::make_unique<HeadManagerImpl>(this_)),
      _ordering(Ordering::make(_descriptor->keyPattern())),
      _isReady(false),
      _prefix(collection->getIndexPrefix(opCtx, _descriptor->indexName())) {
    _descriptor->_cachedEntry = this_;

    _isReady = _catalogIsReady(opCtx);
    _head = _catalogHead(opCtx);

    {
        stdx::lock_guard<stdx::mutex> lk(_indexMultikeyPathsMutex);
        _isMultikey.store(_catalogIsMultikey(opCtx, &_indexMultikeyPaths));
        _indexTracksPathLevelMultikeyInfo = !_indexMultikeyPaths.empty();
    }

    if (BSONElement collationElement = _descriptor->getInfoElement("collation")) {
        invariant(collationElement.isABSONObj());
        BSONObj collation = collationElement.Obj();
        auto statusWithCollator =
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation);

        // Index spec should have already been validated.
        invariantOK(statusWithCollator.getStatus());

        _collator = std::move(statusWithCollator.getValue());
    }

    if (BSONElement filterElement = _descriptor->getInfoElement("partialFilterExpression")) {
        invariant(filterElement.isABSONObj());
        BSONObj filter = filterElement.Obj();
        StatusWithMatchExpression statusWithMatcher = MatchExpressionParser::parse(
            filter, ExtensionsCallbackDisallowExtensions(), _collator.get());
        // this should be checked in create, so can blow up here
        invariantOK(statusWithMatcher.getStatus());
        _filterExpression = std::move(statusWithMatcher.getValue());
        LOG(2) << "have filter expression for " << _ns << " " << _descriptor->indexName() << " "
               << redact(filter);
    }
}

IndexCatalogEntryImpl::~IndexCatalogEntryImpl() {
    _descriptor->_cachedEntry = nullptr;  // defensive

    _headManager.reset();
    _descriptor.reset();
}

void IndexCatalogEntryImpl::init(std::unique_ptr<IndexAccessMethod> accessMethod) {
    invariant(!_accessMethod);
    _accessMethod = std::move(accessMethod);
}

const RecordId& IndexCatalogEntryImpl::head(OperationContext* opCtx) const {
    DEV invariant(_head == _catalogHead(opCtx));
    return _head;
}

bool IndexCatalogEntryImpl::isReady(OperationContext* opCtx) const {
    DEV invariant(_isReady == _catalogIsReady(opCtx));
    return _isReady;
}

bool IndexCatalogEntryImpl::isMultikey() const {
    return _isMultikey.load();
}

MultikeyPaths IndexCatalogEntryImpl::getMultikeyPaths(OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lk(_indexMultikeyPathsMutex);
    return _indexMultikeyPaths;
}

// ---

void IndexCatalogEntryImpl::setIsReady(bool newIsReady) {
    _isReady = newIsReady;
}

class IndexCatalogEntryImpl::SetHeadChange : public RecoveryUnit::Change {
public:
    SetHeadChange(IndexCatalogEntryImpl* ice, RecordId oldHead) : _ice(ice), _oldHead(oldHead) {}

    virtual void commit() {}
    virtual void rollback() {
        _ice->_head = _oldHead;
    }

    IndexCatalogEntryImpl* _ice;
    const RecordId _oldHead;
};

void IndexCatalogEntryImpl::setHead(OperationContext* opCtx, RecordId newHead) {
    _collection->setIndexHead(opCtx, _descriptor->indexName(), newHead);

    opCtx->recoveryUnit()->registerChange(new SetHeadChange(this, _head));
    _head = newHead;
}


/**
 * RAII class, which associates a new RecoveryUnit with an OperationContext for the purposes
 * of simulating a side-transaction. Takes ownership of the new recovery unit and frees it at
 * destruction time.
 */
class RecoveryUnitSwap {
public:
    RecoveryUnitSwap(OperationContext* opCtx, RecoveryUnit* newRecoveryUnit)
        : _opCtx(opCtx),
          _oldRecoveryUnit(_opCtx->releaseRecoveryUnit()),
          _oldRecoveryUnitState(
              _opCtx->setRecoveryUnit(newRecoveryUnit, OperationContext::kNotInUnitOfWork)),
          _newRecoveryUnit(newRecoveryUnit) {}

    ~RecoveryUnitSwap() {
        _opCtx->releaseRecoveryUnit();
        _opCtx->setRecoveryUnit(_oldRecoveryUnit, _oldRecoveryUnitState);
    }

private:
    // Not owned
    OperationContext* const _opCtx;

    // Owned, but life-time is not controlled
    RecoveryUnit* const _oldRecoveryUnit;
    OperationContext::RecoveryUnitState const _oldRecoveryUnitState;

    // Owned and life-time is controlled
    const std::unique_ptr<RecoveryUnit> _newRecoveryUnit;
};

void IndexCatalogEntryImpl::setMultikey(OperationContext* opCtx,
                                        const MultikeyPaths& multikeyPaths) {
    if (!_indexTracksPathLevelMultikeyInfo && isMultikey()) {
        // If the index is already set as multikey and we don't have any path-level information to
        // update, then there's nothing more for us to do.
        return;
    }

    if (_indexTracksPathLevelMultikeyInfo) {
        stdx::lock_guard<stdx::mutex> lk(_indexMultikeyPathsMutex);
        invariant(multikeyPaths.size() == _indexMultikeyPaths.size());

        bool newPathIsMultikey = false;
        for (size_t i = 0; i < multikeyPaths.size(); ++i) {
            if (!std::includes(_indexMultikeyPaths[i].begin(),
                               _indexMultikeyPaths[i].end(),
                               multikeyPaths[i].begin(),
                               multikeyPaths[i].end())) {
                // If 'multikeyPaths' contains a new path component that causes this index to be
                // multikey, then we must update the index metadata in the CollectionCatalogEntry.
                newPathIsMultikey = true;
                break;
            }
        }

        if (!newPathIsMultikey) {
            // Otherwise, if all the path components in 'multikeyPaths' are already tracked in
            // '_indexMultikeyPaths', then there's nothing more for us to do.
            return;
        }
    }

    {
        // Only one thread should set the multi-key value per collection, because the metadata for a
        // collection is one large document.
        Lock::ResourceLock collMDLock(
            opCtx->lockState(), ResourceId(RESOURCE_METADATA, _ns), MODE_X);

        if (!_indexTracksPathLevelMultikeyInfo && isMultikey()) {
            // It's possible that we raced with another thread when acquiring the MD lock. If the
            // index is already set as multikey and we don't have any path-level information to
            // update, then there's nothing more for us to do.
            return;
        }

        // This effectively emulates a side-transaction off the main transaction, which invoked
        // setMultikey. The reason we need is to avoid artificial WriteConflicts, which happen with
        // snapshot isolation.
        {
            StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
            RecoveryUnitSwap ruSwap(opCtx, storageEngine->newRecoveryUnit());

            WriteUnitOfWork wuow(opCtx);

            // It's possible that the index type (e.g. ascending/descending index) supports tracking
            // path-level multikey information, but this particular index doesn't.
            // CollectionCatalogEntry::setIndexIsMultikey() requires that we discard the path-level
            // multikey information in order to avoid unintentionally setting path-level multikey
            // information on an index created before 3.4.
            if (_collection->setIndexIsMultikey(
                    opCtx,
                    _descriptor->indexName(),
                    _indexTracksPathLevelMultikeyInfo ? multikeyPaths : MultikeyPaths{})) {
                if (_infoCache) {
                    LOG(1) << _ns << ": clearing plan cache - index " << _descriptor->keyPattern()
                           << " set to multi key.";
                    _infoCache->clearQueryCache();
                }
            }

            wuow.commit();
        }
    }

    _isMultikey.store(true);

    if (_indexTracksPathLevelMultikeyInfo) {
        stdx::lock_guard<stdx::mutex> lk(_indexMultikeyPathsMutex);
        for (size_t i = 0; i < multikeyPaths.size(); ++i) {
            _indexMultikeyPaths[i].insert(multikeyPaths[i].begin(), multikeyPaths[i].end());
        }
    }
}

// ----

bool IndexCatalogEntryImpl::_catalogIsReady(OperationContext* opCtx) const {
    return _collection->isIndexReady(opCtx, _descriptor->indexName());
}

RecordId IndexCatalogEntryImpl::_catalogHead(OperationContext* opCtx) const {
    return _collection->getIndexHead(opCtx, _descriptor->indexName());
}

bool IndexCatalogEntryImpl::_catalogIsMultikey(OperationContext* opCtx,
                                               MultikeyPaths* multikeyPaths) const {
    return _collection->isIndexMultikey(opCtx, _descriptor->indexName(), multikeyPaths);
}

KVPrefix IndexCatalogEntryImpl::_catalogGetPrefix(OperationContext* opCtx) const {
    return _collection->getIndexPrefix(opCtx, _descriptor->indexName());
}

}  // namespace mongo
