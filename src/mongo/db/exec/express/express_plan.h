/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/classic/projection.h"
#include "mongo/db/exec/classic/update_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/document_validation.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/plan_explainer_express.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/update/update_util.h"

#include <type_traits>
#include <utility>
#include <variant>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>


namespace mongo {
namespace express {

/**
 * We encountered a situation where the record referenced by the index entry is gone. Check whether
 * or not this read is ignoring prepare conflicts, then log and/or error appropriately.
 */
void logRecordNotFound(OperationContext* opCtx,
                       const RecordId& rid,
                       const BSONObj& indexKey,
                       const BSONObj& keyPattern,
                       const NamespaceString& ns);

/**
 * The 'PlanProgress' variant (defined below) represents the possible return values from an
 * execution step in 'ExpressPlan' execution. It holds one of the following results.
 */

/**
 * Execution made forward progress.
 */
class Ready {
public:
    static constexpr bool indicatesSuccessfulProgress = true;
};

/**
 * Execution did not make forward progress, possibly because of a write conflict, and the caller
 * should yield, back off as necessary, and try again.
 */
class WaitingForYield {
public:
    static constexpr bool indicatesSuccessfulProgress = false;
};

/**
 * Execution did not make forward progress because of a 'TemporarilyUnavailable' exception. The
 * caller should yield, back off as necessary, and try again.
 */
class WaitingForBackoff {
public:
    static constexpr bool indicatesSuccessfulProgress = false;
};

/**
 * Execution did not make forward progress because it is blocked on a condition. The caller should
 * yield, wait for the condition to signal, and try again.
 */
class WaitingForCondition {
public:
    WaitingForCondition(SharedSemiFuture<void> waitSignal) : _waitSignal(std::move(waitSignal)) {}

    const SharedSemiFuture<void>& waitSignal() const {
        return _waitSignal;
    }

