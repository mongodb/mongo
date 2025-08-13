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

#include "mongo/db/local_catalog/collection.h"

#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {
/**
 * A class that verifies there are no leftover consistent collection once it goes out of scope. Used
 * in conjunction with ConsistentCollection.
 */
class CollectionsInUse {
public:
    CollectionsInUse() = default;

    ~CollectionsInUse() {
        int leftoverCollections = 0;

        for (const auto& [collection, count] : _collections) {
            invariant(count >= 0);
            if (count == 0)
                continue;
            leftoverCollections += count;
        }

        invariant(leftoverCollections == 0,
                  "Server encountered potential use-after free with leftover Collection*. Crashing "
                  "server to prevent undefined errors.");
    }

    void markCollectionAsOpened(const Collection* coll) {
        _collections[coll]++;
    }

    void markCollectionAsReleased(const Collection* coll) {
        _collections[coll]--;
    }

    int getRefCount(const Collection* coll) {
        return _collections[coll];
    }

private:
    stdx::unordered_map<const Collection*, int> _collections;
};

// We only enable the safety guards in debug builds to avoid paying any performance penalty.
#ifdef MONGO_CONFIG_DEBUG_BUILD
const RecoveryUnit::Snapshot::Decoration<CollectionsInUse> collectionsInUse =
    RecoveryUnit::Snapshot::declareDecoration<CollectionsInUse>();
#endif

}  // namespace

#ifdef MONGO_CONFIG_DEBUG_BUILD
ConsistentCollection::~ConsistentCollection() {
    _releaseCollectionIfInSnapshot();
}

ConsistentCollection::ConsistentCollection(OperationContext* opCtx, const Collection* coll)
    : _collection(coll) {
    // If we have no operation context it means we're acting on an owned collection, we can skip the
    // safety checks.
    if (!opCtx) {
        return;
    }
    // If there's no collection there's no need for safety checks.
    if (!coll) {
        return;
    }
    // If the collection is locked then it is safe to assume it is consistent with the snapshot
    // since no modifications can happen to it while we hold the lock or we're the ones making them.
    // We do not proceed with the safety check.
    if (auto locker = shard_role_details::getLocker(opCtx);
        locker->isCollectionLockedForMode(coll->ns(), MODE_IS)) {
        return;
    }

    _snapshot = &shard_role_details::getRecoveryUnit(opCtx)->getSnapshot();
    collectionsInUse(_snapshot).markCollectionAsOpened(_collection);
}

ConsistentCollection::ConsistentCollection(const ConsistentCollection& other)
    : _collection(other._collection), _snapshot(other._snapshot) {
    if (_snapshot) {
        collectionsInUse(_snapshot).markCollectionAsOpened(_collection);
    }
}

ConsistentCollection::ConsistentCollection(ConsistentCollection&& other)
    : _collection(std::exchange(other._collection, nullptr)),
      _snapshot(std::exchange(other._snapshot, nullptr)) {}

ConsistentCollection& ConsistentCollection::operator=(const ConsistentCollection& other) {
    if (this == &other) {
        return *this;
    }
    _releaseCollectionIfInSnapshot();
    _collection = other._collection;
    _snapshot = other._snapshot;
    if (_snapshot) {
        collectionsInUse(_snapshot).markCollectionAsOpened(_collection);
    }
    return *this;
}
ConsistentCollection& ConsistentCollection::operator=(ConsistentCollection&& other) {
    _releaseCollectionIfInSnapshot();
    _collection = std::exchange(other._collection, nullptr);
    _snapshot = std::exchange(other._snapshot, nullptr);
    return *this;
}

int ConsistentCollection::_getRefCount() const {
    if (!_collection) {
        return 0;
    }
    if (!_snapshot)
        return 1;
    return collectionsInUse(_snapshot).getRefCount(_collection);
}

void ConsistentCollection::_releaseCollectionIfInSnapshot() {
    if (_snapshot) {
        collectionsInUse(_snapshot).markCollectionAsReleased(_collection);
    }
}
#endif

CollectionPtr CollectionPtr::null;

CollectionPtr::CollectionPtr(ConsistentCollection collection)
    : _collection(std::move(collection)) {}

