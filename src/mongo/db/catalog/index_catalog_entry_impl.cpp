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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog_entry_impl.h"

#include <algorithm>

#include "mongo/base/init.h"
#include "mongo/db/catalog/collection_info_cache_impl.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::string;

IndexCatalogEntryImpl::IndexCatalogEntryImpl(OperationContext* const opCtx,
                                             const NamespaceString& nss,
                                             std::unique_ptr<IndexDescriptor> descriptor,
                                             CollectionInfoCache* const infoCache)
    : _ns(nss),
      _descriptor(std::move(descriptor)),
      _infoCache(infoCache),
      _ordering(Ordering::make(_descriptor->keyPattern())),
      _isReady(false),
      _prefix(DurableCatalog::get(opCtx)->getIndexPrefix(opCtx, nss, _descriptor->indexName())) {
    _descriptor->_cachedEntry = this;

    _isReady = _catalogIsReady(opCtx);

    {
        stdx::lock_guard<stdx::mutex> lk(_indexMultikeyPathsMutex);
        _isMultikey.store(_catalogIsMultikey(opCtx, &_indexMultikeyPaths));
        _indexTracksPathLevelMultikeyInfo = !_indexMultikeyPaths.empty();
    }

    const BSONObj& collation = _descriptor->collation();
    if (!collation.isEmpty()) {
        auto statusWithCollator =
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation);

        // Index spec should have already been validated.
        invariant(statusWithCollator.getStatus());

        _collator = std::move(statusWithCollator.getValue());
    }

    if (_descriptor->isPartial()) {
        const BSONObj& filter = _descriptor->partialFilterExpression();

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

    _descriptor.reset();
}

void IndexCatalogEntryImpl::init(std::unique_ptr<IndexAccessMethod> accessMethod) {
    invariant(!_accessMethod);
    _accessMethod = std::move(accessMethod);
}

bool IndexCatalogEntryImpl::isReady(OperationContext* opCtx) const {
    auto txnParticipant = TransactionParticipant::get(opCtx);
    // For multi-document transactions, we can open a snapshot prior to checking the
    // minimumSnapshotVersion on a collection.  This means we are unprotected from reading
    // out-of-sync index catalog entries.  To fix this, we uassert if we detect that the
    // in-memory catalog is out-of-sync with the on-disk catalog.
    if (txnParticipant && txnParticipant.inMultiDocumentTransaction()) {
        if (!_catalogIsPresent(opCtx) || _catalogIsReady(opCtx) != _isReady) {
            uasserted(ErrorCodes::SnapshotUnavailable,
                      str::stream() << "Unable to read from a snapshot due to pending collection"
                                       " catalog changes; please retry the operation.");
        }
    }

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
    auto txnParticipant = TransactionParticipant::get(opCtx);
    if (!txnParticipant || !txnParticipant.inMultiDocumentTransaction()) {
        return false;
    }

    for (const MultikeyPathInfo& path : txnParticipant.getUncommittedMultikeyPathInfos()) {
        if (path.nss == NamespaceString(_ns) && path.indexName == _descriptor->indexName()) {
            return true;
        }
    }

    return false;
}

MultikeyPaths IndexCatalogEntryImpl::getMultikeyPaths(OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lk(_indexMultikeyPathsMutex);

    auto txnParticipant = TransactionParticipant::get(opCtx);
    if (!txnParticipant || !txnParticipant.inMultiDocumentTransaction()) {
        return _indexMultikeyPaths;
    }

    MultikeyPaths ret = _indexMultikeyPaths;
    for (const MultikeyPathInfo& path : txnParticipant.getUncommittedMultikeyPathInfos()) {
        if (path.nss == NamespaceString(_ns) && path.indexName == _descriptor->indexName()) {
            MultikeyPathTracker::mergeMultikeyPaths(&ret, path.multikeyPaths);
        }
    }

    return ret;
}

// ---

void IndexCatalogEntryImpl::setMinimumVisibleSnapshot(Timestamp newMinimumVisibleSnapshot) {
    if (!_minVisibleSnapshot || (newMinimumVisibleSnapshot > _minVisibleSnapshot.get())) {
        _minVisibleSnapshot = newMinimumVisibleSnapshot;
    }
}