    static constexpr bool indicatesSuccessfulProgress = false;

private:
    SharedSemiFuture<void> _waitSignal;
};

/**
 * Execution completed successfully, and there is no remaining work. Note that execution can
 * return 'Exhausted' when it produces its last document.
 *
 * It is safe to make additional calls to the 'ExpressPlan::proceed' function, but they will always
 * return 'Exhausted' and will never produce any more documents.
 */
class Exhausted {
public:
    static constexpr bool indicatesSuccessfulProgress = true;
};

using PlanProgress =
    std::variant<Ready, WaitingForYield, WaitingForBackoff, WaitingForCondition, Exhausted>;

inline bool isSuccessfulResult(const PlanProgress& result) {
    return std::visit([](const auto& value) { return value.indicatesSuccessfulProgress; }, result);
}

/**
 * When a read or write operation generates a possibly recoverable exception (e.g.,
 * WriteConflictException), an 'ExpressPlan' forwards it to its 'ExceptionRecoveryPolicy' to provide
 * a 'PlanProgress' result that will ultimately communicate to the executor what it should do before
 * trying to resume execution. If execution should terminate, the policy can rethrow the exception,
 * which makes it a query-fatal error.
 */
class ExceptionRecoveryPolicy {
public:
    virtual PlanProgress recoverIfPossible(ExceptionFor<ErrorCodes::WriteConflict>&) const = 0;
    virtual PlanProgress recoverIfPossible(
        ExceptionFor<ErrorCodes::TemporarilyUnavailable>&) const = 0;
    virtual PlanProgress recoverIfPossible(
        ExceptionFor<ErrorCodes::TransactionTooLargeForCache>&) const = 0;
    virtual express::PlanProgress recoverIfPossible(
        ExceptionFor<ErrorCodes::StaleConfig>& exception) const = 0;
};

/**
 * An adapter that converts T to boost::optional<T> iff T is not default constructible. This
 * transform is useful for the "iterator" classes, which need to store a CollectionType object as a
 * member but don't initialize it until a call to the open() function. When CollectionType is not
 * default constructible, the iterator can store a boost::optional<CollectionType> that stores
 * boost::none until it gets initialized by the open() function.
 */
template <class T>
struct WrapInOptionalIfNeeded {
    using type = boost::optional<T>;
};

template <class T>
requires(std::is_default_constructible_v<T>)
struct WrapInOptionalIfNeeded<T> {
    using type = T;
};

/**
 * The overloads below provide ways for the iterator classes to interact with a CollectionType
 * object that may be either a CollectionAcquisition or a CollectionPtr.
 *
 * TODO SERVER-76397: Once all PlanExecutors use CollectionAcquisition exclusively, these adapaters
 * won't be necessary.
 */
inline const CollectionAcquisition& unwrapCollection(
    const boost::optional<CollectionAcquisition>& collectionAcquisition) {
    tassert(8375913,
            "Access to invalided plan: plan is not yet open or is yielded",
            collectionAcquisition.has_value());
    return *collectionAcquisition;
}

inline const CollectionPtr* unwrapCollection(const CollectionPtr* collectionPtr) {
    tassert(8375912,
            "Access to invalided plan: plan is not yet open or is yielded",
            collectionPtr != nullptr);
    return collectionPtr;
}

inline const Collection& accessCollection(const CollectionPtr* collectionPtr) {
    return *collectionPtr->get();
}

inline const Collection& accessCollection(const CollectionAcquisition& collectionAcquisition) {
    return *collectionAcquisition.getCollectionPtr().get();
}

inline const CollectionPtr& accessCollectionPtr(const CollectionPtr* collectionPtr) {
    return *collectionPtr;
}

inline const CollectionPtr& accessCollectionPtr(
    const CollectionAcquisition& collectionAcquisition) {
    return collectionAcquisition.getCollectionPtr();
}

inline void prepareForCollectionInvalidation(CollectionPtr const*& referenceToCollectionPtr) {
    // Any caller holding a raw pointer to a CollectionPtr should destroy that pointer before any
    // operation that can invalidate it in order to avoid the bad practice of storing a dangling
    // pointer.
    referenceToCollectionPtr = nullptr;
}

inline void prepareForCollectionInvalidation(const boost::optional<CollectionAcquisition>&) {
    // When collection resources are released, an associated CollectionAcquisition that was valid
    // before the release becomes valid again after the collection is restored, so it is safe to
    // leave the CollectionAcquisition as is.
}

inline void checkRestoredCollection(OperationContext* opCtx,
                                    const CollectionPtr& collection,
                                    const UUID& expectedUUID,
                                    const NamespaceString& expectedNss) {

    if (!collection) {
        auto catalog = CollectionCatalog::get(opCtx);
        auto newNss = catalog->lookupNSSByUUID(opCtx, expectedUUID);
        if (newNss && newNss != expectedNss) {
            PlanYieldPolicy::throwCollectionRenamedError(expectedNss, *newNss, expectedUUID);
        } else {
            PlanYieldPolicy::throwCollectionDroppedError(expectedUUID);
        }
    } else {
        if (const auto& newNss = collection->ns(); newNss != expectedNss) {
            // TODO SERVER-31695: Allow queries to survive collection rename, rather than throwing
            // here when a rename has happened during yield.
            PlanYieldPolicy::throwCollectionRenamedError(expectedNss, newNss, expectedUUID);
        } else if (collection->uuid() != expectedUUID) {
            PlanYieldPolicy::throwCollectionDroppedError(expectedUUID);
        }
    }
}

inline void restoreInvalidatedCollection(OperationContext* opCtx,
                                         CollectionPtr const*& referenceToCollectionPtr,
                                         const CollectionPtr* restoredCollectionPtr,
                                         const UUID& expectedUUID,
                                         const NamespaceString& expectedNss) {

    checkRestoredCollection(opCtx, *restoredCollectionPtr, expectedUUID, expectedNss);
    referenceToCollectionPtr = restoredCollectionPtr;
}

inline void restoreInvalidatedCollection(OperationContext* opCtx,
                                         const boost::optional<CollectionAcquisition>& collAcq,
                                         const CollectionPtr*,
                                         const UUID& expectedUUID,
                                         const NamespaceString& expectedNss) {

    const auto& collPtr = collAcq->getCollectionPtr();
    checkRestoredCollection(opCtx, collPtr, expectedUUID, expectedNss);
}

template <class Callable>
void temporarilyYieldCollection(OperationContext* opCtx,
                                const CollectionPtr* collection,
                                Callable whileYieldedCallback) {

    tassert(8375910,
            "Cannot yield inside a write unit of work",
            !shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    collection->yield();
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    opCtx->checkForInterrupt();

    Locker* locker = shard_role_details::getLocker(opCtx);
    Locker::LockSnapshot lockSnapshot;
    locker->saveLockStateAndUnlock(&lockSnapshot);

    // All PlanExecutor resources are now free.
    CurOp::get(opCtx)->yielded();  // Count the yield in the operation's metrics.
    whileYieldedCallback();  // Perform any work that we intended to do while resources are yielded.

    // Keep trying to recover the yielded resources until we succeed or encounter an
    // unrecoverable error.
    for (int attempt = 1; true; ++attempt) {
        try {
            locker->restoreLockState(opCtx, lockSnapshot);
            collection->restore();

            return;
        } catch (const StorageUnavailableException& exception) {
            logAndRecordWriteConflictAndBackoff(opCtx,
                                                attempt,
                                                "query yield"_sd,
                                                exception.reason(),
                                                NamespaceStringOrUUID(NamespaceString::kEmpty));
        }
    }
}

template <class Callable>
void temporarilyYieldCollection(OperationContext* opCtx,
                                const CollectionAcquisition& acquisition,
                                Callable whileYieldedCallback) {
    opCtx->checkForInterrupt();

    auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext(opCtx);
    ScopeGuard yieldFailedScopeGuard(
        [&] { yieldedTransactionResources.transitionTransactionResourcesToFailedState(opCtx); });

    // All PlanExecutor resources are now free.
    CurOp::get(opCtx)->yielded();  // Count the yield in the operation's metrics.
    whileYieldedCallback();  // Perform any work that we intended to do while resources are yielded.

    yieldFailedScopeGuard.dismiss();

    restoreTransactionResourcesToOperationContext(opCtx, std::move(yieldedTransactionResources));
}

/**
 * Execute the 'writeFunction' callback and then, if it succeeded, execute the 'continuation'
 * function, returning its result. If 'writeFunction' throws an exception that is covered by the
 * 'exceptionRecoveryPolicy' recovery function, instead return the 'PlanProgress' result from the
 * recovery function, without executing the continuation.
 */
template <class WriteFunction, class Continuation>
PlanProgress recoverFromNonFatalWriteException(
    OperationContext* opCtx,
    const ExceptionRecoveryPolicy& exceptionRecoveryPolicy,
    StringData operationName,
    WriteFunction writeFunction,
    Continuation continuation) {
    try {
        writeFunction();
    } catch (ExceptionFor<ErrorCodes::WriteConflict>& exception) {
        recordWriteConflict(opCtx);
        return exceptionRecoveryPolicy.recoverIfPossible(exception);
    } catch (ExceptionFor<ErrorCodes::TemporarilyUnavailable>& exception) {
        if (opCtx->inMultiDocumentTransaction()) {
            convertToWCEAndRethrow(opCtx, operationName, exception);
        }
        return exceptionRecoveryPolicy.recoverIfPossible(exception);
    } catch (ExceptionFor<ErrorCodes::TransactionTooLargeForCache>& exception) {
        return exceptionRecoveryPolicy.recoverIfPossible(exception);
    } catch (ExceptionFor<ErrorCodes::StaleConfig>& exception) {
        if (ShardVersion::isPlacementVersionIgnored(exception->getVersionReceived()) &&
            exception->getCriticalSectionSignal()) {
            // When the 'StaleConfig' exception is the result of an ongoing critical section (and
            // the placement version is IGNORED), we may attempt to resume query execution with a
            // newer shard configuration after the critical section is finished.
            return exceptionRecoveryPolicy.recoverIfPossible(exception);
        }

        // All other 'StaleConfig' exceptions are query fatal errors intended to propagate to the
        // mongos that issued the write and that is responsible for retrying it.
        throw;
    }

    return continuation();
}

/**
 * A document iterator that uses a collection's _id index to iterate over documents in a collection
 * that match a simple equality predicate on their _id field. The _id index is required to be
 * unique, so the iterator will produce at most one document.
 *
 * The iterator owns the resources associated with the collection it iterates.
 *
 * TODO SERVER-76397: The CollectionType template parameter allows the iterator to hold collection
 * resources as either a <const CollectionPtr*> or a <const CollectionAcquisition>. Once
 * PlanExecutors use CollectionAcquisition exclusively, this parameter will no longer need to be
 * templated.
 */
template <class CollectionType>
class IdLookupViaIndex {
public:
    using CollectionTypeChoice = CollectionType;

    IdLookupViaIndex(const BSONObj& queryFilter) : _queryFilter(queryFilter.getOwned()) {}

    void open(OperationContext* opCtx, CollectionType collection, IteratorStats* stats) {
        _indexCatalogEntry =
            IdLookupViaIndex::getIndexCatalogEntryForIdIndex(opCtx, accessCollection(collection));
        _collection = std::move(collection);
        _collectionUUID = accessCollection(unwrapCollection(_collection)).uuid();
        _catalogEpoch = CollectionCatalog::get(opCtx)->getEpoch();

        _stats = stats;
        _stats->setStageName("EXPRESS_IXSCAN"_sd);
        _stats->setIndexName(IndexConstants::kIdIndexName);
        _stats->setIndexKeyPattern("{ _id: 1 }"_sd);
    }

    template <class Continuation>
    PlanProgress consumeOne(OperationContext* opCtx, Continuation continuation) {
        const auto& collection = unwrapCollection(_collection);

        if (_exhausted) {
            return Exhausted();
        }

        tassert(8375902,
                "Id lookup query filter must contain a single field",
                _queryFilter.nFields() == 1);
        auto rid = _indexCatalogEntry->accessMethod()->asSortedData()->findSingle(
            opCtx,
            *shard_role_details::getRecoveryUnit(opCtx),
            accessCollectionPtr(collection),
            _indexCatalogEntry,
            _queryFilter);
        if (rid.isNull()) {
            _exhausted = true;
            return Exhausted();
        }
        _stats->incNumKeysExamined(1);

        Snapshotted<BSONObj> obj;
        bool found = accessCollection(collection).findDoc(opCtx, rid, &obj);
        if (!found) {
            logRecordNotFound(opCtx,
                              rid,
                              _queryFilter,
                              _indexCatalogEntry->descriptor()->keyPattern(),
                              accessCollection(collection).ns());
            _exhausted = true;
            return Exhausted();
        }

        _stats->incNumDocumentsFetched(1);

        auto progress = continuation(collection, std::move(rid), std::move(obj));

        // Only advance the iterator if the continuation completely processed its item, as indicated
        // by its return value.
        if (isSuccessfulResult(progress)) {
            _exhausted = true;
            return Exhausted{};
        }

        return progress;
    }

    bool exhausted() const {
        return _exhausted;
    };

    void releaseResources() {
        prepareForCollectionInvalidation(_collection);
        _indexCatalogEntry = nullptr;
    }

    void restoreResources(OperationContext* opCtx,
                          const CollectionPtr* collection,
                          const NamespaceString& nss) {
        // Note that this can be called with collection pointing to
        // nullptr in cases where executor has a CollectionAcquisition instead
        // of a CollectionPtr, so handle it carefully.
        restoreInvalidatedCollection(opCtx, _collection, collection, *_collectionUUID, nss);
        uassert(ErrorCodes::QueryPlanKilled,
                "the catalog was closed and reopened",
                CollectionCatalog::get(opCtx)->getEpoch() == _catalogEpoch);
        const auto& coll = unwrapCollection(_collection);
        _indexCatalogEntry =
            IdLookupViaIndex::getIndexCatalogEntryForIdIndex(opCtx, accessCollection(coll));
    }

    template <class Callable>
    void temporarilyReleaseResourcesAndYield(OperationContext* opCtx,
                                             Callable whileYieldedCallback) {
        const auto& collection = unwrapCollection(_collection);
        releaseResources();
        temporarilyYieldCollection(opCtx, collection, std::move(whileYieldedCallback));
        restoreResources(
            opCtx, &accessCollectionPtr(collection), accessCollection(collection).ns());
    }

private:
    static const IndexCatalogEntry* getIndexCatalogEntryForIdIndex(OperationContext* opCtx,
                                                                   const Collection& collection) {
        const IndexCatalog* catalog = collection.getIndexCatalog();
        const IndexDescriptor* desc = catalog->findIdIndex(opCtx);
        tassert(8884401, "Missing _id index on non-clustered collection", desc);

        return catalog->getEntry(desc);
    }

    BSONObj _queryFilter;  // Owned BSON.

    typename WrapInOptionalIfNeeded<CollectionType>::type _collection{};
    boost::optional<UUID> _collectionUUID;
    uint64_t _catalogEpoch{0};
    const IndexCatalogEntry* _indexCatalogEntry{nullptr};  // Unowned.
    IteratorStats* _stats{nullptr};
    bool _exhausted{false};
};

/**
 * A document iterator for collections clustered by the _id field that directly iterates documents
 * matching a simple equality predicate on their _id field. The _id field is required to be unique,
 * so the iterator will produce at most one document.
 *
 * The iterator owns the resources associated with the collection it iterates.
 *
 * TODO SERVER-76397: The CollectionType template parameter allows the iterator to hold collection
 * resources as either a <const CollectionPtr*> or a <const CollectionAcquisition>. Once
 * PlanExecutors use CollectionAcquisition exclusively, this class will no longer need to be
 * templated.
 */
template <class CollectionType>
class IdLookupOnClusteredCollection {
public:
    using CollectionTypeChoice = CollectionType;

    IdLookupOnClusteredCollection(const BSONObj& queryFilter)
        : _queryFilter(queryFilter.getOwned()) {}

    void open(OperationContext* opCtx, CollectionType collection, IteratorStats* stats) {
        _collection = std::move(collection);
        _collectionUUID = accessCollection(unwrapCollection(_collection)).uuid();
        _catalogEpoch = CollectionCatalog::get(opCtx)->getEpoch();

        _stats = stats;
        _stats->setStageName("EXPRESS_CLUSTERED_IXSCAN"_sd);
    }

    template <class Continuation>
    PlanProgress consumeOne(OperationContext* opCtx, Continuation continuation) {
        const auto& collection = unwrapCollection(_collection);

        if (_exhausted) {
            return Exhausted();
        }

        auto rid = record_id_helpers::keyForObj(IndexBoundsBuilder::objFromElement(
            _queryFilter["_id"], accessCollection(collection).getDefaultCollator()));

        Snapshotted<BSONObj> obj;
        bool found = accessCollection(collection).findDoc(opCtx, rid, &obj);
        if (!found) {
            _exhausted = true;
            return Exhausted();
        }
        _stats->incNumDocumentsFetched(1);

        auto progress = continuation(collection, std::move(rid), std::move(obj));

        // Only advance the iterator if the continuation completely processed its item, as indicated
        // by its return value.
        if (isSuccessfulResult(progress)) {
            _exhausted = true;
            return Exhausted();
        }

        return progress;
    }

    bool exhausted() const {
        return _exhausted;
    };

    void releaseResources() {
        prepareForCollectionInvalidation(_collection);
    }

    void restoreResources(OperationContext* opCtx,
                          const CollectionPtr* collection,
                          const NamespaceString& nss) {
        restoreInvalidatedCollection(opCtx, _collection, collection, *_collectionUUID, nss);
        uassert(ErrorCodes::QueryPlanKilled,
                "the catalog was closed and reopened",
                CollectionCatalog::get(opCtx)->getEpoch() == _catalogEpoch);
    }

    template <class Callable>
    void temporarilyReleaseResourcesAndYield(OperationContext* opCtx,
                                             Callable whileYieldedCallback) {
        const auto& collection = unwrapCollection(_collection);
        releaseResources();
        temporarilyYieldCollection(opCtx, collection, std::move(whileYieldedCallback));
        restoreResources(
            opCtx, &accessCollectionPtr(collection), accessCollection(collection).ns());
    }

private:
    BSONObj _queryFilter;  // Owned BSON.

    typename WrapInOptionalIfNeeded<CollectionType>::type _collection{};
    boost::optional<UUID> _collectionUUID;
    uint64_t _catalogEpoch{0};

    bool _exhausted{false};

    IteratorStats* _stats{nullptr};
};

template <typename CollectonType>
struct CreateDocumentFromIndexKey {
    bool operator()(OperationContext* opCtx,
                    const CollectonType& _,
                    const IndexCatalogEntry* indexCatalogEntry,
                    const SortedDataKeyValueView& keyEntry,
                    const projection_ast::Projection* projection,
                    Snapshotted<BSONObj>& obj,
                    IteratorStats* stats) {
        tassert(10399101,
                "Only simple inclusion projections are supported",
                projection && projection->isSimple() && projection->isInclusionOnly());

        StringSet projFields{projection->getRequiredFields().begin(),
                             projection->getRequiredFields().end()};

        const auto& keyPattern = indexCatalogEntry->descriptor()->keyPattern();
        auto dehydratedKey = key_string::toBson(keyEntry.getKeyStringWithoutRecordIdView(),
                                                Ordering::make(keyPattern),
                                                keyEntry.getTypeBitsView(),
                                                keyEntry.getVersion());

        BSONObjBuilder bob;
        BSONObjIterator keyIter(keyPattern);
        BSONObjIterator valueIter(dehydratedKey);

        while (keyIter.more() && valueIter.more()) {
            StringData fieldName = keyIter.next().fieldNameStringData();
            auto nextValue = valueIter.next();

            // Erase the element to support indexes with duplicate fields.
            if (projFields.erase(fieldName) > 0) {
                bob.appendAs(nextValue, fieldName);
                if (projFields.empty()) {
                    break;
                }
            }
        }

        tassert(10399102, "Selected index did not cover the projection", projFields.empty());

        obj.setValue(bob.obj());
        return true;
    }
};

template <typename CollectonType>
struct FetchFromCollectionCallback {
    bool operator()(OperationContext* opCtx,
                    const CollectonType& collection,
                    const IndexCatalogEntry* _,
                    const SortedDataKeyValueView& keyEntry,
                    const projection_ast::Projection* projection,
                    Snapshotted<BSONObj>& obj,
                    IteratorStats* stats) {
        auto rid = keyEntry.getRecordId();
        tassert(8884402, "Index entry with null record id", rid && !rid->isNull());
        // Projection is applied later by the executor.
        bool found = accessCollection(collection).findDoc(opCtx, *rid, &obj);
        stats->incNumDocumentsFetched(found);
        return found;
    }
};

/**
 * A document iterator that uses an arbitrary index to iterate over documents in a collection that
 * match a simple equality predicate on the first field in the index key pattern. There is no
 * uniqueness requirement for the queried field, and this iterator can produce multiple matching
 * documents.
 *
 * The iterator owns the resources associated with the collection it iterates.
 *
 * TODO SERVER-76397: The CollectionType template parameter allows the iterator to hold collection
 * resources as either a <const CollectionPtr*> or a <const CollectionAcquisition>. Once
 * PlanExecutors use CollectionAcquisition exclusively, this class will no longer need to be
 * templated.
 */
template <class CollectionType, class FetchCallback>
class LookupViaUserIndex {
public:
    using CollectionTypeChoice = CollectionType;

    LookupViaUserIndex(const BSONElement& filterValue,
                       std::string indexIdent,
                       std::string indexName,
                       const CollatorInterface* collator,
                       const projection_ast::Projection* projection)
        : _filterValue(filterValue),
          _indexIdent(std::move(indexIdent)),
          _indexName(std::move(indexName)),
          _collator(collator),
          _projection(projection) {}

    void open(OperationContext* opCtx, CollectionType collection, IteratorStats* stats) {
        _indexCatalogEntry = LookupViaUserIndex::getIndexCatalogEntryForUserIndex(
            opCtx, accessCollection(collection), _indexIdent, _indexName);
        _collection = std::move(collection);
        _collectionUUID = accessCollection(unwrapCollection(_collection)).uuid();
        _catalogEpoch = CollectionCatalog::get(opCtx)->getEpoch();

        _stats = stats;
        _stats->setStageName("EXPRESS_IXSCAN"_sd);
        _stats->setIndexName(_indexName);
        _stats->setIndexKeyPattern(
            KeyPattern::toString(_indexCatalogEntry->descriptor()->keyPattern()));
        if constexpr (std::is_same_v<FetchCallback, CreateDocumentFromIndexKey<CollectionType>>) {
            _stats->setProjectionCovered(true);
        }
    }

    template <class Continuation>
    PlanProgress consumeOne(OperationContext* opCtx, Continuation continuation) {
        const auto& collection = unwrapCollection(_collection);

        if (_exhausted) {
            return Exhausted();
        }

        // Build the start and end bounds for the equality by appending a fully-open bound for each
        // remaining field in the compound index.
        BSONObjBuilder startBob, endBob;
        CollationIndexKey::collationAwareIndexKeyAppend(_filterValue, _collator, &startBob);
        CollationIndexKey::collationAwareIndexKeyAppend(_filterValue, _collator, &endBob);
        auto desc = _indexCatalogEntry->descriptor();
        for (int i = 1; i < desc->getNumFields(); ++i) {
            if (desc->ordering().get(i) == 1) {
                startBob.appendMinKey("");
                endBob.appendMaxKey("");
            } else {
                startBob.appendMaxKey("");
                endBob.appendMinKey("");
            }
        }
        auto startKey = startBob.obj();
        auto endKey = endBob.obj();

        // Now seek to the first matching key in the index.
        auto sortedAccessMethod = _indexCatalogEntry->accessMethod()->asSortedData();
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
        auto indexCursor = sortedAccessMethod->newCursor(opCtx, ru, true /* forward */);
        indexCursor->setEndPosition(endKey, true /* endKeyInclusive */);
        key_string::Builder builder(
            sortedAccessMethod->getSortedDataInterface()->getKeyStringVersion());
        auto keyStringForSeek = IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
            startKey,
            sortedAccessMethod->getSortedDataInterface()->getOrdering(),
            true /* forward */,
            true /* startKeyInclusive */,
            builder);

        auto keyEntry = indexCursor->seekForKeyValueView(ru, keyStringForSeek);
        if (keyEntry.isEmpty()) {
            _exhausted = true;
            return Exhausted();
        }
        _stats->incNumKeysExamined(1);

        Snapshotted<BSONObj> obj;
        bool found = FetchCallback{}(
            opCtx, collection, _indexCatalogEntry, keyEntry, _projection, obj, _stats);
        if (!found) {
            const auto& keyPattern = _indexCatalogEntry->descriptor()->keyPattern();
            auto dehydratedKp = key_string::toBson(keyEntry.getKeyStringWithoutRecordIdView(),
                                                   Ordering::make(keyPattern),
                                                   keyEntry.getTypeBitsView(),
                                                   keyEntry.getVersion());

            logRecordNotFound(opCtx,
                              *keyEntry.getRecordId(),
                              IndexKeyEntry::rehydrateKey(keyPattern, dehydratedKp),
                              keyPattern,
                              accessCollection(collection).ns());
            return Ready();
        }

        auto progress = continuation(collection, *keyEntry.getRecordId(), std::move(obj));

        // Only advance the iterator if the continuation completely processed its item, as indicated
        // by its return value.
        if (isSuccessfulResult(progress)) {
            _exhausted = true;
            return Exhausted{};
        }

        return progress;
    }

    bool exhausted() const {
        return _exhausted;
    };

    void releaseResources() {
        prepareForCollectionInvalidation(_collection);
        _indexCatalogEntry = nullptr;
    }

    void restoreResources(OperationContext* opCtx,
                          const CollectionPtr* collection,
                          const NamespaceString& nss) {
        // Note that this can be called with collection pointing to
        // nullptr in cases where executor has a CollectionAcquisition instead
        // of a CollectionPtr, so handle it carefully.
        restoreInvalidatedCollection(opCtx, _collection, collection, *_collectionUUID, nss);
        uassert(ErrorCodes::QueryPlanKilled,
                "the catalog was closed and reopened",
                CollectionCatalog::get(opCtx)->getEpoch() == _catalogEpoch);
        const auto& coll = unwrapCollection(_collection);
        _indexCatalogEntry = LookupViaUserIndex::getIndexCatalogEntryForUserIndex(
            opCtx, accessCollection(coll), _indexIdent, _indexName);
    }

    template <class Callable>
    void temporarilyReleaseResourcesAndYield(OperationContext* opCtx,
                                             Callable whileYieldedCallback) {
        const auto& collection = unwrapCollection(_collection);
        releaseResources();
        temporarilyYieldCollection(opCtx, collection, std::move(whileYieldedCallback));
        restoreResources(
            opCtx, &accessCollectionPtr(collection), accessCollection(collection).ns());
    }

private:
    static const IndexCatalogEntry* getIndexCatalogEntryForUserIndex(OperationContext* opCtx,
                                                                     const Collection& collection,
                                                                     const std::string& indexIdent,
                                                                     const std::string& indexName) {
        const IndexCatalog* catalog = collection.getIndexCatalog();
        const IndexDescriptor* desc = catalog->findIndexByIdent(opCtx, indexIdent);
        uassert(ErrorCodes::QueryPlanKilled,
                fmt::format("query plan killed :: index {} dropped", indexName),
                desc);

        return catalog->getEntry(desc);
    }

    BSONElement _filterValue;  // Unowned BSON.
    const std::string _indexIdent;
    const std::string _indexName;

    typename WrapInOptionalIfNeeded<CollectionType>::type _collection{};
    boost::optional<UUID> _collectionUUID;
    uint64_t _catalogEpoch{0};
    const IndexCatalogEntry* _indexCatalogEntry{nullptr};  // Unowned.

    const CollatorInterface* _collator;             // Owned by the query's ExpressionContext.
    const projection_ast::Projection* _projection;  // Owned by the CanonicalQuery.

    bool _exhausted{false};

    IteratorStats* _stats{nullptr};
};

class NoShardFilter {};

template <class Continuation>
PlanProgress applyShardFilter(NoShardFilter&,
                              const Snapshotted<BSONObj>&,
                              const NamespaceString&,
                              StringData,
                              Continuation continuation) {
    bool shouldWriteToOrphan = false;
    return continuation(shouldWriteToOrphan);
}

template <class Continuation>
PlanProgress applyShardFilter(ScopedCollectionFilter& collectionFilter,
                              const Snapshotted<BSONObj>& obj,
                              const NamespaceString&,
                              StringData,
                              Continuation continuation) {
    bool accepted = [&]() {
        if (!collectionFilter.isSharded()) {
            return true;
        }

        auto shardKey = collectionFilter.getShardKeyPattern().extractShardKeyFromDoc(obj.value());
        return !shardKey.isEmpty() && collectionFilter.keyBelongsToMe(shardKey);
    }();

    if (accepted) {
        bool shouldWriteToOrphan = false;
        return continuation(shouldWriteToOrphan);
    } else {
        // Indicate that we have made progress on plan execution but have not produced a document.
        return Ready();
    }
}

void releaseShardFilterResources(ScopedCollectionFilter&);
void restoreShardFilterResources(ScopedCollectionFilter&);

void releaseShardFilterResources(NoShardFilter&);
void restoreShardFilterResources(NoShardFilter&);

void releaseShardFilterResources(write_stage_common::PreWriteFilter& preWriteFilter);
void restoreShardFilterResources(write_stage_common::PreWriteFilter& preWriteFilter);

template <class Continuation>
PlanProgress applyShardFilter(write_stage_common::PreWriteFilter& preWriteFilter,
                              const Snapshotted<BSONObj>& obj,
                              const NamespaceString& nss,
                              StringData operationName,
                              Continuation continuation) {
    boost::optional<SharedSemiFuture<void>> criticalSectionSignal;
    auto [filterStatus, shouldWriteToOrphan] =
        preWriteFilter.checkIfNotWritable(Document(obj.value()),
                                          operationName,
                                          nss,
                                          [&](const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
                                              criticalSectionSignal =
                                                  ex->getCriticalSectionSignal();
                                          });

    if (!filterStatus) {
        return continuation(shouldWriteToOrphan);
    } else if (*filterStatus == PlanStage::NEED_TIME) {
        // Reject this document but indicate that we have made progress on plan execution.
        return Ready();
    } else if (*filterStatus == PlanStage::NEED_YIELD) {
        if (criticalSectionSignal) {
            return WaitingForCondition(std::move(*criticalSectionSignal));
        } else {
            return WaitingForYield();
        }
    } else {
        MONGO_UNREACHABLE_TASSERT(8375900);
    }
}

const char idFieldName[] = "_id";
const FieldRef idFieldRef(idFieldName);

class UpdateOperation {
public:
    static constexpr StringData name = "update"_sd;

