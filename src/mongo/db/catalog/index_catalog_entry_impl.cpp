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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog_entry_impl.h"

#include <algorithm>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::string;

IndexCatalogEntryImpl::IndexCatalogEntryImpl(OperationContext* const opCtx,
                                             const std::string& ident,
                                             std::unique_ptr<IndexDescriptor> descriptor,
                                             CollectionQueryInfo* const queryInfo,
                                             bool isFrozen)
    : _ident(ident),
      _descriptor(std::move(descriptor)),
      _queryInfo(queryInfo),
      _ordering(Ordering::make(_descriptor->keyPattern())),
      _isReady(false),
      _isFrozen(isFrozen),
      _isDropped(false),
      _prefix(DurableCatalog::get(opCtx)->getIndexPrefix(
          opCtx, _descriptor->getCollection()->getCatalogId(), _descriptor->indexName())) {
    _descriptor->_cachedEntry = this;

    _isReady = _catalogIsReady(opCtx);

    {
        stdx::lock_guard<Latch> lk(_indexMultikeyPathsMutex);
        const bool isMultikey = _catalogIsMultikey(opCtx, &_indexMultikeyPaths);
        _isMultikeyForRead.store(isMultikey);
        _isMultikeyForWrite.store(isMultikey);
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

        _expCtxForFilter = make_intrusive<ExpressionContext>(
            opCtx, CollatorInterface::cloneCollator(_collator.get()), ns());

        // Parsing the partial filter expression is not expected to fail here since the
        // expression would have been successfully parsed upstream during index creation.
        _filterExpression =
            MatchExpressionParser::parseAndNormalize(filter,
                                                     _expCtxForFilter,
                                                     ExtensionsCallbackNoop(),
                                                     MatchExpressionParser::kBanAllSpecialFeatures);
        LOGV2_DEBUG(20350,
                    2,
                    "have filter expression for {ns} {descriptor_indexName} {filter}",
                    "ns"_attr = ns(),
                    "descriptor_indexName"_attr = _descriptor->indexName(),
                    "filter"_attr = redact(filter));
    }
}

IndexCatalogEntryImpl::~IndexCatalogEntryImpl() {
    _descriptor->_cachedEntry = nullptr;  // defensive

    _descriptor.reset();
}

const NamespaceString& IndexCatalogEntryImpl::ns() const {
    return _descriptor->parentNS();
}

void IndexCatalogEntryImpl::init(std::unique_ptr<IndexAccessMethod> accessMethod) {
    invariant(!_accessMethod);
    _accessMethod = std::move(accessMethod);
}

bool IndexCatalogEntryImpl::isReady(OperationContext* opCtx) const {
    // For multi-document transactions, we can open a snapshot prior to checking the
    // minimumSnapshotVersion on a collection.  This means we are unprotected from reading
    // out-of-sync index catalog entries.  To fix this, we uassert if we detect that the
    // in-memory catalog is out-of-sync with the on-disk catalog.
    if (opCtx->inMultiDocumentTransaction()) {
        if (!_catalogIsPresent(opCtx) || _catalogIsReady(opCtx) != _isReady) {
            uasserted(ErrorCodes::SnapshotUnavailable,
                      str::stream() << "Unable to read from a snapshot due to pending collection"
                                       " catalog changes; please retry the operation.");
        }
    }

    if (kDebugBuild)
        invariant(_isReady == _catalogIsReady(opCtx));
    return _isReady;
}

bool IndexCatalogEntryImpl::isFrozen() const {
    invariant(!_isFrozen || !_isReady);
    return _isFrozen;
}

bool IndexCatalogEntryImpl::isMultikey() const {
    return _isMultikeyForRead.load();
}

MultikeyPaths IndexCatalogEntryImpl::getMultikeyPaths(OperationContext* opCtx) const {
    stdx::lock_guard<Latch> lk(_indexMultikeyPathsMutex);
    return _indexMultikeyPaths;
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
    if (!_indexTracksPathLevelMultikeyInfo && _isMultikeyForWrite.load()) {
        // If the index is already set as multikey and we don't have any path-level information to
        // update, then there's nothing more for us to do.
        return;
    }

    if (_indexTracksPathLevelMultikeyInfo) {
        stdx::lock_guard<Latch> lk(_indexMultikeyPathsMutex);
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
        info.nss = ns();
        info.indexName = _descriptor->indexName();
        info.multikeyPaths = paths;
        MultikeyPathTracker::get(opCtx).addMultikeyPathInfo(info);
        return;
    }

    if (opCtx->inMultiDocumentTransaction()) {
        auto status = _setMultikeyInMultiDocumentTransaction(opCtx, paths);
        // Retry without side transaction.
        if (!status.isOK()) {
            _catalogSetMultikey(opCtx, paths);
        }
    } else {
        _catalogSetMultikey(opCtx, paths);
    }
}

