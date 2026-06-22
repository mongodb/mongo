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

#include "mongo/db/shard_role/shard_catalog/index_catalog_entry_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/wildcard_metadata_key.h"
#include "mongo/db/index_builds/index_build_interceptor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/durable_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry_helpers.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/shard_catalog/multikey_path_metrics.h"
#include "mongo/db/shard_role/shard_catalog/set_multikey_metadata_oplog_helpers.h"
#include "mongo/db/shard_role/shard_catalog/txn_wildcard_multikey_paths.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

namespace {

mongo::BSONObj multikeyPathsToBSON(const mongo::BSONObj& keyPattern,
                                   const mongo::MultikeyPaths& multikeyPaths) {
    // Multikey paths can be empty. Examples: legacy index formats that do not support path-level
    // multikeyness, FTS indices, or wildcard indices. The latter embeds multikey paths directly in
    // the index table, and uses this code path only for setting the global "is multikey" catalog
    // flag.
    return multikeyPaths.empty() ? mongo::BSONObj()
                                 : mongo::multikey_paths::serialize(keyPattern, multikeyPaths);
}

}  // namespace

namespace mongo {

MONGO_FAIL_POINT_DEFINE(skipUpdateIndexMultikey);

IndexCatalogEntryImpl::IndexCatalogEntryImpl(OperationContext* const opCtx,
                                             const CollectionPtr& collection,
                                             const std::string& ident,
                                             IndexDescriptor&& descriptor,
                                             bool isFrozen)
    : _shared(make_intrusive<SharedState>(ident, collection->getCatalogId())),
      _descriptor(descriptor),
      _isReady(false),
      _isFrozen(isFrozen),
      _shouldValidateDocument(false),
      _indexOffset(invariantStatusOK(
          collection->checkMetaDataForIndex(_descriptor.indexName(), _descriptor.infoObj()))) {
    _isReady = collection->isIndexReady(_descriptor.indexName());

    // For time-series collections, we need to check that the indexed metric fields do not have
    // expanded array values.
    _shouldValidateDocument =
        collection->getTimeseriesOptions() &&
        timeseries::doesBucketsIndexIncludeMeasurement(
            opCtx, collection->ns(), *collection->getTimeseriesOptions(), _descriptor.infoObj());

    const BSONObj& collation = _descriptor.collation();
    if (!collation.isEmpty()) {
        auto statusWithCollator =
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation);

        // Index spec should have already been validated.
        invariant(statusWithCollator.getStatus());

        _shared->_collator = std::move(statusWithCollator.getValue());
    }

    if (_descriptor.isPartial()) {
        const BSONObj& filter = _descriptor.partialFilterExpression();
        _shared->_expCtxForFilter =
            ExpressionContextBuilder{}
                .opCtx(opCtx)
                .collator(CollatorInterface::cloneCollator(_shared->_collator.get()))
                .ns(collection->ns())
                .build();
        // Parsing the partial filter expression is not expected to fail here since the
        // expression would have been successfully parsed upstream during index creation.
        _shared->_filterExpression =
            MatchExpressionParser::parseAndNormalize(filter,
                                                     _shared->_expCtxForFilter,
                                                     ExtensionsCallbackNoop(),
                                                     MatchExpressionParser::kBanAllSpecialFeatures);
        LOGV2_DEBUG(20350,
                    2,
                    "have filter expression for {namespace} {indexName} {filter}",
                    logAttrs(collection->ns()),
                    "indexName"_attr = _descriptor.indexName(),
                    "filter"_attr = redact(filter));
    }
}

IndexCatalogEntryImpl::~IndexCatalogEntryImpl() = default;

void IndexCatalogEntryImpl::setAccessMethod(std::unique_ptr<IndexAccessMethod> accessMethod) {
    invariant(!_shared->_accessMethod);
    _shared->_accessMethod = std::move(accessMethod);
    index_catalog_helpers::computeUpdateIndexData(
        this, _shared->_accessMethod.get(), &_shared->_indexedPaths);
}

bool IndexCatalogEntryImpl::isFrozen() const {
    invariant(!_isFrozen || !_isReady);
    return _isFrozen;
}