    UpdateOperation(UpdateDriver* updateDriver,
                    bool isUserInitiatedWrite,
                    const UpdateRequest* request)
        : _updateDriver(updateDriver),
          _returnDocs(request->getReturnDocs()),
          _stmtIds(request->getStmtIds()),
          _allowShardKeyUpdatesWithoutFullShardKeyInQuery(
              request->getAllowShardKeyUpdatesWithoutFullShardKeyInQuery()),
          _isUserInitiatedWrite(isUserInitiatedWrite),
          _isExplain(request->getIsExplain()),
          _isMulti(request->isMulti()),
          _source(request->source()),
          _sampleId(request->getSampleId()) {

        tassert(8375904, "Upserts not supported in Express.", !request->isUpsert());
    }

    void open(WriteOperationStats* stats) {
        _stats = stats;
        _stats->setIsModUpdate(_updateDriver->type() == UpdateDriver::UpdateType::kOperator);
        _stats->setStageName("EXPRESS_UPDATE");
    }

    template <class Continuation>
    PlanProgress write(OperationContext* opCtx,
                       const CollectionAcquisition& collection,
                       const ExceptionRecoveryPolicy& exceptionRecoveryPolicy,
                       const RecordId& rid,
                       Snapshotted<BSONObj> obj,
                       bool shouldWriteToOrphan,
                       Continuation continuation) {
        // This should be impossible, because an ExpressPlan does not release a snapshot from the
        // time it reads a record id to the time it has finished all operations on the associated
        // document.
        tassert(8375903,
                "Cannot update document that is not from the current snapshot",
                shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId() == obj.snapshotId());

        BSONObj newObj;
        return recoverFromNonFatalWriteException(
            opCtx,
            exceptionRecoveryPolicy,
            UpdateOperation::name,
            [&]() {
                newObj = updateById(opCtx, collection, obj, rid, shouldWriteToOrphan);
                if (_returnDocs == UpdateRequest::ReturnDocOption::RETURN_OLD) {
                    newObj = obj.value();
                }
                _stats->setContainsDotsAndDollarsField(
                    _updateDriver->containsDotsAndDollarsField());
            },
            [&]() -> PlanProgress {
                // This continuation gets called after the write operation succeeds.
                size_t numDocsMatched = 1;
                _stats->incUpdatedStats(numDocsMatched);

                if (_returnDocs == UpdateRequest::ReturnDocOption::RETURN_NONE) {
                    return Ready{};
                } else {
                    return continuation(std::move(newObj));
                }
            });
    }

