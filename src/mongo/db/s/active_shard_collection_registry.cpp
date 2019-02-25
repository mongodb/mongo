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

#include "mongo/platform/basic.h"

#include "mongo/db/s/active_shard_collection_registry.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/service_context.h"

namespace mongo {
namespace {

const auto getRegistry = ServiceContext::declareDecoration<ActiveShardCollectionRegistry>();

bool ActiveShardsvrShardCollectionEqualsNewRequest(const ShardsvrShardCollection& activeRequest,
                                                   const ShardsvrShardCollection& newRequest) {
    if (activeRequest.get_shardsvrShardCollection().get() !=
        newRequest.get_shardsvrShardCollection().get())
        return false;
    if (activeRequest.getKey().woCompare(newRequest.getKey()) != 0)
        return false;
    if (activeRequest.getUnique() != newRequest.getUnique())
        return false;
    if (activeRequest.getNumInitialChunks() != newRequest.getNumInitialChunks())
        return false;
    if ((activeRequest.getCollation() && newRequest.getCollation()) &&
        (activeRequest.getCollation().get().woCompare(newRequest.getCollation().get()) != 0))
        return false;
    if (activeRequest.getGetUUIDfromPrimaryShard() != newRequest.getGetUUIDfromPrimaryShard())
        return false;

    if (activeRequest.getInitialSplitPoints() && newRequest.getInitialSplitPoints()) {
        if (activeRequest.getInitialSplitPoints().get().size() !=
            newRequest.getInitialSplitPoints().get().size()) {
            return false;
        } else {
            for (std::size_t i = 0; i < activeRequest.getInitialSplitPoints().get().size(); i++) {
                if (activeRequest.getInitialSplitPoints().get()[i].woCompare(
                        newRequest.getInitialSplitPoints().get()[i]) != 0)
                    return false;
            }
        }
    }

    return true;
}

}  // namespace

ActiveShardCollectionRegistry::ActiveShardCollectionRegistry() = default;

ActiveShardCollectionRegistry::~ActiveShardCollectionRegistry() {
    invariant(_activeShardCollectionMap.empty());
}

ActiveShardCollectionRegistry& ActiveShardCollectionRegistry::get(ServiceContext* service) {
    return getRegistry(service);
}

ActiveShardCollectionRegistry& ActiveShardCollectionRegistry::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

StatusWith<ScopedShardCollection> ActiveShardCollectionRegistry::registerShardCollection(
    const ShardsvrShardCollection& request) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    std::string nss = request.get_shardsvrShardCollection().get().ns();

    auto iter = _activeShardCollectionMap.find(nss);
    if (iter == _activeShardCollectionMap.end()) {
        auto activeShardCollectionState = std::make_shared<ActiveShardCollectionState>(request);
        _activeShardCollectionMap.try_emplace(nss, activeShardCollectionState);

        return {ScopedShardCollection(
            nss, this, true, activeShardCollectionState->_uuidPromise.getFuture())};
    } else {
        auto activeShardCollectionState = iter->second;

        if (ActiveShardsvrShardCollectionEqualsNewRequest(activeShardCollectionState->activeRequest,
                                                          request)) {
            return {ScopedShardCollection(
                nss, nullptr, false, activeShardCollectionState->_uuidPromise.getFuture())};
        }
        return activeShardCollectionState->constructErrorStatus(request);
    }
}

void ActiveShardCollectionRegistry::_clearShardCollection(std::string nss) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto iter = _activeShardCollectionMap.find(nss);
    invariant(iter != _activeShardCollectionMap.end());
    _activeShardCollectionMap.erase(nss);
}

void ActiveShardCollectionRegistry::_setUUIDOrError(std::string nss,
                                                    StatusWith<boost::optional<UUID>> swUUID) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto iter = _activeShardCollectionMap.find(nss);
    invariant(iter != _activeShardCollectionMap.end());
    auto activeShardCollectionState = iter->second;
    activeShardCollectionState->_uuidPromise.setFromStatusWith(swUUID);
}

Status ActiveShardCollectionRegistry::ActiveShardCollectionState::constructErrorStatus(
    const ShardsvrShardCollection& request) const {
    return {ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Unable to shard collection "
                          << request.get_shardsvrShardCollection().get().ns()
                          << " with arguments:  "
                          << request.toBSON()
                          << " because this shard is currently running shard collection on this "
                          << "collection with arguments: "
                          << activeRequest.toBSON()};
}

ScopedShardCollection::ScopedShardCollection(std::string nss,
                                             ActiveShardCollectionRegistry* registry,
                                             bool shouldExecute,
                                             SharedSemiFuture<boost::optional<UUID>> uuidFuture)
    : _nss(nss),
      _registry(registry),
      _shouldExecute(shouldExecute),
      _uuidFuture(std::move(uuidFuture)) {}

ScopedShardCollection::~ScopedShardCollection() {
    if (_registry && _shouldExecute) {
        _registry->_clearShardCollection(_nss);
    }
}

ScopedShardCollection::ScopedShardCollection(ScopedShardCollection&& other) {
    *this = std::move(other);
}

ScopedShardCollection& ScopedShardCollection::operator=(ScopedShardCollection&& other) {
    if (&other != this) {
        _registry = other._registry;
        other._registry = nullptr;
        _shouldExecute = other._shouldExecute;
        _uuidFuture = std::move(other._uuidFuture);
        _nss = std::move(other._nss);
    }

    return *this;
}

void ScopedShardCollection::emplaceUUID(StatusWith<boost::optional<UUID>> swUUID) {
    invariant(_shouldExecute);
    _registry->_setUUIDOrError(_nss, swUUID);
}

SharedSemiFuture<boost::optional<UUID>> ScopedShardCollection::getUUID() {
    invariant(!_shouldExecute);
    return _uuidFuture;
}

}  // namespace mongo
