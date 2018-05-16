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
#include "mongo/db/catalog/collection_info_cache_impl.h"
#include "mongo/db/catalog/head_manager.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
MONGO_REGISTER_SHIM(IndexCatalogEntry::makeImpl)
(IndexCatalogEntry* const this_,
 OperationContext* const opCtx,
 const StringData ns,
 CollectionCatalogEntry* const collection,
 std::unique_ptr<IndexDescriptor> descriptor,
 CollectionInfoCache* const infoCache,
 PrivateTo<IndexCatalogEntry>)
    ->std::unique_ptr<IndexCatalogEntry::Impl> {
    return std::make_unique<IndexCatalogEntryImpl>(
        this_, opCtx, ns, collection, std::move(descriptor), infoCache);
}

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
        invariant(statusWithCollator.getStatus());

        _collator = std::move(statusWithCollator.getValue());
    }

    if (BSONElement filterElement = _descriptor->getInfoElement("partialFilterExpression")) {
        invariant(filterElement.isABSONObj());
        BSONObj filter = filterElement.Obj();
        boost::intrusive_ptr<ExpressionContext> expCtx(
            new ExpressionContext(opCtx, _collator.get()));

        // Parsing the partial filter expression is not expected to fail here since the
        // expression would have been successfully parsed upstream during index creation.
        StatusWithMatchExpression statusWithMatcher =
            MatchExpressionParser::parse(filter,
                                         std::move(expCtx),
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kBanAllSpecialFeatures);
        invariant(statusWithMatcher.getStatus());
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

bool IndexCatalogEntryImpl::isMultikey(OperationContext* opCtx) const {
    auto ret = _isMultikey.load();
    if (ret) {
        return true;
    }

    // Multikey updates are only persisted, to disk and in memory, when the transaction
    // commits. In the case of multi-statement transactions, a client attempting to read their own
    // transactions writes can return wrong results if their writes include multikey changes.
    //
    // To accomplish this, the write-path will persist multikey changes on the `Session` object
    // and the read-path will query this state before determining there is no interesting multikey
    // state. Note, it's always legal, though potentially wasteful, to return `true`.
    auto session = OperationContextSession::get(opCtx);
    if (!session || !session->inMultiDocumentTransaction()) {
        return false;
    }

    for (const MultikeyPathInfo& path : session->getMultikeyPathInfo()) {
        if (path.nss == NamespaceString(_ns) && path.indexName == _descriptor->indexName()) {
            return true;
        }
    }

    return false;
}

MultikeyPaths IndexCatalogEntryImpl::getMultikeyPaths(OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lk(_indexMultikeyPathsMutex);

    auto session = OperationContextSession::get(opCtx);
    if (!session || !session->inMultiDocumentTransaction()) {
        return _indexMultikeyPaths;
    }

    MultikeyPaths ret = _indexMultikeyPaths;
    for (const MultikeyPathInfo& path : session->getMultikeyPathInfo()) {
        if (path.nss == NamespaceString(_ns) && path.indexName == _descriptor->indexName()) {
            MultikeyPathTracker::mergeMultikeyPaths(&ret, path.multikeyPaths);
        }
    }

    return ret;
}

// ---

void IndexCatalogEntryImpl::setIsReady(bool newIsReady) {
    _isReady = newIsReady;
}

class IndexCatalogEntryImpl::SetHeadChange : public RecoveryUnit::Change {
public:
    SetHeadChange(IndexCatalogEntryImpl* ice, RecordId oldHead) : _ice(ice), _oldHead(oldHead) {}

    virtual void commit(boost::optional<Timestamp>) {}
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

void IndexCatalogEntryImpl::setMultikey(OperationContext* opCtx,
                                        const MultikeyPaths& multikeyPaths) {
    if (!_indexTracksPathLevelMultikeyInfo && isMultikey(opCtx)) {
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

    MultikeyPaths paths = _indexTracksPathLevelMultikeyInfo ? multikeyPaths : MultikeyPaths{};

    // On a primary, we can simply assign this write the same timestamp as the index creation,
    // insert, or update that caused this index to become multikey. This is because if two
    // operations concurrently try to change the index to be multikey, they will conflict and the
    // loser will simply get a higher timestamp and go into the oplog second with a later optime.
    //
    // On a secondary, writes must get the timestamp of their oplog entry, and the multikey change
    // must occur before the timestamp of the earliest write that makes the index multikey.
    // Secondaries only serialize writes by document, not by collection. If two inserts that both
    // make an index multikey are applied out of order, changing the index to multikey at the
    // insert timestamps would change the index to multikey at the later timestamp, which would be
    // wrong. To prevent this, rather than setting the index to be multikey here, we add the
    // necessary information to the OperationContext and do the write at the timestamp of the
    // beginning of the batch.
    //
    // One exception to this rule is for background indexes. Background indexes are built using
    // a different OperationContext and thus this information would be ignored. Background index
    // builds happen concurrently though and thus the multikey write can safely occur at the
    // current clock time. Once a background index is committed, if a future write makes
    // it multikey, that write will be marked as "isTrackingMultikeyPathInfo" on the applier's
    // OperationContext and we can safely defer that write to the end of the batch.
    if (MultikeyPathTracker::get(opCtx).isTrackingMultikeyPathInfo()) {
        MultikeyPathInfo info;
        info.nss = _collection->ns();
        info.indexName = _descriptor->indexName();
        info.multikeyPaths = paths;
        MultikeyPathTracker::get(opCtx).addMultikeyPathInfo(info);
        return;
    }

    // It's possible that the index type (e.g. ascending/descending index) supports tracking
    // path-level multikey information, but this particular index doesn't.
    // CollectionCatalogEntry::setIndexIsMultikey() requires that we discard the path-level
    // multikey information in order to avoid unintentionally setting path-level multikey
    // information on an index created before 3.4.
    const bool indexMetadataHasChanged =
        _collection->setIndexIsMultikey(opCtx, _descriptor->indexName(), paths);

    // When the recovery unit commits, update the multikey paths if needed and clear the plan cache
    // if the index metadata has changed.
    opCtx->recoveryUnit()->onCommit(
        [this, multikeyPaths, indexMetadataHasChanged](boost::optional<Timestamp>) {
            _isMultikey.store(true);

            if (_indexTracksPathLevelMultikeyInfo) {
                stdx::lock_guard<stdx::mutex> lk(_indexMultikeyPathsMutex);
                for (size_t i = 0; i < multikeyPaths.size(); ++i) {
                    _indexMultikeyPaths[i].insert(multikeyPaths[i].begin(), multikeyPaths[i].end());
                }
            }

            if (indexMetadataHasChanged && _infoCache) {
                LOG(1) << _ns << ": clearing plan cache - index " << _descriptor->keyPattern()
                       << " set to multi key.";
                _infoCache->clearQueryCache();
            }
        });

    // Keep multikey changes in memory to correctly service later reads using this index.
    auto session = OperationContextSession::get(opCtx);
    if (session && session->inMultiDocumentTransaction()) {
        MultikeyPathInfo info;
        info.nss = _collection->ns();
        info.indexName = _descriptor->indexName();
        info.multikeyPaths = paths;
        session->addMultikeyPathInfo(std::move(info));
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