    BSONObj updateById(OperationContext* opCtx,
                       const CollectionAcquisition& collection,
                       const Snapshotted<BSONObj>& oldObj,
                       const RecordId& rid,
                       bool shouldWriteToOrphan) const {
        BSONObj logObj;
        bool docWasModified = false;
        FieldRefSet immutablePaths;
        Status status = Status::OK();

        mutablebson::Document doc(oldObj.value(),
                                  (collection.getCollectionPtr()->updateWithDamagesSupported()
                                       ? mutablebson::Document::kInPlaceEnabled
                                       : mutablebson::Document::kInPlaceDisabled));
        DamageVector damages;


        if (_isUserInitiatedWrite) {
            const auto& collDesc = collection.getShardingDescription();
            if (collDesc.isSharded() && !OperationShardingState::isComingFromRouter(opCtx)) {
                immutablePaths.fillFrom(collDesc.getKeyPatternFields());
            }
            immutablePaths.keepShortest(&idFieldRef);
        }

        // positional updates would not be simple _id queries, so match details should never be
        // needed.
        tassert(8375905,
                "Positional updates not allowed in Express",
                !_updateDriver->needMatchDetails());

        status = _updateDriver->update(opCtx,
                                       StringData(),
                                       &doc,
                                       _isUserInitiatedWrite,
                                       immutablePaths,
                                       false, /* isUpsert */
                                       &logObj,
                                       &docWasModified);
        uassertStatusOK(status);

        // Skip adding _id field if the collection is capped (since capped collection documents can
        // neither grow nor shrink).
        const auto createIdField = !collection.getCollectionPtr()->isCapped();
        // Ensure _id is first if it exists, and generate a new OID if appropriate.
        update::ensureIdFieldIsFirst(&doc, createIdField);

        const char* source = nullptr;
        const bool inPlace = doc.getInPlaceUpdates(&damages, &source);

        if (inPlace && damages.empty()) {
            // A modifier didn't notice that it was really a no-op during its 'prepare' phase. That
            // represents a missed optimization, but we still shouldn't do any real work. Toggle
            // 'docWasModified' to 'false'.
            //
            // Currently, an example of this is '{ $push : { x : {$each: [], $sort: 1} } }' when the
            // 'x' array exists and is already sorted.
            docWasModified = false;
        }

        if (!docWasModified) {
            return oldObj.value();
        }

        // Prepare to modify the document
        CollectionUpdateArgs args{oldObj.value()};
        args.update = logObj;
        if (_isUserInitiatedWrite) {
            const auto& collDesc = collection.getShardingDescription();
            args.criteria = collDesc.extractDocumentKey(oldObj.value());
        } else {
            const auto docId = oldObj.value()["_id"_sd];
            args.criteria = docId ? docId.wrap() : oldObj.value();
        }
        uassert(8375909,
                "Multi-update operations require all documents to have an '_id' field",
                !_isMulti || args.criteria.hasField("_id"_sd));

        args.source = shouldWriteToOrphan ? OperationSource::kFromMigrate : _source;
        args.stmtIds = _stmtIds;
        args.sampleId = _sampleId;

        if (_returnDocs == UpdateRequest::ReturnDocOption::RETURN_NEW) {
            args.storeDocOption = CollectionUpdateArgs::StoreDocOption::PostImage;
        } else if (_returnDocs == UpdateRequest::ReturnDocOption::RETURN_OLD) {
            args.storeDocOption = CollectionUpdateArgs::StoreDocOption::PreImage;
        } else {
            args.storeDocOption = CollectionUpdateArgs::StoreDocOption::None;
        }

        args.mustCheckExistenceForInsertOperations =
            _updateDriver->getUpdateExecutor()->getCheckExistenceForDiffInsertOperations();

        args.retryableWrite = write_stage_common::isRetryableWrite(opCtx);

        BSONObj newObj = doc.getObject();

        if (inPlace) {
            if (!_isExplain) {
                if (_isUserInitiatedWrite) {
                    ShardingChecksForUpdate scfu(collection,
                                                 _allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                                                 _isMulti,
                                                 nullptr);
                    scfu.checkUpdateChangesShardKeyFields(opCtx, doc, boost::none, oldObj);
                }

                auto diff = update_oplog_entry::extractDiffFromOplogEntry(logObj);
                WriteUnitOfWork wunit(opCtx);
                bool indexesAffected = false;
                newObj = uassertStatusOK(collection_internal::updateDocumentWithDamages(
                    opCtx,
                    collection.getCollectionPtr(),
                    rid,
                    oldObj,
                    source,
                    damages,
                    diff.has_value() ? &*diff : collection_internal::kUpdateAllIndexes,
                    &indexesAffected,
                    &CurOp::get(opCtx)->debug(),
                    &args));

                tassert(8375906,
                        "Old and new snapshot ids must not change after update",
                        oldObj.snapshotId() ==
                            shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId());
                wunit.commit();
            }
            _stats->incDocsUpdated(1);  // explains are also treated as though they wrote
        } else {
            newObj = doc.getObject();
            if (!DocumentValidationSettings::get(opCtx).isInternalValidationDisabled()) {
                uassert(ErrorCodes::BSONObjectTooLarge,
                        str::stream() << "Resulting document after update is larger than "
                                      << BSONObjMaxUserSize,
                        newObj.objsize() <= BSONObjMaxUserSize);
            }

            if (!_isExplain) {
                if (_isUserInitiatedWrite) {
                    ShardingChecksForUpdate scfu(collection,
                                                 _allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                                                 _isMulti,
                                                 nullptr);
                    scfu.checkUpdateChangesShardKeyFields(opCtx, doc, newObj, oldObj);
                }

                bool indexesAffected = false;
                auto diff = update_oplog_entry::extractDiffFromOplogEntry(logObj);
                WriteUnitOfWork wunit(opCtx);
                collection_internal::updateDocument(
                    opCtx,
                    collection.getCollectionPtr(),
                    rid,
                    oldObj,
                    newObj,
                    diff.has_value() ? &*diff : collection_internal::kUpdateAllIndexes,
                    &indexesAffected,
                    &CurOp::get(opCtx)->debug(),
                    &args);
                tassert(8375907,
                        "Old and new snapshot ids must not change after update",
                        oldObj.snapshotId() ==
                            shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId());
                wunit.commit();
            }
            _stats->incDocsUpdated(1);  // explains are also treated as though they wrote
        }

        // The general-purpose update stage tracks records ids of all updated documents so that it
        // can avoid the Halloween problem, but that is not necessary for this stage, because it
        // never writes more than one document.

        return newObj;
    }

private:
    UpdateDriver* _updateDriver;
    const UpdateRequest::ReturnDocOption _returnDocs;
    const std::vector<StmtId> _stmtIds;
    const OptionalBool _allowShardKeyUpdatesWithoutFullShardKeyInQuery;
    const bool _isUserInitiatedWrite;
    const bool _isExplain;
    const bool _isMulti;
    const OperationSource _source;
    const boost::optional<UUID> _sampleId;