CollectionPtr CollectionPtr::CollectionPtr_UNSAFE(const Collection* coll) {
    return CollectionPtr(coll);
}

CollectionPtr::CollectionPtr(Collection* coll) : _collection(ConsistentCollection{nullptr, coll}) {}

CollectionPtr::CollectionPtr(const Collection* coll)
    : _collection(ConsistentCollection{nullptr, coll}) {}

CollectionPtr::CollectionPtr(CollectionPtr&&) = default;
CollectionPtr::~CollectionPtr() {}
CollectionPtr& CollectionPtr::operator=(CollectionPtr&&) = default;

bool CollectionPtr::yieldable() const {
    // We only set the opCtx when this CollectionPtr is yieldable.
    return _opCtx || !_collection;
}

void CollectionPtr::yield() const {
    // Yield if we are yieldable and have a valid collection.
    if (_collection) {
        invariant(_opCtx);
        _yieldedUUID = _collection->uuid();
        _collection = ConsistentCollection{};
    }
    // Enter in 'yielded' state whether or not we held a valid collection pointer.
    _yielded = true;
}
void CollectionPtr::restore() const {
    // Restore from yield if we are yieldable and if we are in 'yielded' state.
    invariant(_opCtx);
    if (_yielded) {
        // We may only do yield restore when we were holding locks that was yielded so we need to
        // refresh from the catalog to make sure we have a valid collection pointer. This call may
        // still return a null collection pointer if the yieldedUUID is boost::none or if the
        // collection no longer exists.
        _collection = _restoreFn(_opCtx, _yieldedUUID);
        _yieldedUUID.reset();
        _yielded = false;
    }
}

void CollectionPtr::setShardKeyPattern(const BSONObj& shardKeyPattern) {
    _shardKeyPattern.emplace(shardKeyPattern.getOwned());
}

const ShardKeyPattern& CollectionPtr::getShardKeyPattern() const {
    invariant(_shardKeyPattern);
    return *_shardKeyPattern;
}

// ----

namespace {
const auto getFactory = ServiceContext::declareDecoration<std::unique_ptr<Collection::Factory>>();
}  // namespace

Collection::Factory* Collection::Factory::get(ServiceContext* service) {
    return getFactory(service).get();
}

Collection::Factory* Collection::Factory::get(OperationContext* opCtx) {
    return getFactory(opCtx->getServiceContext()).get();
}

void Collection::Factory::set(ServiceContext* service,
                              std::unique_ptr<Collection::Factory> newFactory) {
    auto& factory = getFactory(service);
    factory = std::move(newFactory);
}

std::pair<std::unique_ptr<CollatorInterface>, ExpressionContextCollationMatchesDefault>
resolveCollator(OperationContext* opCtx, BSONObj userCollation, const CollectionPtr& collection) {
    if (!collection || !collection->getDefaultCollator()) {
        if (userCollation.isEmpty()) {
            return {nullptr, ExpressionContextCollationMatchesDefault::kYes};
        } else {
            return {getUserCollator(opCtx, userCollation),
                    // If the user explicitly provided a simple collation, we can still treat it as
                    // 'CollationMatchesDefault::kYes', as no collation and simple collation are
                    // functionally equivalent in the query code.
                    (SimpleBSONObjComparator::kInstance.evaluate(userCollation ==
                                                                 CollationSpec::kSimpleSpec))
                        ? ExpressionContextCollationMatchesDefault::kYes
                        : ExpressionContextCollationMatchesDefault::kNo};
        }
    }

    auto defaultCollator = collection->getDefaultCollator()->clone();
    if (userCollation.isEmpty()) {
        return {std::move(defaultCollator), ExpressionContextCollationMatchesDefault::kYes};
    }
    auto userCollator = getUserCollator(opCtx, userCollation);

    if (CollatorInterface::collatorsMatch(defaultCollator.get(), userCollator.get())) {
        return {std::move(defaultCollator), ExpressionContextCollationMatchesDefault::kYes};
    } else {
        return {std::move(userCollator), ExpressionContextCollationMatchesDefault::kNo};
    }
}

}  // namespace mongo