bool IndexCatalogEntryImpl::shouldValidateDocument() const {
    return _shouldValidateDocument;
}

bool IndexCatalogEntryImpl::isMultikey(OperationContext* const opCtx,
                                       const CollectionPtr& collection) const {
    return _catalogIsMultikey(opCtx, collection, nullptr);
}

MultikeyPaths IndexCatalogEntryImpl::getMultikeyPaths(OperationContext* opCtx,
                                                      const CollectionPtr& collection) const {
    MultikeyPaths indexMultikeyPathsForRead;
    [[maybe_unused]] const bool isMultikeyInCatalog =
        _catalogIsMultikey(opCtx, collection, &indexMultikeyPathsForRead);
    return indexMultikeyPathsForRead;
}

// ---

void IndexCatalogEntryImpl::setIsReady(bool newIsReady) {
    _isReady = newIsReady;
}

void IndexCatalogEntryImpl::setIsFrozen(bool newIsFrozen) {
    _isFrozen = newIsFrozen;
}

void IndexCatalogEntryImpl::setMultikey(OperationContext* opCtx,
                                        const CollectionPtr& collection,
                                        const KeyStringSet& multikeyMetadataKeys,
                                        const MultikeyPaths& multikeyPaths) const {
    // An index can either track path-level multikey information in the catalog or as metadata keys
    // in the index itself, but not both.
    MultikeyPaths indexMultikeyPathsForWrite;
    auto isMultikeyForWrite = _catalogIsMultikey(opCtx, collection, &indexMultikeyPathsForWrite);
    auto indexTracksMultikeyPathsInCatalog = !indexMultikeyPathsForWrite.empty();
    auto& multikeyPathTracker = MultikeyPathTracker::get(opCtx);
    invariant(!(indexTracksMultikeyPathsInCatalog && multikeyMetadataKeys.size() > 0));
    // If the index is already set as multikey and we don't have any path-level information to
    // update, then there's nothing more for us to do.
    bool hasNoPathLevelInfo = (!indexTracksMultikeyPathsInCatalog && multikeyMetadataKeys.empty());
    if (hasNoPathLevelInfo && isMultikeyForWrite) {
        return;
    }

    if (indexTracksMultikeyPathsInCatalog) {
        invariant(multikeyPaths.size() == indexMultikeyPathsForWrite.size());

        bool newPathIsMultikey = false;
        for (size_t i = 0; i < multikeyPaths.size(); ++i) {
            if (!std::includes(indexMultikeyPathsForWrite[i].begin(),
                               indexMultikeyPathsForWrite[i].end(),
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

    if (MONGO_unlikely(skipUpdateIndexMultikey.shouldFail())) {
        return;
    }

    MultikeyPaths paths = indexTracksMultikeyPathsInCatalog ? multikeyPaths : MultikeyPaths{};

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
    if (multikeyPathTracker.isTrackingMultikeyPathInfo()) {
        auto earliestTimestamp =
            shard_role_details::getRecoveryUnit(opCtx)->getTimestamp().value_or(Timestamp());
        multikeyPathTracker.addMultikeyPathInfo({collection->ns(),
                                                 collection->uuid(),
                                                 _descriptor.indexName(),
                                                 multikeyMetadataKeys,
                                                 std::move(paths),
                                                 earliestTimestamp});
        return;
    }

    // `multikeyMetadataKeys` is wildcard-only by construction: only wildcard indexes store
    // multikey-path information as keys inside the index itself. Other index types track multikey
    // paths exclusively in the catalog and always pass an empty set here. Violating this would
    // attempt to insert metadata keys into a non-wildcard index, which has no reserved RecordId for
    // them and risks index corruption.
    invariant(multikeyMetadataKeys.empty() ||
                  _descriptor.getIndexType() == IndexType::INDEX_WILDCARD,
              "Non-empty multikeyMetadataKeys must come from a wildcard index");

    if (!opCtx->inMultiDocumentTransaction()) {
        _insertWildcardMultikeyMetadataKeysAndCountNew(opCtx, collection, multikeyMetadataKeys);
        _catalogSetMultikey(opCtx, collection, paths);
        return;
    }

    VersionContext::FixedOperationFCVRegion fixedOfcvRegion(opCtx);
    const bool replicateMultikeyness = gFeatureFlagReplicateMultikeynessInTransactions.isEnabled(
        VersionContext::getDecoration(opCtx));
    const bool isWildcardMultikey = !multikeyMetadataKeys.empty();

    // Feature off, insert metadata keys in transaction itself. Otherwise the keys are written in
    // the side txn.
    if (!replicateMultikeyness && isWildcardMultikey) {
        const auto newPathCount =
            _insertWildcardMultikeyMetadataKeysAndCountNew(opCtx, collection, multikeyMetadataKeys);
        if (newPathCount == 0) {
            // Every metadata key was already present in the parent transaction, so these paths
            // are already multikey. There is nothing to write in a side transaction.
            return;
        }
    }

    // replicateMultikeyness==false: writes catalog changes in a side txn, uses no-op entry.
    // replicateMultikeyness==true: writes catalog changes + wildcard metadata keys in a side txn,
    // uses setMultikeyMetadata entry.
    auto status = _setMultikeyInMultiDocumentTransaction(
        opCtx, collection, multikeyMetadataKeys, paths, replicateMultikeyness);

    if (!status.isOK()) {
        // The only expected failure is IndexNotFound — the index was created in the same
        // parent transaction and is not yet visible to the side transaction's snapshot.
        // Any other error is unexpected and should not be silently swallowed.
        tassert(11609105,
                str::stream() << "Unexpected error from side transaction for multikey update: "
                              << status.toString(),
                status.code() == ErrorCodes::IndexNotFound);

        // Fallback: the side transaction could not see the index because it was created (but not
        // yet committed) in the parent transaction. This happens when a user creates a collection
        // with a wildcard index and inserts data in the same multi-document transaction. Since the
        // side transaction was abandoned, we insert the metadata keys directly in the parent
        // transaction instead.
        //
        // No setMultikeyMetadata oplog entry is emitted. On the secondary, the MultikeyPathTracker
        // collects multikey info during oplog application and flushes it at the batch timestamp
        // (firstTimeInBatch). This is still timestamp-consistent because transactions containing
        // DDL commands (create, createIndexes) are always processed in their own oplog applier
        // batch, so firstTimeInBatch == T_commit for these transactions.
        if (replicateMultikeyness) {
            _insertWildcardMultikeyMetadataKeysAndCountNew(opCtx, collection, multikeyMetadataKeys);
        }
        _catalogSetMultikey(opCtx, collection, paths);
    } else if (replicateMultikeyness && isWildcardMultikey) {
        // Side transaction succeeded. The metadata keys were committed on a side RU and are
        // invisible to the parent RU's snapshot, so populate the per-snapshot RYOW cache. The
        // fallback branch above writes directly to the parent RU and needs no cache entry.

        // TODO (SERVER-128058): avoid path extraction from metadata keys, this was already
        // performed upstream, pipe through.
        TxnWildcardMultikeyPaths::get(opCtx).append(
            collection->uuid(),
            _descriptor.indexName(),
            extractWildcardMultikeyPathsFromMetadataKeys(multikeyMetadataKeys,
                                                         _descriptor.ordering()));
    }
}

void IndexCatalogEntryImpl::setMultikeyForApplyOps(OperationContext* opCtx,
                                                   const CollectionPtr& coll,
                                                   const KeyStringSet& multikeyMetadataKeys,
                                                   const MultikeyPaths& multikeyPaths) const {
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(coll->ns(), MODE_IX));
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    // Insert wildcard metadata keys into the index if provided.
    _insertWildcardMultikeyMetadataKeysAndCountNew(opCtx, coll, multikeyMetadataKeys);

    opCtx->getClient()->getServiceContext()->getOpObserver()->onSetMultikeyMetadata(
        opCtx,
        coll->ns(),
        descriptor()->indexName(),
        multikeyPathsToBSON(descriptor()->keyPattern(), multikeyPaths));

    _catalogSetMultikey(opCtx, coll, multikeyPaths);
}

void IndexCatalogEntryImpl::forceSetMultikey(OperationContext* const opCtx,
                                             const CollectionPtr& coll,
                                             bool isMultikey,
                                             const MultikeyPaths& multikeyPaths) const {
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(coll->ns(), MODE_X));

    // Don't check _indexTracksMultikeyPathsInCatalog because the caller may be intentionally trying
    // to bypass this check. That is, pre-3.4 indexes may be 'stuck' in a state where they are not
    // tracking multikey paths in the catalog (i.e. the multikeyPaths field is absent), but the
    // caller wants to upgrade this index because it knows exactly which paths are multikey. We rely
    // on the following function to make sure this upgrade only takes place on index types that
    // currently support path-level multikey path tracking.
    coll->forceSetIndexIsMultikey(opCtx, &_descriptor, isMultikey, multikeyPaths);

    // Since multikey metadata has changed, invalidate the query cache and rebuild PathArrayness.
    auto& collectionQueryInfo = CollectionQueryInfo::get(coll);
    collectionQueryInfo.clearQueryCacheForSetMultikey(coll);
    if (feature_flags::gFeatureFlagPathArrayness.isEnabled()) {
        collectionQueryInfo.updatePathArraynessForSetMultikey(_descriptor, multikeyPaths);
    }
}

Status IndexCatalogEntryImpl::_setMultikeyInMultiDocumentTransaction(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const KeyStringSet& multikeyMetadataKeys,
    const MultikeyPaths& multikeyPaths,
    bool replicateMultikeyness) const {
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
    if (!durable_catalog::isIndexPresent(
            opCtx, _shared->_catalogId, _descriptor.indexName(), MDBCatalog::get(opCtx))) {
        return {ErrorCodes::IndexNotFound,
                str::stream() << "index '" << _descriptor.indexName()
                              << "' not visible in side transaction — likely created in the "
                                 "same multi-document transaction"};
    }

    writeConflictRetry(opCtx, "set index multikey", collection->ns(), [&] {
        WriteUnitOfWork wuow(opCtx);

        // If we have a prepare optime for recovery, then we always use that. This is safe since
        // the prepare timestamp is always <= the commit timestamp of a transaction, which
        // satisfies the correctness requirement for multikey writes i.e. they must occur at or
        // before the first write that set the multikey flag. This only occurs when
        // reconstructing prepared transactions, and not during replication recovery oplog
        // application.
        auto recoveryPrepareOpTime = txnParticipant.getPrepareOpTimeForRecovery();
        if (!recoveryPrepareOpTime.isNull()) {
            if (replicateMultikeyness) {
                // Start-up recovery and replication rollback never expect to hit this path.
                // Recovery either has the relevant multikey metadata persisted in the stable
                // checkpoint it's based off, or replays the setMultikeyMetadata oplog entry,
                // so this replayed transaction does not ever need to make any multikey metadata
                // write.
                invariant(!InReplicationRecovery::isSet(opCtx->getServiceContext()));

                // Because prepared transactions are never reconstructed during replication
                // recovery, the invariant above means that we must be in logical initial sync.
                // Logical initial sync is therefore the only path where a reconstructed transaction
                // violates the timestamp consistency guarantees: the multikey metadata can be set
                // in the context of the reconstructed prepared transaction, without replicating the
                // setMultikeyMetadata oplog entry, therefore using a different timestamp than the
                // rest of the replicas.
            }

            // We might replay a prepared transaction behind the oldest timestamp during initial
            // sync (or behind the stable timestamp during rollback if
            // FeatureFlagReplicateMultikeynessInTransactions is disabled).
            //
            // During initial sync, we may not have a stable timestamp. Therefore, we need to round
            // up the multi-key write timestamp to the max of the three so that we don't write
            // behind the oldest/stable timestamp. This code path is only hit during initial
            // sync (and recovery if FeatureFlagReplicateMultikeynessInTransactions is disabled)
            // when reconstructing prepared transactions, and so we don't expect the oldest/stable
            // timestamp to advance concurrently.
            //
            // WiredTiger disallows committing at the stable timestamp to avoid confusion during
            // checkpoints, to overcome that we allow setting the timestamp slightly after the
            // prepared timestamp of the original transaction. This is currently not an issue as
            // the index metadata state is read from in-memory cache and this is modifying the
            // state on-disk from the _mdb_catalog document. To put in other words, multikey
            // doesn't go backwards. This would be a problem if we move to a versioned catalog
            // world as a different transaction could choose an earlier timestamp (i.e. the
            // original transaction timestamp) and encounter an invalid system state where the
            // document that enables multikey hasn't enabled it yet but is present in the
            // collection. In other words, the index is not set for multikey but there is
            // already data present that relies on it.
            auto status = shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(std::max(
                {recoveryPrepareOpTime.getTimestamp(),
                 opCtx->getServiceContext()->getStorageEngine()->getOldestTimestamp(),
                 opCtx->getServiceContext()->getStorageEngine()->getStableTimestamp() + 1}));
            fassert(31164, status);
        } else {
            // If there is no recovery prepare OpTime, then this node must be a primary.
            invariant(opCtx->writesAreReplicated());

            if (replicateMultikeyness) {
                BSONObj pathsObj;
                if (!multikeyMetadataKeys.empty()) {
                    // Wildcard index: serialize field path names extracted from metadata keys.
                    auto fieldPaths =
                        set_multikey_metadata_oplog_helpers::extractFieldPathsFromMetadataKeys(
                            multikeyMetadataKeys, descriptor()->ordering());
                    pathsObj = set_multikey_metadata_oplog_helpers::fieldPathsToBSON(fieldPaths);
                    if (_insertWildcardMultikeyMetadataKeysAndCountNew(
                            opCtx, collection, multikeyMetadataKeys) == 0) {
                        // Every metadata key was already present, so these paths are already
                        // multikey and replicated. There is nothing to write or replicate, so roll
                        // back this empty side transaction rather than committing a no-op.
                        return;
                    }
                } else {
                    // Regular index: existing multikey paths format.
                    pathsObj = multikeyPathsToBSON(descriptor()->keyPattern(), multikeyPaths);
                }

                opCtx->getClient()->getServiceContext()->getOpObserver()->onSetMultikeyMetadata(
                    opCtx, collection->ns(), descriptor()->indexName(), pathsObj);
            } else {
                // Write a noop oplog entry to get a properly ordered timestamp.
                auto msg = BSON("msg" << "Setting index to multikey"
                                      << "coll"
                                      << NamespaceStringUtil::serializeForCatalog(collection->ns())
                                      << "index" << _descriptor.indexName());
                opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(opCtx, msg);
            }
        }

        _catalogSetMultikey(opCtx, collection, multikeyPaths);

        wuow.commit();
        catalog_metrics::recordSideTransaction();
    });

    return Status::OK();
}

std::shared_ptr<Ident> IndexCatalogEntryImpl::getSharedIdent() const {
    return _shared->_accessMethod ? _shared->_accessMethod->getSharedIdent() : nullptr;
}

const Ordering& IndexCatalogEntryImpl::ordering() const {
    return _descriptor.ordering();
}

void IndexCatalogEntryImpl::setIdent(std::shared_ptr<Ident> newIdent) {
    if (!_shared->_accessMethod)
        return;
    _shared->_accessMethod->setIdent(std::move(newIdent));
}

namespace {

class NormalizedIndexCatalogEntry : public IndexCatalogEntry {
public:
    NormalizedIndexCatalogEntry(OperationContext* opCtx,
                                const CollectionPtr& collection,
                                const IndexCatalogEntry* entry)
        : IndexCatalogEntry(),
          _original(entry),
          _indexDescriptor([&] {
              auto desc = entry->descriptor();
              auto normalizedSpec =
                  IndexCatalog::normalizeIndexSpec(opCtx, collection, desc->infoObj());
              return IndexDescriptor{desc->getAccessMethodName(), std::move(normalizedSpec)};
          }()),
          _collator([&]() -> std::unique_ptr<CollatorInterface> {
              const auto& collation = _indexDescriptor.collation();
              if (!entry->getCollator() && !collation.isEmpty()) {
                  auto statusWithCollator =
                      CollatorFactoryInterface::get(opCtx->getServiceContext())
                          ->makeFromBSON(collation);

                  invariantStatusOK(statusWithCollator.getStatus());
                  // Index spec should have already been validated.
                  return std::move(statusWithCollator.getValue());
              }
              return nullptr;
          }()) {}

    NormalizedIndexCatalogEntry(const NormalizedIndexCatalogEntry& other)
        : IndexCatalogEntry(other),
          _original(other._original),
          _indexDescriptor(other._indexDescriptor),
          _collator(other._collator->clone()) {}

    const std::string& getIdent() const final {
        return _original->getIdent();
    }

    std::shared_ptr<Ident> getSharedIdent() const final {
        return _original->getSharedIdent();
    }

    void setIdent(std::shared_ptr<Ident> newIdent) final {
        MONGO_UNIMPLEMENTED_TASSERT(10083543);
    }

    IndexDescriptor* descriptor() final {
        MONGO_UNIMPLEMENTED_TASSERT(10083544);
    }

    const IndexDescriptor* descriptor() const final {
        return &_indexDescriptor;
    }

    IndexAccessMethod* accessMethod() const final {
        return _original->accessMethod();
    }

    void setAccessMethod(std::unique_ptr<IndexAccessMethod> accessMethod) final {
        MONGO_UNIMPLEMENTED_TASSERT(10083545);
    }

    std::shared_ptr<IndexBuildInterceptor> indexBuildInterceptor() const final {
        return _original->indexBuildInterceptor();
    }

    void setIndexBuildInterceptor(std::shared_ptr<IndexBuildInterceptor> interceptor) final {
        MONGO_UNIMPLEMENTED_TASSERT(10083546);
    }

    const Ordering& ordering() const final {
        return _original->ordering();
    }

    const MatchExpression* getFilterExpression() const final {
        return _original->getFilterExpression();
    }

    const CollatorInterface* getCollator() const final {
        return _collator ? _collator.get() : _original->getCollator();
    }

    NamespaceString getNSSFromCatalog(OperationContext* opCtx) const final {
        return _original->getNSSFromCatalog(opCtx);
    }

    void setIsReady(bool newIsReady) final {
        MONGO_UNIMPLEMENTED_TASSERT(10083547);
    }

    void setIsFrozen(bool newIsFrozen) final {
        MONGO_UNIMPLEMENTED_TASSERT(10083548);
    }

    bool isMultikey(OperationContext* opCtx, const CollectionPtr& collection) const final {
        return _original->isMultikey(opCtx, collection);
    }

    MultikeyPaths getMultikeyPaths(OperationContext* opCtx,
                                   const CollectionPtr& collection) const final {
        return _original->getMultikeyPaths(opCtx, collection);
    }

    void setMultikey(OperationContext* opCtx,
                     const CollectionPtr& coll,
                     const KeyStringSet& multikeyMetadataKeys,
                     const MultikeyPaths& multikeyPaths) const final {
        return _original->setMultikey(opCtx, coll, multikeyMetadataKeys, multikeyPaths);
    }

    void setMultikeyForApplyOps(OperationContext* opCtx,
                                const CollectionPtr& coll,
                                const KeyStringSet& multikeyMetadataKeys,
                                const MultikeyPaths& multikeyPaths) const final {
        return _original->setMultikeyForApplyOps(opCtx, coll, multikeyMetadataKeys, multikeyPaths);
    }

    void forceSetMultikey(OperationContext* opCtx,
                          const CollectionPtr& coll,
                          bool isMultikey,
                          const MultikeyPaths& multikeyPaths) const final {
        return _original->forceSetMultikey(opCtx, coll, isMultikey, multikeyPaths);
    }

    bool isReady() const final {
        return _original->isReady();
    }

    bool isFrozen() const final {
        return _original->isFrozen();
    }

    bool shouldValidateDocument() const final {
        return _original->shouldValidateDocument();
    }

    const UpdateIndexData& getIndexedPaths() const final {
        return _original->getIndexedPaths();
    }

    std::unique_ptr<const IndexCatalogEntry> getNormalizedEntry(
        OperationContext* opCtx, const CollectionPtr& coll) const final {
        return std::make_unique<NormalizedIndexCatalogEntry>(*this);
    };

    std::unique_ptr<const IndexCatalogEntry> cloneWithDifferentDescriptor(
        IndexDescriptor descriptor) const final {
        MONGO_UNIMPLEMENTED_TASSERT(10083549);
    }

private:
    const IndexCatalogEntry* _original;
    IndexDescriptor _indexDescriptor;
    std::unique_ptr<CollatorInterface> _collator;
};

class WithDifferentIndexDescriptorEntry : public IndexCatalogEntry {
public:
    WithDifferentIndexDescriptorEntry(IndexDescriptor descriptor, const IndexCatalogEntry* entry)
        : IndexCatalogEntry(), _original(entry), _indexDescriptor(std::move(descriptor)) {}

    const std::string& getIdent() const final {
        return _original->getIdent();
    }

    std::shared_ptr<Ident> getSharedIdent() const final {
        return _original->getSharedIdent();
    }

    void setIdent(std::shared_ptr<Ident> newIdent) final {
        MONGO_UNIMPLEMENTED_TASSERT(10083550);
    }

    IndexDescriptor* descriptor() final {
        MONGO_UNIMPLEMENTED_TASSERT(10083551);
    }

    const IndexDescriptor* descriptor() const final {
        return &_indexDescriptor;
    }

    IndexAccessMethod* accessMethod() const final {
        return _original->accessMethod();
    }

    void setAccessMethod(std::unique_ptr<IndexAccessMethod> accessMethod) final {
        MONGO_UNIMPLEMENTED_TASSERT(10083552);
    }

    std::shared_ptr<IndexBuildInterceptor> indexBuildInterceptor() const final {
        return _original->indexBuildInterceptor();
    }

    void setIndexBuildInterceptor(std::shared_ptr<IndexBuildInterceptor> interceptor) final {
        MONGO_UNIMPLEMENTED_TASSERT(10083553);
    }

    const Ordering& ordering() const final {
        return _original->ordering();
    }

    const MatchExpression* getFilterExpression() const final {
        return _original->getFilterExpression();
    }

    const CollatorInterface* getCollator() const final {
        return _original->getCollator();
    }

    NamespaceString getNSSFromCatalog(OperationContext* opCtx) const final {
        return _original->getNSSFromCatalog(opCtx);
    }

    void setIsReady(bool newIsReady) final {
        MONGO_UNIMPLEMENTED_TASSERT(10083554);
    }

    void setIsFrozen(bool newIsFrozen) final {
        MONGO_UNIMPLEMENTED_TASSERT(10083555);
    }

    bool isMultikey(OperationContext* opCtx, const CollectionPtr& collection) const final {
        return _original->isMultikey(opCtx, collection);
    }

    MultikeyPaths getMultikeyPaths(OperationContext* opCtx,
                                   const CollectionPtr& collection) const final {
        return _original->getMultikeyPaths(opCtx, collection);
    }

    void setMultikey(OperationContext* opCtx,
                     const CollectionPtr& coll,
                     const KeyStringSet& multikeyMetadataKeys,
                     const MultikeyPaths& multikeyPaths) const final {
        return _original->setMultikey(opCtx, coll, multikeyMetadataKeys, multikeyPaths);
    }

    void setMultikeyForApplyOps(OperationContext* opCtx,
                                const CollectionPtr& coll,
                                const KeyStringSet& multikeyMetadataKeys,
                                const MultikeyPaths& multikeyPaths) const final {
        return _original->setMultikeyForApplyOps(opCtx, coll, multikeyMetadataKeys, multikeyPaths);
    }

    void forceSetMultikey(OperationContext* opCtx,
                          const CollectionPtr& coll,
                          bool isMultikey,
                          const MultikeyPaths& multikeyPaths) const final {
        return _original->forceSetMultikey(opCtx, coll, isMultikey, multikeyPaths);
    }

    bool isReady() const final {
        return _original->isReady();
    }

    bool isFrozen() const final {
        return _original->isFrozen();
    }

    bool shouldValidateDocument() const final {
        return _original->shouldValidateDocument();
    }

    const UpdateIndexData& getIndexedPaths() const final {
        return _original->getIndexedPaths();
    }

    std::unique_ptr<const IndexCatalogEntry> getNormalizedEntry(
        OperationContext* opCtx, const CollectionPtr& coll) const final {
        MONGO_UNIMPLEMENTED_TASSERT(10083556);
    };

    std::unique_ptr<const IndexCatalogEntry> cloneWithDifferentDescriptor(
        IndexDescriptor descriptor) const final {
        return std::make_unique<WithDifferentIndexDescriptorEntry>(std::move(descriptor),
                                                                   _original);
    }

private:
    const IndexCatalogEntry* _original;
    IndexDescriptor _indexDescriptor;
};

}  // namespace

std::unique_ptr<const IndexCatalogEntry> IndexCatalogEntryImpl::getNormalizedEntry(
    OperationContext* opCtx, const CollectionPtr& coll) const {
    return std::make_unique<NormalizedIndexCatalogEntry>(opCtx, coll, this);
}

std::unique_ptr<const IndexCatalogEntry> IndexCatalogEntryImpl::cloneWithDifferentDescriptor(
    IndexDescriptor descriptor) const {
    return std::make_unique<WithDifferentIndexDescriptorEntry>(std::move(descriptor), this);
}

// ----

NamespaceString IndexCatalogEntryImpl::getNSSFromCatalog(OperationContext* opCtx) const {
    return MDBCatalog::get(opCtx)->getNSSFromCatalog(opCtx, _shared->_catalogId);
}

bool IndexCatalogEntryImpl::_catalogIsMultikey(OperationContext* opCtx,
                                               const CollectionPtr& collection,
                                               MultikeyPaths* multikeyPaths) const {
    return collection->isIndexMultikey(opCtx, _descriptor.indexName(), multikeyPaths, _indexOffset);
}

int64_t IndexCatalogEntryImpl::_insertWildcardMultikeyMetadataKeysAndCountNew(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const KeyStringSet& multikeyMetadataKeys) const {
    if (multikeyMetadataKeys.empty()) {
        return 0;
    }

    // All metadata keys are indexed against a single RecordId. Duplicate keys are skipped as
    // no-ops and reported via numSkipped.
    int64_t numSkipped = 0;
    uassertStatusOK(
        accessMethod()->asSortedData()->insertKeys(opCtx,
                                                   *shard_role_details::getRecoveryUnit(opCtx),
                                                   collection,
                                                   this,
                                                   multikeyMetadataKeys,
                                                   {},
                                                   {},
                                                   nullptr,
                                                   &numSkipped));

    const auto newPathCount = static_cast<int64_t>(multikeyMetadataKeys.size()) - numSkipped;
    invariant(newPathCount >= 0);
    if (newPathCount > 0) {
        catalog_metrics::recordWildcardMultikeyPathChanges(opCtx, newPathCount);
    }
    return newPathCount;
}

void IndexCatalogEntryImpl::_catalogSetMultikey(OperationContext* opCtx,
                                                const CollectionPtr& collection,
                                                const MultikeyPaths& multikeyPaths) const {
    // It's possible that the index type (e.g. ascending/descending index) supports tracking
    // path-level multikey information, but this particular index doesn't.
    // CollectionCatalogEntry::setIndexIsMultikey() requires that we discard the path-level
    // multikey information in order to avoid unintentionally setting path-level multikey
    // information on an index created before 3.4.
    const auto newPathComponents =
        collection->setIndexIsMultikey(opCtx, _descriptor.indexName(), multikeyPaths, _indexOffset);

    if (newPathComponents > 0) {
        if (!multikeyPaths.empty()) {
            catalog_metrics::recordOrdinaryMultikeyPathChanges(opCtx, newPathComponents);
        }
        LOGV2_DEBUG(4718705,
                    1,
                    "Index set to multi key, clearing query plan cache",
                    logAttrs(collection->ns()),
                    "keyPattern"_attr = _descriptor.keyPattern());
        auto& collectionQueryInfo = CollectionQueryInfo::get(collection);
        collectionQueryInfo.clearQueryCacheForSetMultikey(collection);
        if (feature_flags::gFeatureFlagPathArrayness.isEnabled()) {
            collectionQueryInfo.updatePathArraynessForSetMultikey(_descriptor, multikeyPaths);
        }
    }
}

}  // namespace mongo