    WriteOperationStats* _stats{nullptr};
};

class DeleteOperation {
public:
    DeleteOperation(StmtId stmtId, bool fromMigrate, bool returnDeleted)
        : _stmtId(stmtId), _fromMigrate(fromMigrate), _returnDeleted(returnDeleted) {}

    void open(WriteOperationStats* stats) {
        _stats = stats;
        _stats->setStageName("EXPRESS_DELETE");
    }

    template <class Continuation>
    PlanProgress write(OperationContext* opCtx,
                       const CollectionAcquisition& collection,
                       const ExceptionRecoveryPolicy& exceptionRecoveryPolicy,
                       const RecordId& rid,
                       Snapshotted<BSONObj> obj,
                       bool shouldWriteToOrphan,
                       Continuation continuation) {
        // This should be impossible, because an ExpressPlan does not release a snapshot from the
        // time it reads a record id to the time it has finished all operations on the associated
        // document.
        tassert(5555515,
                "Cannot delete document that is not from the current snapshot",
                shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId() == obj.snapshotId());

        return recoverFromNonFatalWriteException(
            opCtx,
            exceptionRecoveryPolicy,
            DeleteOperation::name,
            [&]() {
                bool noWarn = false;
                WriteUnitOfWork wunit(opCtx);
                collection_internal::deleteDocument(opCtx,
                                                    collection.getCollectionPtr(),
                                                    obj,
                                                    _stmtId,
                                                    rid,
                                                    &CurOp::get(opCtx)->debug(),
                                                    _fromMigrate || shouldWriteToOrphan,
                                                    noWarn,
                                                    _returnDeleted
                                                        ? collection_internal::StoreDeletedDoc::On
                                                        : collection_internal::StoreDeletedDoc::Off,
                                                    CheckRecordId::Off,
                                                    write_stage_common::isRetryableWrite(opCtx)
                                                        ? collection_internal::RetryableWrite::kYes
                                                        : collection_internal::RetryableWrite::kNo);
                wunit.commit();
            },
            [&]() -> PlanProgress {
                // This continuation gets called after the write operation succeeds.
                size_t numDocsDeleted = 1;
                _stats->incDeletedStats(numDocsDeleted);

                if (_returnDeleted) {
                    return continuation(std::move(obj.value()));
                } else {
                    return Ready{};
                }
            });
    }

