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

#include "mongo/db/catalog/collection.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

CollectionPtr CollectionPtr::null;

CollectionPtr::CollectionPtr(const Collection* collection) : _collection(collection) {}

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
        _collection = nullptr;
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

std::pair<std::unique_ptr<CollatorInterface>, ExpressionContext::CollationMatchesDefault>
resolveCollator(OperationContext* opCtx, BSONObj userCollation, const CollectionPtr& collection) {
    if (!collection || !collection->getDefaultCollator()) {
        if (userCollation.isEmpty()) {
            return {nullptr, ExpressionContext::CollationMatchesDefault::kYes};
        } else {
            return {getUserCollator(opCtx, userCollation),
                    // If the user explicitly provided a simple collation, we can still treat it as
                    // 'CollationMatchesDefault::kYes', as no collation and simple collation are
                    // functionally equivalent in the query code.
                    (SimpleBSONObjComparator::kInstance.evaluate(userCollation ==
                                                                 CollationSpec::kSimpleSpec))
                        ? ExpressionContext::CollationMatchesDefault::kYes
                        : ExpressionContext::CollationMatchesDefault::kNo};
        }
    }

    auto defaultCollator = collection->getDefaultCollator()->clone();
    if (userCollation.isEmpty()) {
        return {std::move(defaultCollator), ExpressionContext::CollationMatchesDefault::kYes};
    }
    auto userCollator = getUserCollator(opCtx, userCollation);

    if (CollatorInterface::collatorsMatch(defaultCollator.get(), userCollator.get())) {
        return {std::move(defaultCollator), ExpressionContext::CollationMatchesDefault::kYes};
    } else {
        return {std::move(userCollator), ExpressionContext::CollationMatchesDefault::kNo};
    }
}

}  // namespace mongo