Status IndexCatalogEntryImpl::_setMultikeyInMultiDocumentTransaction(
    OperationContext* opCtx, const MultikeyPaths& multikeyPaths) {
    // If we are inside a multi-document transaction, we write the on-disk multikey update in a
    // separate transaction so that it will not generate prepare conflicts with other operations
    // that try to set the multikey flag. In general, it should always be safe to update the
    // multikey flag earlier than necessary, and so we are not concerned with the atomicity of the
    // multikey flag write and the parent transaction. We can do this write separately and commit it
    // before the parent transaction commits.
    auto txnParticipant = TransactionParticipant::get(opCtx);

    TransactionParticipant::SideTransactionBlock sideTxn(opCtx);

    // If the index is not visible within the side transaction, the index may have been created,
    // but not committed, in the parent transaction. Therefore, we abandon the side transaction
    // and set the multikey flag in the parent transaction.
    if (!_catalogIsPresent(opCtx)) {
        return {ErrorCodes::SnapshotUnavailable, "index not visible in side transaction"};
    }

    writeConflictRetry(opCtx, "set index multikey", ns().ns(), [&] {
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
            LOGV2(20352,
                  "Temporarily could not timestamp the multikey catalog write, retrying.",
                  "reason"_attr = status.reason());
            throw WriteConflictException();
        }
        fassert(31164, status);

        _catalogSetMultikey(opCtx, multikeyPaths);

        wuow.commit();
    });

    return Status::OK();
}

// ----

bool IndexCatalogEntryImpl::_catalogIsReady(OperationContext* opCtx) const {
    return DurableCatalog::get(opCtx)->isIndexReady(
        opCtx, _descriptor->getCollection()->getCatalogId(), _descriptor->indexName());
}

bool IndexCatalogEntryImpl::_catalogIsPresent(OperationContext* opCtx) const {
    return DurableCatalog::get(opCtx)->isIndexPresent(
        opCtx, _descriptor->getCollection()->getCatalogId(), _descriptor->indexName());
}

bool IndexCatalogEntryImpl::_catalogIsMultikey(OperationContext* opCtx,
                                               MultikeyPaths* multikeyPaths) const {
    return DurableCatalog::get(opCtx)->isIndexMultikey(opCtx,
                                                       _descriptor->getCollection()->getCatalogId(),
                                                       _descriptor->indexName(),
                                                       multikeyPaths);
}

void IndexCatalogEntryImpl::_catalogSetMultikey(OperationContext* opCtx,
                                                const MultikeyPaths& multikeyPaths) {
    // It's possible that the index type (e.g. ascending/descending index) supports tracking
    // path-level multikey information, but this particular index doesn't.
    // CollectionCatalogEntry::setIndexIsMultikey() requires that we discard the path-level
    // multikey information in order to avoid unintentionally setting path-level multikey
    // information on an index created before 3.4.
    auto indexMetadataHasChanged =
        DurableCatalog::get(opCtx)->setIndexIsMultikey(opCtx,
                                                       _descriptor->getCollection()->getCatalogId(),
                                                       _descriptor->indexName(),
                                                       multikeyPaths);

    // In the absense of using the storage engine to read from the catalog, we must set multikey
    // prior to the storage engine transaction committing.
    //
    // Moreover, there must not be an `onRollback` handler to reset this back to false. Given a long
    // enough pause in processing `onRollback` handlers, a later writer that successfully flipped
    // multikey can be undone. Alternatively, one could use a counter instead of a boolean to avoid
    // that problem.
    _isMultikeyForRead.store(true);
    if (_indexTracksPathLevelMultikeyInfo) {
        stdx::lock_guard<Latch> lk(_indexMultikeyPathsMutex);
        for (size_t i = 0; i < multikeyPaths.size(); ++i) {
            _indexMultikeyPaths[i].insert(multikeyPaths[i].begin(), multikeyPaths[i].end());
        }
    }
    if (indexMetadataHasChanged && _queryInfo) {
        LOGV2_DEBUG(47187005,
                    1,
                    "Index set to multi key, clearing query plan cache",
                    "namespace"_attr = ns(),
                    "keyPattern"_attr = _descriptor->keyPattern());
        _queryInfo->clearQueryCache();
    }

    opCtx->recoveryUnit()->onCommit([this](boost::optional<Timestamp>) {
        // Writers must attempt to flip multikey until it's confirmed a storage engine
        // transaction successfully commits. Only after this point may a writer optimize out
        // flipping multikey.
        _isMultikeyForWrite.store(true);
    });
}

KVPrefix IndexCatalogEntryImpl::_catalogGetPrefix(OperationContext* opCtx) const {
    return DurableCatalog::get(opCtx)->getIndexPrefix(
        opCtx, _descriptor->getCollection()->getCatalogId(), _descriptor->indexName());
}

}  // namespace mongo