    static constexpr StringData name = "delete"_sd;

private:
    StmtId _stmtId;
    bool _fromMigrate;
    bool _returnDeleted;

    WriteOperationStats* _stats{nullptr};
};

class DummyDeleteOperationForExplain {
public:
    DummyDeleteOperationForExplain(bool returnDeleted) : _returnDeleted(returnDeleted) {}

    void open(WriteOperationStats* stats) {
        _stats = stats;
        _stats->setStageName("EXPRESS_DELETE");
    }

    template <class Continuation>
    PlanProgress write(OperationContext* opCtx,
                       const CollectionAcquisition& collection,
                       const ExceptionRecoveryPolicy& exceptionRecoveryPolicy,
                       const RecordId& rid,
                       Snapshotted<BSONObj> obj,
                       bool shouldWriteToOrphan,
                       Continuation continuation) {
        size_t numDocsDeleted = 1;
        _stats->incDeletedStats(numDocsDeleted);

        if (_returnDeleted) {
            return continuation(std::move(obj.value()));
        } else {
            return Ready{};
        }
    }

    static constexpr StringData name = "delete"_sd;

private:
    bool _returnDeleted;

    WriteOperationStats* _stats{nullptr};
};

class NoWriteOperation {
public:
    static constexpr StringData name = "nowriteop"_sd;