void IndexCatalogEntryImpl::setIsReady(bool newIsReady) {
    _isReady = newIsReady;
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
        info.nss = _ns;
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
    bool indexMetadataHasChanged;

    // The commit handler for a transaction that sets the multikey flag. When the recovery unit
    // commits, update the multikey paths if needed and clear the plan cache if the index metadata
    // has changed.
    auto onMultikeyCommitFn = [this, multikeyPaths](bool indexMetadataHasChanged) {
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
    };

    // If we are inside a multi-document transaction, we write the on-disk multikey update in a
    // separate transaction so that it will not generate prepare conflicts with other operations
    // that try to set the multikey flag. In general, it should always be safe to update the
    // multikey flag earlier than necessary, and so we are not concerned with the atomicity of the
    // multikey flag write and the parent transaction. We can do this write separately and commit it
    // before the parent transaction commits.
    auto txnParticipant = TransactionParticipant::get(opCtx);
    if (txnParticipant && txnParticipant.inMultiDocumentTransaction()) {
        TransactionParticipant::SideTransactionBlock sideTxn(opCtx);
        writeConflictRetry(opCtx, "set index multikey", _ns.ns(), [&] {
            WriteUnitOfWork wuow(opCtx);

            // If we have a prepare optime for recovery, then we always use that. During recovery of
            // prepared transactions, the logical clock may not yet be initialized, so we use the
            // prepare timestamp of the transaction for this write. This is safe since the prepare
            // timestamp is always <= the commit timestamp of a transaction, which satisfies the
            // correctness requirement for multikey writes i.e. they must occur at or before the
            // first write that set the multikey flag.
            auto recoveryPrepareOpTime = txnParticipant.getPrepareOpTimeForRecovery();
            Timestamp writeTs = recoveryPrepareOpTime.isNull()
                ? LogicalClock::get(opCtx)->getClusterTime().asTimestamp()
                : recoveryPrepareOpTime.getTimestamp();

            auto status = opCtx->recoveryUnit()->setTimestamp(writeTs);
            if (status.code() == ErrorCodes::BadValue) {
                log() << "Temporarily could not timestamp the multikey catalog write, retrying. "
                      << status.reason();
                throw WriteConflictException();
            }
            fassert(31164, status);
            indexMetadataHasChanged = DurableCatalog::get(opCtx)->setIndexIsMultikey(
                opCtx, _ns, _descriptor->indexName(), paths);
            opCtx->recoveryUnit()->onCommit([onMultikeyCommitFn, indexMetadataHasChanged](
                boost::optional<Timestamp>) { onMultikeyCommitFn(indexMetadataHasChanged); });
            wuow.commit();
        });
    } else {
        indexMetadataHasChanged = DurableCatalog::get(opCtx)->setIndexIsMultikey(
            opCtx, _ns, _descriptor->indexName(), paths);
    }

    opCtx->recoveryUnit()->onCommit([onMultikeyCommitFn, indexMetadataHasChanged](
        boost::optional<Timestamp>) { onMultikeyCommitFn(indexMetadataHasChanged); });

    // Within a multi-document transaction, reads should be able to see the effect of previous
    // writes done within that transaction. If a previous write in a transaction has set the index
    // to be multikey, then a subsequent read MUST know that fact in order to return correct
    // results. This is true in general for multikey writes. Since we don't update the in-memory
    // multikey flag until after the transaction commits, we track extra information here to let
    // subsequent readers within the same transaction know if this index was set as multikey by a
    // previous write in the transaction.
    if (txnParticipant && txnParticipant.inMultiDocumentTransaction()) {
        txnParticipant.addUncommittedMultikeyPathInfo(
            MultikeyPathInfo{_ns, _descriptor->indexName(), std::move(paths)});
    }
}

void IndexCatalogEntryImpl::setNs(NamespaceString ns) {
    _ns = ns;
    _descriptor->setNs(std::move(ns));
}

// ----

bool IndexCatalogEntryImpl::_catalogIsReady(OperationContext* opCtx) const {
    return DurableCatalog::get(opCtx)->isIndexReady(opCtx, _ns, _descriptor->indexName());
}

bool IndexCatalogEntryImpl::_catalogIsPresent(OperationContext* opCtx) const {
    return DurableCatalog::get(opCtx)->isIndexPresent(opCtx, _ns, _descriptor->indexName());
}

bool IndexCatalogEntryImpl::_catalogIsMultikey(OperationContext* opCtx,
                                               MultikeyPaths* multikeyPaths) const {
    return DurableCatalog::get(opCtx)->isIndexMultikey(
        opCtx, _ns, _descriptor->indexName(), multikeyPaths);
}

KVPrefix IndexCatalogEntryImpl::_catalogGetPrefix(OperationContext* opCtx) const {
    return DurableCatalog::get(opCtx)->getIndexPrefix(opCtx, _ns, _descriptor->indexName());
}

}  // namespace mongo