    void open(WriteOperationStats*) {}

    template <class Continuation, class CollectionType>
    PlanProgress write(OperationContext*,
                       const CollectionType&,
                       const ExceptionRecoveryPolicy& exceptionRecoveryPolicy,
                       const RecordId&,
                       Snapshotted<BSONObj> obj,
                       bool,
                       Continuation continuation) {
        // Great job writing everyone! Let's grab an early lunch.
        return continuation(std::move(obj.value()));
    }
};

class IdentityProjection {};

template <class Continuation>
PlanProgress applyProjection(IdentityProjection, BSONObj obj, Continuation continuation) {
    // Presto change-o!
    return continuation(std::move(obj));
}

template <class Continuation>
auto applyProjection(const projection_ast::Projection* projection,
                     BSONObj obj,
                     Continuation continuation) {
    auto projType = projection->type();
    if (projType == projection_ast::ProjectType::kInclusion) {
        return continuation(
            ProjectionStageSimple::transform(obj, projection->getRequiredFields(), projType));
    } else {
        return continuation(
            ProjectionStageSimple::transform(obj, projection->getExcludedPaths(), projType));
    }
}

/**
 * ExpressPlan is a streamlined execution engine that supports one specific sequence of query
 * stages.
 *   - Document iterator -> optional shard filter -> optional write -> optional projection
 *
 * Each stage has multiple implementations which are specified as template parameters. Optional
 * stages can be disabled by specifying a no-op implementation.
 */
template <class IteratorChoice,
          class WriteOperationChoice,
          class ShardFilterChoice,
          class ProjectionChoice>
class ExpressPlan {
public:
    using CollectionType = typename IteratorChoice::CollectionTypeChoice;

    ExpressPlan(IteratorChoice iterator,
                WriteOperationChoice writeOperation,
                ShardFilterChoice shardFilter,
                ProjectionChoice projection)
        : _iterator(std::move(iterator)),
          _writeOperation(std::move(writeOperation)),
          _shardFilter(std::move(shardFilter)),
          _projection(std::move(projection)) {}

    void open(OperationContext* opCtx,
              CollectionType collection,
              const ExceptionRecoveryPolicy* exceptionRecoveryPolicy,
              PlanStats* planStats,
              IteratorStats* iteratorStats,
              WriteOperationStats* writeOperationStats) {
        _planStats = planStats;
        _iterator.open(opCtx, collection, iteratorStats);
        _exceptionRecoveryPolicy = exceptionRecoveryPolicy;
        _writeOperation.open(writeOperationStats);
    }

    template <class Continuation>
    PlanProgress proceed(OperationContext* opCtx, Continuation continuation) {
        return _iterator.consumeOne(
            opCtx, [&](const auto& collection, RecordId rid, Snapshotted<BSONObj> obj) {
                // Continue execution with one (rid, obj) pair from the iterator.
                return applyShardFilter(
                    _shardFilter,
                    obj,
                    accessCollection(collection).ns(),
                    _writeOperation.name,
                    [&](bool shouldWriteToOrphan) {
                        // Continue execution when the document is accepted by the filter.
                        return _writeOperation.write(
                            opCtx,
                            collection,
                            *_exceptionRecoveryPolicy,
                            rid,
                            std::move(obj),
                            shouldWriteToOrphan,
                            [&](BSONObj outObj) {
                                // Continue execution after the write operation succeeds.
                                return applyProjection(
                                    _projection, std::move(outObj), [&](BSONObj projectionResult) {
                                        // Continue execution with the result of applying the
                                        // projection.
                                        _planStats->incNumResults(1);
                                        return continuation(std::move(rid),
                                                            std::move(projectionResult));
                                    });
                            });
                    });
            });
    }

    void releaseResources() {
        _iterator.releaseResources();
        releaseShardFilterResources(_shardFilter);
    }

    void restoreResources(OperationContext* opCtx,
                          const CollectionPtr* collection,
                          const NamespaceString& nss) {
        _iterator.restoreResources(opCtx, collection, nss);
        restoreShardFilterResources(_shardFilter);
    }

    template <class Callable>
    void temporarilyReleaseResourcesAndYield(OperationContext* opCtx,
                                             Callable whileYieldedCallback) {
        releaseShardFilterResources(_shardFilter);
        _iterator.temporarilyReleaseResourcesAndYield(opCtx, std::move(whileYieldedCallback));
        restoreShardFilterResources(_shardFilter);
    }

    bool exhausted() const {
        return _iterator.exhausted();
    }

private:
    IteratorChoice _iterator;
    WriteOperationChoice _writeOperation;
    ShardFilterChoice _shardFilter;
    ProjectionChoice _projection;
    const ExceptionRecoveryPolicy* _exceptionRecoveryPolicy{nullptr};

    PlanStats* _planStats{nullptr};
};
}  // namespace express
}  // namespace mongo
